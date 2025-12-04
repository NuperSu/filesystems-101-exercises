#include <solution.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>

/* ---------------- On-disk EXT2 structures we need ---------------- */

struct ext2_superblock_disk
{
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    int32_t  s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;

    /* The rest of the original ext2 superblock is not needed here. */
} __attribute__((packed));

struct ext2_group_desc_disk
{
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode_disk
{
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

struct ext2_dir_entry_disk
{
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

/* Values for file_type. */
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

/* ----------------------- Small I/O helpers ----------------------- */

static int read_exact_at(int fd, void *buf, size_t count, off_t offset)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = count;

    while (remaining > 0) {
        ssize_t r = pread(fd, p, remaining, offset);
        if (r < 0)
            return -errno;
        if (r == 0)
            return -EPROTO; /* unexpected EOF */
        p += (size_t)r;
        remaining -= (size_t)r;
        offset += (off_t)r;
    }
    return 0;
}

/* Read the superblock and extract basic parameters. */
static int ext2_read_basic_sb(int img,
                              int *block_size_out,
                              uint32_t *first_data_block_out,
                              uint32_t *inodes_per_group_out,
                              uint32_t *inode_size_out,
                              uint32_t *inodes_count_out,
                              uint32_t *blocks_count_out)
{
    struct ext2_superblock_disk sb;
    int r = read_exact_at(img, &sb, sizeof(sb), 1024);
    if (r < 0)
        return r;

    if (sb.s_magic != 0xEF53)
        return -EPROTO;

    /* Compute block size safely: 1024 << s_log_block_size. */
    if (sb.s_log_block_size > 31)
        return -EPROTO;

    uint64_t block_size_u64 = 1024ull << sb.s_log_block_size;
    if (block_size_u64 == 0 || block_size_u64 > (uint64_t)INT_MAX)
        return -EPROTO;

    int block_size = (int)block_size_u64;

    if (sb.s_inodes_per_group == 0)
        return -EPROTO;

    uint32_t inode_size = sb.s_inode_size ? sb.s_inode_size : 128u;
    /* Basic sanity check: the on-disk inode size must be large enough
       to cover the mandatory 128-byte inode structure used by ext2. */
    if (inode_size < 128u)
        return -EPROTO;

    *block_size_out        = block_size;
    *first_data_block_out  = sb.s_first_data_block;
    *inodes_per_group_out  = sb.s_inodes_per_group;
    *inode_size_out        = inode_size;
    *inodes_count_out      = sb.s_inodes_count;
    *blocks_count_out      = sb.s_blocks_count;

    return 0;
}

/* Read inode inode_nr into *inode_out. */
static int ext2_read_inode(int img,
                           int block_size,
                           uint32_t first_data_block,
                           uint32_t inodes_per_group,
                           uint32_t inode_size,
                           int inode_nr,
                           struct ext2_inode_disk *inode_out)
{
    if (inode_nr <= 0)
        return -EINVAL;

    uint32_t ino  = (uint32_t)inode_nr;
    uint32_t idx0 = ino - 1;
    uint32_t group = idx0 / inodes_per_group;
    uint32_t idx   = idx0 % inodes_per_group;

    /* Locate the group descriptor for this inode.  Group descriptors are
       stored starting at the block right after the superblock. */
    off_t gdt_off = (off_t)(first_data_block + 1u) * (off_t)block_size
                    + (off_t)group * (off_t)sizeof(struct ext2_group_desc_disk);

    struct ext2_group_desc_disk gd;
    int r = read_exact_at(img, &gd, sizeof(gd), gdt_off);
    if (r < 0)
        return r;

    if (gd.bg_inode_table == 0)
        return -EPROTO;

    /* Offset of the inode within the inode table. */
    off_t inode_off = (off_t)gd.bg_inode_table * (off_t)block_size
                      + (off_t)idx * (off_t)inode_size;

    size_t to_read = inode_size < (uint32_t)sizeof(*inode_out)
        ? (size_t)inode_size
        : sizeof(*inode_out);

    r = read_exact_at(img, inode_out, to_read, inode_off);
    if (r < 0)
        return r;

    /* Free / unused inode? */
    if (inode_out->i_mode == 0 || inode_out->i_links_count == 0)
        return -ENOENT;

    return 0;
}

/* Map a logical block number to a physical block number using i_block[]. */
static int inode_get_block(int img,
                           int block_size,
                           uint32_t block_count,
                           const struct ext2_inode_disk *inode,
                           uint32_t lblock,
                           uint32_t *blkno)
{
    uint32_t ptrs_per_block = (uint32_t)block_size / sizeof(uint32_t);
    const uint32_t direct_count = 12;
    uint32_t idx = lblock;

    if (ptrs_per_block == 0)
        return -EPROTO;

    /* ---- Direct blocks ---- */
    if (idx < direct_count) {
        uint32_t b = inode->i_block[idx];
        if (b == 0 || b >= block_count)
            return -EPROTO;
        *blkno = b;
        return 0;
    }

    idx -= direct_count;

    /* ---- Single-indirect ---- */
    {
        uint32_t per_sind = ptrs_per_block;
        if (idx < per_sind) {
            uint32_t ind_blk = inode->i_block[12];
            uint32_t *buf;
            int r;

            if (ind_blk == 0 || ind_blk >= block_count)
                return -EPROTO;

            buf = (uint32_t *)malloc((size_t)block_size);
            if (!buf)
                return -ENOMEM;

            r = read_exact_at(img, buf, (size_t)block_size,
                              (off_t)ind_blk * (off_t)block_size);
            if (r < 0) {
                free(buf);
                return r;
            }

            uint32_t b = buf[idx];
            free(buf);

            if (b == 0 || b >= block_count)
                return -EPROTO;

            *blkno = b;
            return 0;
        }

        idx -= per_sind;
    }

    /* ---- Double-indirect ---- */
    {
        uint64_t per_dind = (uint64_t)ptrs_per_block * (uint64_t)ptrs_per_block;
        if (per_dind == 0 || per_dind > UINT32_MAX)
            return -EPROTO;

        if (idx < (uint32_t)per_dind) {
            uint32_t dind_blk = inode->i_block[13];
            uint32_t *outer = NULL;
            uint32_t *inner = NULL;
            int r;

            if (dind_blk == 0 || dind_blk >= block_count)
                return -EPROTO;

            outer = (uint32_t *)malloc((size_t)block_size);
            if (!outer)
                return -ENOMEM;

            r = read_exact_at(img, outer, (size_t)block_size,
                              (off_t)dind_blk * (off_t)block_size);
            if (r < 0) {
                free(outer);
                return r;
            }

            uint32_t outer_index = idx / ptrs_per_block;
            uint32_t inner_index = idx % ptrs_per_block;
            uint32_t ind_blk = outer[outer_index];

            if (ind_blk == 0 || ind_blk >= block_count) {
                free(outer);
                return -EPROTO;
            }

            inner = (uint32_t *)malloc((size_t)block_size);
            if (!inner) {
                free(outer);
                return -ENOMEM;
            }

            r = read_exact_at(img, inner, (size_t)block_size,
                              (off_t)ind_blk * (off_t)block_size);
            if (r < 0) {
                free(inner);
                free(outer);
                return r;
            }

            uint32_t b = inner[inner_index];

            free(inner);
            free(outer);

            if (b == 0 || b >= block_count)
                return -EPROTO;

            *blkno = b;
            return 0;
        }

        idx -= (uint32_t)per_dind;

        /* ---- Triple-indirect ---- */
        {
            uint64_t per_tind = per_dind * ptrs_per_block;
            if (per_tind == 0)
                return -EPROTO;

            if (idx < (uint32_t)per_tind) {
                uint32_t tind_blk = inode->i_block[14];
                uint32_t *lvl1 = NULL;
                uint32_t *lvl2 = NULL;
                uint32_t *lvl3 = NULL;
                int r;

                if (tind_blk == 0 || tind_blk >= block_count)
                    return -EPROTO;

                lvl1 = (uint32_t *)malloc((size_t)block_size);
                if (!lvl1)
                    return -ENOMEM;

                r = read_exact_at(img, lvl1, (size_t)block_size,
                                  (off_t)tind_blk * (off_t)block_size);
                if (r < 0) {
                    free(lvl1);
                    return r;
                }

                uint64_t per_lvl2 = (uint64_t)ptrs_per_block * (uint64_t)ptrs_per_block;
                if (per_lvl2 == 0) {
                    free(lvl1);
                    return -EPROTO;
                }

                uint32_t idx_lvl1 = idx / (uint32_t)per_lvl2;
                uint32_t rem      = idx % (uint32_t)per_lvl2;
                uint32_t idx_lvl2 = rem / ptrs_per_block;
                uint32_t idx_lvl3 = rem % ptrs_per_block;

                uint32_t blk_lvl2 = lvl1[idx_lvl1];
                if (blk_lvl2 == 0 || blk_lvl2 >= block_count) {
                    free(lvl1);
                    return -EPROTO;
                }

                lvl2 = (uint32_t *)malloc((size_t)block_size);
                if (!lvl2) {
                    free(lvl1);
                    return -ENOMEM;
                }

                r = read_exact_at(img, lvl2, (size_t)block_size,
                                  (off_t)blk_lvl2 * (off_t)block_size);
                if (r < 0) {
                    free(lvl2);
                    free(lvl1);
                    return r;
                }

                uint32_t blk_lvl3 = lvl2[idx_lvl2];
                if (blk_lvl3 == 0 || blk_lvl3 >= block_count) {
                    free(lvl2);
                    free(lvl1);
                    return -EPROTO;
                }

                lvl3 = (uint32_t *)malloc((size_t)block_size);
                if (!lvl3) {
                    free(lvl2);
                    free(lvl1);
                    return -ENOMEM;
                }

                r = read_exact_at(img, lvl3, (size_t)block_size,
                                  (off_t)blk_lvl3 * (off_t)block_size);
                if (r < 0) {
                    free(lvl3);
                    free(lvl2);
                    free(lvl1);
                    return r;
                }

                uint32_t b = lvl3[idx_lvl3];

                free(lvl3);
                free(lvl2);
                free(lvl1);

                if (b == 0 || b >= block_count)
                    return -EPROTO;

                *blkno = b;
                return 0;
            }
        }
    }

    /* Logical block outside representable range. */
    return -EPROTO;
}

/* ----------------------------- dump_dir ----------------------------- */

int dump_dir(int img, int inode_nr)
{
    if (inode_nr <= 0)
        return -EINVAL;

    int      block_size = 0;
    uint32_t first_data_block = 0;
    uint32_t inodes_per_group = 0;
    uint32_t inode_size = 0;
    uint32_t inodes_count = 0;
    uint32_t blocks_count = 0;

    int r = ext2_read_basic_sb(img,
                               &block_size,
                               &first_data_block,
                               &inodes_per_group,
                               &inode_size,
                               &inodes_count,
                               &blocks_count);
    if (r < 0)
        return r;

    struct ext2_inode_disk inode;
    r = ext2_read_inode(img,
                        block_size,
                        first_data_block,
                        inodes_per_group,
                        inode_size,
                        inode_nr,
                        &inode);
    if (r < 0)
        return r;

    /* Directory size in bytes. */
    uint64_t dir_size = (uint64_t)inode.i_size;

    if (block_size <= 0)
        return -EPROTO;

    uint64_t total_blocks64 =
        (dir_size + (uint64_t)block_size - 1u) / (uint64_t)block_size;

    /* Guard against insane sizes. */
    if (total_blocks64 > (uint64_t)INT32_MAX)
        return -EPROTO;

    int total_blocks = (int)total_blocks64;

    uint8_t *buf = (uint8_t *)malloc((size_t)block_size);
    if (!buf)
        return -ENOMEM;

    int err = 0;

    for (int lblock = 0; lblock < total_blocks; ++lblock) {
        uint32_t blkno;
        r = inode_get_block(img,
                            block_size,
                            blocks_count,
                            &inode,
                            (uint32_t)lblock,
                            &blkno);
        if (r < 0) {
            err = r;
            break;
        }

        /* Read the whole block, but only parse the bytes that belong to
           the directory according to i_size. */
        r = read_exact_at(img,
                          buf,
                          (size_t)block_size,
                          (off_t)blkno * (off_t)block_size);
        if (r < 0) {
            err = r;
            break;
        }

        uint64_t block_start = (uint64_t)lblock * (uint64_t)block_size;
        size_t bytes_in_block;

        if (dir_size >= block_start + (uint64_t)block_size)
            bytes_in_block = (size_t)block_size;
        else if (dir_size > block_start)
            bytes_in_block = (size_t)(dir_size - block_start);
        else
            bytes_in_block = 0;

        size_t offset = 0;

        while (offset + sizeof(struct ext2_dir_entry_disk) <= bytes_in_block) {
            struct ext2_dir_entry_disk *de =
                (struct ext2_dir_entry_disk *)(buf + offset);

            uint32_t inode_child = de->inode;
            uint16_t rec_len     = de->rec_len;
            uint8_t  name_len    = de->name_len;
            uint8_t  file_type   = de->file_type;

            if (rec_len < 8 ||
                (rec_len & 3u) != 0 ||
                offset + rec_len > bytes_in_block) {
                err = -EPROTO;
                goto out;
            }

            if (name_len > 0 && inode_child != 0) {
                if (name_len > rec_len - 8) {
                    err = -EPROTO;
                    goto out;
                }

                /* Guard against inode numbers that do not fit into the
                   report_file() interface. */
                if (inode_child > (uint32_t)INT_MAX) {
                    err = -EPROTO;
                    goto out;
                }

                char type_char;
                if (file_type == EXT2_FT_REG_FILE)
                    type_char = 'f';
                else if (file_type == EXT2_FT_DIR)
                    type_char = 'd';
                else {
                    /* Only regular files / directories are expected. */
                    err = -EPROTO;
                    goto out;
                }

                char name[256];
                memcpy(name, de->name, name_len);
                name[name_len] = '\0';

                report_file((int)inode_child, type_char, name);
            }

            offset += rec_len;
        }
    }

out:
    free(buf);
    return err;
}
