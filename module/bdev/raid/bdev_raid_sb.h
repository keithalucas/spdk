/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_RAID_SB_H_
#define SPDK_BDEV_RAID_SB_H_

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

#define RAID_BDEV_SB_VERSION_MAJOR	1
#define RAID_BDEV_SB_VERSION_MINOR	0

#define RAID_BDEV_SB_NAME_SIZE		32

#define RAID_BDEV_SB_MAX_LENGTH \
	SPDK_ALIGN_CEIL((sizeof(struct raid_bdev_superblock) + UINT8_MAX * sizeof(struct raid_bdev_sb_base_bdev)), 0x1000)

enum raid_bdev_sb_base_bdev_state {
	RAID_SB_BASE_BDEV_MISSING	= 0,
	RAID_SB_BASE_BDEV_CONFIGURED	= 1,
	RAID_SB_BASE_BDEV_FAILED	= 2,
	RAID_SB_BASE_BDEV_SPARE		= 3,
};

struct raid_bdev_sb_base_bdev {
	/* uuid of the base bdev */
	struct spdk_uuid	uuid;
	/* offset in blocks from base device start to the start of raid data area */
	uint64_t		data_offset;
	/* size in blocks of the base device raid data area */
	uint64_t		data_size;
	/* state of the base bdev */
	uint32_t		state;
	/* feature/status flags */
	uint32_t		flags;
	/* slot number of this base bdev in the raid */
	uint8_t			slot;

	uint8_t			reserved[23];
};
SPDK_STATIC_ASSERT(sizeof(struct raid_bdev_sb_base_bdev) == 64, "incorrect size");

struct raid_bdev_superblock {
#define RAID_BDEV_SB_SIG "SPDKRAID"
	uint8_t			signature[8];
	struct {
		/* incremented when a breaking change in the superblock structure is made */
		uint16_t	major;
		/* incremented for changes in the superblock that are backward compatible */
		uint16_t	minor;
	} version;
	/* length in bytes of the entire superblock */
	uint32_t		length;
	/* crc32c checksum of the entire superblock */
	uint32_t		crc;
	/* feature/status flags */
	uint32_t		flags;
	/* unique id of the raid bdev */
	struct spdk_uuid	uuid;
	/* name of the raid bdev */
	uint8_t			name[RAID_BDEV_SB_NAME_SIZE];
	/* size of the raid bdev in blocks */
	uint64_t		raid_size;
	/* the raid bdev block size - must be the same for all base bdevs */
	uint32_t		block_size;
	/* the raid level */
	uint32_t		level;
	/* strip (chunk) size in blocks */
	uint32_t		strip_size;
	/* state of the raid */
	uint32_t		state;
	/* sequence number, incremented on every superblock update */
	uint64_t		seq_number;
	/* number of raid base devices */
	uint8_t			num_base_bdevs;

	uint8_t			reserved[86];

	/* size of the base bdevs array */
	uint8_t			base_bdevs_size;
	/* array of base bdev descriptors */
	struct raid_bdev_sb_base_bdev base_bdevs[];
};
SPDK_STATIC_ASSERT(sizeof(struct raid_bdev_superblock) == 192, "incorrect size");

typedef void (*raid_bdev_load_sb_cb)(const struct raid_bdev_superblock *sb, int status, void *ctx);
typedef void (*raid_bdev_save_sb_cb)(int status, void *ctx);

int raid_bdev_load_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
					raid_bdev_load_sb_cb cb, void *cb_ctx);
int raid_bdev_save_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
					const struct raid_bdev_superblock *sb, raid_bdev_save_sb_cb cb, void *cb_ctx);
void raid_bdev_sb_update_crc(struct raid_bdev_superblock *sb);

#endif /* SPDK_BDEV_RAID_SB_H_ */
