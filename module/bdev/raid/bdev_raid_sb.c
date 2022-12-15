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

struct raid_bdev_read_sb_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	raid_bdev_load_sb_cb cb;
	void *cb_ctx;
	void *buf;
	uint32_t buf_size;
};

struct raid_bdev_save_sb_ctx {
	raid_bdev_save_sb_cb cb;
	void *cb_ctx;
};

static void raid_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static int
raid_bdev_parse_superblock(struct raid_bdev_read_sb_ctx *ctx)
{
	struct raid_bdev_superblock *sb = ctx->buf;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	uint32_t crc;

	if (memcmp(sb->signature, RAID_BDEV_SB_SIG, sizeof(sb->signature))) {
		SPDK_DEBUGLOG(bdev_raid_sb, "invalid signature\n");
		return -EINVAL;
	}

	if (sb->length > ctx->buf_size) {
		if (sb->length > RAID_BDEV_SB_MAX_LENGTH) {
			SPDK_DEBUGLOG(bdev_raid_sb, "invalid length\n");
			return -EINVAL;
		}

		return -EAGAIN;
	}

	crc = sb->crc;
	raid_bdev_sb_update_crc(sb);
	if (sb->crc != crc) {
		SPDK_WARNLOG("Incorrect superblock crc on bdev %s\n", spdk_bdev_get_name(bdev));
		sb->crc = crc;
		return -EINVAL;
	}

	if (sb->version.major > RAID_BDEV_SB_VERSION_MAJOR) {
		SPDK_ERRLOG("Not supported superblock major version %d on bdev %s\n",
			    sb->version.major, spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	if (sb->version.major == RAID_BDEV_SB_VERSION_MAJOR &&
	    sb->version.minor > RAID_BDEV_SB_VERSION_MINOR) {
		SPDK_WARNLOG("Superblock minor version %d on bdev %s is higher than the currently supported: %d\n",
			     sb->version.minor, spdk_bdev_get_name(bdev), RAID_BDEV_SB_VERSION_MINOR);
	}

	return 0;
}

static void
raid_bdev_read_sb_ctx_free(struct raid_bdev_read_sb_ctx *ctx)
{
	spdk_dma_free(ctx->buf);

	free(ctx);
}

static int
raid_bdev_read_sb_remainder(struct raid_bdev_read_sb_ctx *ctx)
{
	struct raid_bdev_superblock *sb = ctx->buf;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	uint32_t buf_size_prev;
	void *buf;
	int rc;

	buf_size_prev = ctx->buf_size;
	ctx->buf_size = SPDK_ALIGN_CEIL(sb->length, spdk_bdev_get_block_size(bdev));
	buf = spdk_dma_realloc(ctx->buf, ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (buf == NULL) {
		SPDK_ERRLOG("Failed to reallocate buffer\n");
		return -ENOMEM;
	}
	ctx->buf = buf;

	rc = spdk_bdev_read(ctx->desc, ctx->ch, ctx->buf + buf_size_prev, buf_size_prev,
			    ctx->buf_size - buf_size_prev, raid_bdev_read_sb_cb, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read bdev %s superblock remainder: %s\n",
			    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static void
raid_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_read_sb_ctx *ctx = cb_arg;
	struct raid_bdev_superblock *sb = NULL;
	int status;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		status = raid_bdev_parse_superblock(ctx);
		if (status == -EAGAIN) {
			status = raid_bdev_read_sb_remainder(ctx);
			if (status == 0) {
				return;
			}
		} else if (status != 0) {
			SPDK_DEBUGLOG(bdev_raid_sb, "failed to parse bdev %s superblock\n",
				      spdk_bdev_get_name(spdk_bdev_desc_get_bdev(ctx->desc)));
		} else {
			sb = ctx->buf;
		}
	} else {
		status = -EIO;
	}

	if (ctx->cb) {
		ctx->cb(sb, status, ctx->cb_ctx);
	}

	raid_bdev_read_sb_ctx_free(ctx);
}

int
raid_bdev_load_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				    raid_bdev_load_sb_cb cb, void *cb_ctx)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct raid_bdev_read_sb_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->desc = desc;
	ctx->ch = ch;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;
	ctx->buf_size = SPDK_ALIGN_CEIL(sizeof(struct raid_bdev_superblock),
					spdk_bdev_get_block_size(bdev));
	ctx->buf = spdk_dma_malloc(ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (!ctx->buf) {
		rc = -ENOMEM;
		goto err;
	}

	rc = spdk_bdev_read(desc, ch, ctx->buf, 0, ctx->buf_size, raid_bdev_read_sb_cb, ctx);
	if (rc) {
		goto err;
	}

	return 0;
err:
	raid_bdev_read_sb_ctx_free(ctx);

	return rc;
}

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
