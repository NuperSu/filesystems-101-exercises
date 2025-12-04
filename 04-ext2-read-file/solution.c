#include <solution.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>

/* --------------------- On-disk EXT2 structures --------------------- */

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
    /* the rest is not needed */
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
    uint32_t i_osd2[3];
} __attribute__((packed));

/* -------------------------- Local helpers -------------------------- */

static int read_exact_at(int fd, void *buf, size_t count, off_t offset)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = count;

    while (remaining > 0) {
        size_t chunk = remaining;
        if (chunk > (size_t)SSIZE_MAX)
            chunk = (size_t)SSIZE_MAX;

        ssize_t r = pread(fd, p, chunk, offset);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (r == 0)
            return -EIO; /* unexpected EOF */

        p        += (size_t)r;
        remaining -= (size_t)r;
        offset   += (off_t)r;
    }

    return 0;
}

static int ext2_read_basic_sb(int img,
                              int *block_size_out,
                              uint32_t *inodes_per_group_out,
                              uint32_t *inode_size_out,
                              uint32_t *first_data_block_out,
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

    /* Basic sanity on counts we are going to use later. */
    if (sb.s_inodes_per_group == 0 ||
        sb.s_inodes_count     == 0 ||
        sb.s_blocks_count     == 0)
        return -EPROTO;

    if (sb.s_inodes_per_group > (uint32_t)INT_MAX ||
        sb.s_inodes_count     > (uint32_t)INT_MAX ||
        sb.s_first_data_block > (uint32_t)INT_MAX)
        return -EPROTO;

    uint32_t inode_size = sb.s_inode_size ? sb.s_inode_size : 128u;

    /* Inode size must be reasonable so that i_mode/i_links_count/i_size
     * are fully initialised and the inode fits into a block.
     */
    if (inode_size < (uint32_t)sizeof(struct ext2_inode_disk) ||
        inode_size > (uint32_t)block_size)
        return -EPROTO;

    *block_size_out        = block_size;
    *inodes_per_group_out  = sb.s_inodes_per_group;
    *inode_size_out        = inode_size;
    *first_data_block_out  = sb.s_first_data_block;
    *inodes_count_out      = sb.s_inodes_count;
    *blocks_count_out      = sb.s_blocks_count;

    return 0;
}

static int ext2_read_inode(int img,
                           int inode_nr,
                           int block_size,
                           uint32_t inodes_per_group,
                           uint32_t inode_size,
                           uint32_t first_data_block,
                           struct ext2_inode_disk *inode_out)
{
    if (inode_nr <= 0)
        return -EINVAL;

    if (inodes_per_group == 0)
        return -EPROTO;

    uint32_t ino  = (uint32_t)inode_nr;
    uint32_t idx0 = ino - 1;
    uint32_t group = idx0 / inodes_per_group;
    uint32_t idx   = idx0 % inodes_per_group;

    /* Read the corresponding group descriptor.  Descriptors start in the
     * block right after the superblock.
     */
    off_t gdt_off = (off_t)(first_data_block + 1u) * (off_t)block_size
                    + (off_t)group * (off_t)sizeof(struct ext2_group_desc_disk);

    struct ext2_group_desc_disk gd;
    int r = read_exact_at(img, &gd, sizeof(gd), gdt_off);
    if (r < 0)
        return r;

    if (gd.bg_inode_table == 0)
        return -EPROTO;

    /* Compute on-disk inode offset. */
    off_t inode_off = (off_t)gd.bg_inode_table * (off_t)block_size
                      + (off_t)idx * (off_t)inode_size;

    struct ext2_inode_disk inode;

    /* The on-disk inode may be larger than the classic 128-byte struct, but
     * we only care about the initial fields we know.  ext2_read_basic_sb()
     * ensured inode_size >= sizeof(struct ext2_inode_disk).
     */
    memset(&inode, 0, sizeof(inode));

    r = read_exact_at(img, &inode, sizeof(inode), inode_off);
    if (r < 0)
        return r;

    if (inode.i_mode == 0 || inode.i_links_count == 0)
        return -ENOENT;

    *inode_out = inode;
    return 0;
}

/* Map logical block index -> physical block number.
 *
 * Supports:
 *   - 12 direct blocks
 *   - single-indirect
 *   - double-indirect
 *
 * Returns 0 on success, <0 on error.
 */
static int inode_get_block_simple(int img,
                                  int block_size,
                                  uint32_t block_count,
                                  const struct ext2_inode_disk *inode,
                                  uint32_t lblock,
                                  uint32_t *blkno_out)
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
        *blkno_out = b;
        return 0;
    }

    idx -= direct_count;

    /* ---- Single-indirect ---- */
    if (idx < ptrs_per_block) {
        uint32_t ind_blk = inode->i_block[12];
        uint32_t *buf = NULL;
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

        *blkno_out = b;
        return 0;
    }

    idx -= ptrs_per_block;

    /* ---- Double-indirect ---- */
    uint64_t per_dind = (uint64_t)ptrs_per_block * (uint64_t)ptrs_per_block;
    if (idx >= per_dind)
        return -EFBIG; /* would require triple-indirect, not supported */

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

    if (outer_index >= ptrs_per_block) {
        free(outer);
        return -EPROTO;
    }

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

    *blkno_out = b;
    return 0;
}

/* ----------------------------- API ----------------------------- */

int dump_file(int img, int inode_nr, int out)
{
    if (inode_nr <= 0)
        return -EINVAL;

    int block_size = 0;
    uint32_t inodes_per_group = 0;
    uint32_t inode_size = 0;
    uint32_t first_data_block = 0;
    uint32_t inodes_count = 0;
    uint32_t blocks_count = 0;

    int r = ext2_read_basic_sb(img,
                               &block_size,
                               &inodes_per_group,
                               &inode_size,
                               &first_data_block,
                               &inodes_count,
                               &blocks_count);
    if (r < 0)
        return r;

    if ((uint32_t)inode_nr > inodes_count)
        return -EINVAL;

    struct ext2_inode_disk inode;
    r = ext2_read_inode(img,
                        inode_nr,
                        block_size,
                        inodes_per_group,
                        inode_size,
                        first_data_block,
                        &inode);
    if (r < 0)
        return r;

    uint64_t file_size = (uint64_t)inode.i_size;
    uint64_t remaining = file_size;

    if (remaining == 0)
        return 0;

    char *buf = (char *)malloc((size_t)block_size);
    if (!buf)
        return -ENOMEM;

    uint32_t ptrs_per_block = (uint32_t)block_size / sizeof(uint32_t);
    const uint64_t direct_count = 12;
    uint64_t max_blocks = direct_count
                        + (uint64_t)ptrs_per_block
                        + (uint64_t)ptrs_per_block * (uint64_t)ptrs_per_block;
    uint64_t needed_blocks =
        (file_size + (uint64_t)block_size - 1u) / (uint64_t)block_size;

    if (ptrs_per_block == 0 || needed_blocks > max_blocks) {
        free(buf);
        return -EFBIG; /* would require triple-indirect or larger */
    }

    int err = 0;
    uint32_t lblock = 0;

    while (remaining > 0) {
        uint32_t blkno = 0;

        r = inode_get_block_simple(img,
                                   block_size,
                                   blocks_count,
                                   &inode,
                                   lblock,
                                   &blkno);
        if (r < 0) {
            err = r;
            break;
        }

        size_t this_len = (size_t)block_size;
        if (remaining < (uint64_t)block_size)
            this_len = (size_t)remaining;

        off_t off = (off_t)blkno * (off_t)block_size;

        r = read_exact_at(img, buf, this_len, off);
        if (r < 0) {
            err = r;
            break;
        }

        size_t written = 0;
        while (written < this_len) {
            ssize_t w = write(out, buf + written, this_len - written);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                err = -errno;
                goto out;
            }
            if (w == 0) {
                err = -EIO;
                goto out;
            }
            written += (size_t)w;
        }

        remaining -= this_len;
        lblock++;
    }

out:
    free(buf);
    return err;
}
