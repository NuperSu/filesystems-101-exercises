#include <solution.h>
#include <fs_malloc.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>

/* -------- On-disk EXT2 structures (only the parts we need) -------- */

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
	/* rest not needed */
} __attribute__((packed));

/* Block group descriptor on disk (classic 32-byte format). */
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

/* Base 128-byte inode on disk. */
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

/* -------- In-memory helper structures -------- */

struct ext2_group_desc {
	uint32_t bg_inode_table; /* absolute block number */
};

struct ext2_fs
{
	int fd;
	int block_size;
	int block_count;
	int inodes_count;
	int inodes_per_group;
	int blocks_per_group;
	int first_data_block;
	int inode_size;
	struct ext2_group_desc *group_desc;
	int group_count;
};

struct ext2_blkiter
{
	struct ext2_fs *fs;
	int *blocks;       /* array of physical block numbers */
	int total_blocks;  /* number of entries in @blocks */
	int current_index; /* next index to return */
};

/* -------- Small helpers -------- */

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

static off_t block_offset(const struct ext2_fs *fs, uint32_t blk)
{
	return (off_t) blk * (off_t) fs->block_size;
}

/* Map logical block index -> physical block number. */
static int inode_get_block(struct ext2_fs *fs,
                           const struct ext2_inode_disk *inode,
                           uint32_t lblock,
                           uint32_t *blkno)
{
	uint32_t ptrs_per_block = (uint32_t)fs->block_size / sizeof(uint32_t);
	const uint32_t direct_count = 12;
	uint32_t idx = lblock;

	if (ptrs_per_block == 0)
		return -EPROTO;

	/* ---- Direct blocks ---- */
	if (idx < direct_count) {
		uint32_t b = inode->i_block[idx];
		if (b == 0 || b >= (uint32_t)fs->block_count)
			return -EPROTO;
		*blkno = b;
		return 0;
	}

	idx -= direct_count;

	/* ---- Single-indirect ---- */
	if (idx < ptrs_per_block) {
		uint32_t ind_blk = inode->i_block[12];
		uint32_t *buf;
		int r;

		if (ind_blk == 0 || ind_blk >= (uint32_t)fs->block_count)
			return -EPROTO;

		buf = (uint32_t *)malloc(fs->block_size);
		if (!buf)
			return -ENOMEM;

		r = read_exact_at(fs->fd, buf, fs->block_size,
		                  block_offset(fs, ind_blk));
		if (r < 0) {
			free(buf);
			return r;
		}

		uint32_t b = buf[idx];
		free(buf);

		if (b == 0 || b >= (uint32_t)fs->block_count)
			return -EPROTO;

		*blkno = b;
		return 0;
	}

	idx -= ptrs_per_block;

	/* ---- Double-indirect ---- */
	{
		uint64_t per_dind = (uint64_t)ptrs_per_block * ptrs_per_block;
		if (idx < per_dind) {
			uint32_t dind_blk = inode->i_block[13];
			uint32_t *outer = NULL;
			uint32_t *inner = NULL;
			int r;

			if (dind_blk == 0 || dind_blk >= (uint32_t)fs->block_count)
				return -EPROTO;

			outer = (uint32_t *)malloc(fs->block_size);
			if (!outer)
				return -ENOMEM;

			r = read_exact_at(fs->fd, outer, fs->block_size,
			                  block_offset(fs, dind_blk));
			if (r < 0) {
				free(outer);
				return r;
			}

			uint32_t outer_index = idx / ptrs_per_block;
			uint32_t inner_index = idx % ptrs_per_block;
			uint32_t ind_blk = outer[outer_index];

			if (ind_blk == 0 || ind_blk >= (uint32_t)fs->block_count) {
				free(outer);
				return -EPROTO;
			}

			inner = (uint32_t *)malloc(fs->block_size);
			if (!inner) {
				free(outer);
				return -ENOMEM;
			}

			r = read_exact_at(fs->fd, inner, fs->block_size,
			                  block_offset(fs, ind_blk));
			if (r < 0) {
				free(inner);
				free(outer);
				return r;
			}

			uint32_t b = inner[inner_index];

			free(inner);
			free(outer);

			if (b == 0 || b >= (uint32_t)fs->block_count)
				return -EPROTO;

			*blkno = b;
			return 0;
		}

		idx -= (uint32_t)per_dind;

		/* ---- Triple-indirect ---- */
		{
			uint64_t per_tind = per_dind * ptrs_per_block;
			if (idx < per_tind) {
				uint32_t tind_blk = inode->i_block[14];
				uint32_t *lvl1 = NULL;
				uint32_t *lvl2 = NULL;
				uint32_t *lvl3 = NULL;
				int r;

				if (tind_blk == 0 ||
				    tind_blk >= (uint32_t)fs->block_count)
					return -EPROTO;

				lvl1 = (uint32_t *)malloc(fs->block_size);
				if (!lvl1)
					return -ENOMEM;

				r = read_exact_at(fs->fd, lvl1, fs->block_size,
				                  block_offset(fs, tind_blk));
				if (r < 0) {
					free(lvl1);
					return r;
				}

				uint64_t idx1 = idx / per_dind;
				uint64_t rem  = idx % per_dind;

				uint32_t blk_lvl2 = lvl1[idx1];

				if (blk_lvl2 == 0 ||
				    blk_lvl2 >= (uint32_t)fs->block_count) {
					free(lvl1);
					return -EPROTO;
				}

				lvl2 = (uint32_t *)malloc(fs->block_size);
				if (!lvl2) {
					free(lvl1);
					return -ENOMEM;
				}

				r = read_exact_at(fs->fd, lvl2, fs->block_size,
				                  block_offset(fs, blk_lvl2));
				if (r < 0) {
					free(lvl2);
					free(lvl1);
					return r;
				}

				uint32_t outer_index =
					(uint32_t)(rem / ptrs_per_block);
				uint32_t inner_index =
					(uint32_t)(rem % ptrs_per_block);

				uint32_t blk_lvl3 = lvl2[outer_index];

				if (blk_lvl3 == 0 ||
				    blk_lvl3 >= (uint32_t)fs->block_count) {
					free(lvl2);
					free(lvl1);
					return -EPROTO;
				}

				lvl3 = (uint32_t *)malloc(fs->block_size);
				if (!lvl3) {
					free(lvl2);
					free(lvl1);
					return -ENOMEM;
				}

				r = read_exact_at(fs->fd, lvl3, fs->block_size,
				                  block_offset(fs, blk_lvl3));
				if (r < 0) {
					free(lvl3);
					free(lvl2);
					free(lvl1);
					return r;
				}

				uint32_t b = lvl3[inner_index];

				free(lvl3);
				free(lvl2);
				free(lvl1);

				if (b == 0 || b >= (uint32_t)fs->block_count)
					return -EPROTO;

				*blkno = b;
				return 0;
			}
		}
	}

	/* Logical block outside representable range */
	return -EPROTO;
}

/* -------- Public API -------- */

int ext2_fs_init(struct ext2_fs **fs_out, int fd)
{
	struct ext2_superblock_disk sb;
	struct ext2_fs *fs = NULL;
	struct ext2_group_desc_disk *gdt = NULL;
	int r;

	if (!fs_out)
		return -EINVAL;
	*fs_out = NULL;

	/* Superblock is always at offset 1024. */
	r = read_exact_at(fd, &sb, sizeof(sb), 1024);
	if (r < 0)
		return r;

	if (sb.s_magic != 0xEF53)
		return -EPROTO;

	/* Sanity-check and compute block size safely. */
	if (sb.s_log_block_size > 31)
		return -EPROTO;

	uint64_t block_size_u64 = 1024ull << sb.s_log_block_size;
	if (block_size_u64 == 0 || block_size_u64 > INT_MAX)
		return -EPROTO;

	int block_size = (int)block_size_u64;

	if (sb.s_blocks_per_group == 0 || sb.s_inodes_per_group == 0)
		return -EPROTO;

	fs = (struct ext2_fs *)malloc(sizeof(*fs));
	if (!fs)
		return -ENOMEM;

	/* Ensure 32-bit on-disk counters fit into signed ints. */
	if (sb.s_blocks_count > (uint32_t)INT_MAX ||
	    sb.s_inodes_count > (uint32_t)INT_MAX ||
	    sb.s_inodes_per_group > (uint32_t)INT_MAX ||
	    sb.s_blocks_per_group > (uint32_t)INT_MAX ||
	    sb.s_first_data_block > (uint32_t)INT_MAX) {
		free(fs);
		return -EPROTO;
	}

	fs->fd               = fd;
	fs->block_size       = block_size;
	fs->block_count      = (int)sb.s_blocks_count;
	fs->inodes_count     = (int)sb.s_inodes_count;
	fs->inodes_per_group = (int)sb.s_inodes_per_group;
	fs->blocks_per_group = (int)sb.s_blocks_per_group;
	fs->first_data_block = (int)sb.s_first_data_block;
	fs->inode_size       = sb.s_inode_size ? (int)sb.s_inode_size : 128;
	fs->group_desc       = NULL;
	fs->group_count      = 0;

	/* Basic sanity on derived fields. */
	if (fs->block_count <= 0 ||
	    fs->blocks_per_group <= 0 ||
	    fs->inodes_per_group <= 0) {
		free(fs);
		return -EPROTO;
	}

	/* Inode size must be at least the base on-disk inode and not larger
	 * than a block. This avoids reading partially initialised inodes later. */
	if (fs->inode_size < (int)sizeof(struct ext2_inode_disk) ||
	    fs->inode_size > fs->block_size) {
		free(fs);
		return -EPROTO;
	}

	/* Compute number of block groups using a wide type to avoid overflow. */
	uint64_t blocks_per_grp_u64 = (uint64_t)fs->blocks_per_group;
	uint64_t groups_u64 =
		((uint64_t)fs->block_count + blocks_per_grp_u64 - 1) /
		blocks_per_grp_u64;
	if (groups_u64 == 0 || groups_u64 > (uint64_t)INT_MAX) {
		free(fs);
		return -EPROTO;
	}
	int groups = (int)groups_u64;
	fs->group_count = groups;

	fs->group_desc = (struct ext2_group_desc *)
		malloc((size_t)groups * sizeof(struct ext2_group_desc));
	if (!fs->group_desc) {
		free(fs);
		return -ENOMEM;
	}

	/* Block group descriptor table is in the block after the superblock. */
	size_t gdt_bytes = (size_t)groups * sizeof(struct ext2_group_desc_disk);
	gdt = (struct ext2_group_desc_disk *)malloc(gdt_bytes);
	if (!gdt) {
		free(fs->group_desc);
		free(fs);
		return -ENOMEM;
	}

	off_t gdt_offset =
		(off_t)((uint64_t)(sb.s_first_data_block + 1) *
		        (uint64_t)block_size);

	r = read_exact_at(fd, gdt, gdt_bytes, gdt_offset);
	if (r < 0) {
		free(gdt);
		free(fs->group_desc);
		free(fs);
		return r;
	}

	for (int i = 0; i < groups; ++i) {
		uint32_t table_blk = gdt[i].bg_inode_table;

		if (table_blk == 0 || table_blk >= (uint32_t)fs->block_count) {
			free(gdt);
			free(fs->group_desc);
			free(fs);
			return -EPROTO;
		}

		fs->group_desc[i].bg_inode_table = table_blk;
	}

	free(gdt);

	*fs_out = fs;
	return 0;
}

void ext2_fs_free(struct ext2_fs *fs)
{
	if (!fs)
		return;

	if (fs->fd >= 0)
		close(fs->fd);

	free(fs->group_desc);
	free(fs);
}

int ext2_blkiter_init(struct ext2_blkiter **i_out, struct ext2_fs *fs, int ino)
{
	struct ext2_blkiter *it = NULL;
	struct ext2_inode_disk inode;
	int r;

	if (!i_out || !fs)
		return -EINVAL;
	*i_out = NULL;

	if (ino <= 0 || ino > fs->inodes_count)
		return -EINVAL;

	/* inode numbers are 1-based */
	int idx0  = ino - 1;
	int group = idx0 / fs->inodes_per_group;
	int idx   = idx0 % fs->inodes_per_group;

	if (group < 0 || group >= fs->group_count)
		return -EPROTO;

	uint32_t table_blk = fs->group_desc[group].bg_inode_table;
	if (table_blk == 0 || table_blk >= (uint32_t)fs->block_count)
		return -EPROTO;

	off_t inode_off = block_offset(fs, table_blk) +
	                  (off_t)idx * (off_t)fs->inode_size;

	size_t to_read = (fs->inode_size < (int)sizeof(inode))
		? (size_t)fs->inode_size
		: sizeof(inode);

	r = read_exact_at(fs->fd, &inode, to_read, inode_off);
	if (r < 0)
		return r;

	/* Free inode? */
	if (inode.i_mode == 0 || inode.i_links_count == 0)
		return -ENOENT;

	if (fs->block_size <= 0)
		return -EPROTO;

	uint64_t size = inode.i_size;
	uint64_t total_blocks64 =
		(size + (uint64_t)fs->block_size - 1) / (uint64_t)fs->block_size;

	if (total_blocks64 > INT32_MAX)
		return -EPROTO;

	int total_blocks = (int)total_blocks64;

	it = (struct ext2_blkiter *)malloc(sizeof(*it));
	if (!it)
		return -ENOMEM;

	it->fs            = fs;
	it->total_blocks  = total_blocks;
	it->current_index = 0;
	it->blocks        = NULL;

	if (total_blocks > 0) {
		it->blocks = (int *)malloc((size_t)total_blocks * sizeof(int));
		if (!it->blocks) {
			free(it);
			return -ENOMEM;
		}

		for (int j = 0; j < total_blocks; ++j) {
			uint32_t b;
			r = inode_get_block(fs, &inode, (uint32_t)j, &b);
			if (r < 0) {
				free(it->blocks);
				free(it);
				return r;
			}
			it->blocks[j] = (int)b;
		}
	}

	*i_out = it;
	return 0;
}

int ext2_blkiter_next(struct ext2_blkiter *i, int *blkno)
{
	if (!i || !blkno)
		return -EINVAL;

	if (i->current_index >= i->total_blocks)
		return 0; // iteration is over

	*blkno = i->blocks[i->current_index];
	i->current_index++;
	return 1;
}

void ext2_blkiter_free(struct ext2_blkiter *i)
{
	if (!i)
		return;

	free(i->blocks);
	free(i);
}
