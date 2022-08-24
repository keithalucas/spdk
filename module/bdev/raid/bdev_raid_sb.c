/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev_module.h"
#include "spdk/crc32.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "bdev_raid_sb.h"

struct raid_bdev_save_sb_ctx {
	raid_bdev_save_sb_cb cb;
	void *cb_ctx;
};

static void
raid_bdev_write_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_save_sb_ctx *ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (ctx->cb) {
		ctx->cb(success ? 0 : -EIO, ctx->cb_ctx);
	}

	free(ctx);
}

int
raid_bdev_save_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				    const struct raid_bdev_superblock *sb,
				    raid_bdev_save_sb_cb cb, void *cb_ctx)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	uint64_t nbytes = SPDK_ALIGN_CEIL(sb->length, spdk_bdev_get_block_size(bdev));
	struct raid_bdev_save_sb_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;

	rc = spdk_bdev_write(desc, ch, (void *)sb, 0, nbytes, raid_bdev_write_sb_cb, ctx);
	if (rc) {
		free(ctx);
	}

	return rc;
}

void
raid_bdev_sb_update_crc(struct raid_bdev_superblock *sb)
{
	sb->crc = 0;
	sb->crc = spdk_crc32c_update(sb, sb->length, 0);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_raid_sb)
