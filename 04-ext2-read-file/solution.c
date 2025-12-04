#include <solution.h>
#include <ext2blkiter.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>

/* Forward declarations for the ext2 block iterator helpers that live in
 * ext2blkiter.c.  Only the types are opaque; we do not depend on their
 * internal layout here.
 */
struct ext2_fs;
struct ext2_blkiter;

int  ext2_fs_init(struct ext2_fs **fs_out, int fd);
void ext2_fs_free(struct ext2_fs *fs);

int  ext2_blkiter_init(struct ext2_blkiter **it_out,
                       struct ext2_fs *fs,
                       int ino);
int  ext2_blkiter_next(struct ext2_blkiter *it, int *blkno);
/* ext2_blkiter_free() is already declared in ext2blkiter.h. */

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
    if (lseek(fd, offset, SEEK_SET) < 0)
        return -errno;

    uint8_t *p = (uint8_t *)buf;
    size_t remaining = count;

    while (remaining > 0) {
        ssize_t r = read(fd, p, remaining);
        if (r < 0)
            return -errno;
        if (r == 0)
            return -EIO; /* unexpected EOF */
        p += (size_t)r;
        remaining -= (size_t)r;
    }

    return 0;
}

static int ext2_read_basic_sb(int img,
                              int *block_size_out,
                              uint32_t *inodes_per_group_out,
                              uint32_t *inode_size_out,
                              uint32_t *first_data_block_out,
                              uint32_t *inodes_count_out)
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

    *block_size_out        = block_size;
    *inodes_per_group_out  = sb.s_inodes_per_group;
    *inode_size_out        = inode_size;
    *first_data_block_out  = sb.s_first_data_block;
    *inodes_count_out      = sb.s_inodes_count;

    return 0;
}

static int ext2_read_inode_size(int img,
                                int inode_nr,
                                int block_size,
                                uint32_t inodes_per_group,
                                uint32_t inode_size,
                                uint32_t first_data_block,
                                uint64_t *size_out)
{
    if (inode_nr <= 0)
        return -EINVAL;

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
    size_t to_read = inode_size < (uint32_t)sizeof(inode)
        ? (size_t)inode_size
        : sizeof(inode);

    r = read_exact_at(img, &inode, to_read, inode_off);
    if (r < 0)
        return r;

    if (inode.i_mode == 0 || inode.i_links_count == 0)
        return -ENOENT;

    *size_out = (uint64_t)inode.i_size;
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

    int r = ext2_read_basic_sb(img,
                               &block_size,
                               &inodes_per_group,
                               &inode_size,
                               &first_data_block,
                               &inodes_count);
    if (r < 0)
        return r;

    if ((uint32_t)inode_nr > inodes_count)
        return -EINVAL;

    uint64_t file_size = 0;
    r = ext2_read_inode_size(img,
                             inode_nr,
                             block_size,
                             inodes_per_group,
                             inode_size,
                             first_data_block,
                             &file_size);
    if (r < 0)
        return r;

    /* Use the ext2 block iterator helpers working on a *duplicate* of the
     * image fd, so that we do not interfere with the caller's descriptor.
     */
    int dupfd = dup(img);
    if (dupfd < 0)
        return -errno;

    struct ext2_fs *fs = NULL;
    r = ext2_fs_init(&fs, dupfd);
    if (r < 0) {
        close(dupfd);
        return r;
    }

    struct ext2_blkiter *it = NULL;
    r = ext2_blkiter_init(&it, fs, inode_nr);
    if (r < 0) {
        ext2_fs_free(fs);
        return r;
    }

    char *buf = (char *)malloc((size_t)block_size);
    if (!buf) {
        ext2_blkiter_free(it);
        ext2_fs_free(fs);
        return -ENOMEM;
    }

    uint64_t remaining = file_size;
    int err = 0;

    while (1) {
        int blkno = 0;
        r = ext2_blkiter_next(it, &blkno);
        if (r < 0) {
            err = r;
            break;
        }
        if (r == 0) {
            /* No more blocks.  We are done. */
            break;
        }

        if (remaining == 0) {
            /* More blocks than bytes to output – ignore the rest. */
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
    }

out:
    free(buf);
    ext2_blkiter_free(it);
    ext2_fs_free(fs);
    return err;
}
