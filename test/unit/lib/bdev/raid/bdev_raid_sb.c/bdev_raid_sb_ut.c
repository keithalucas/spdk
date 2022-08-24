/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/env.h"
#include "spdk_internal/mock.h"

#include "common/lib/test_env.c"
#include "bdev/raid/bdev_raid_sb.c"

#define TEST_BUF_ALIGN	64
#define TEST_BLOCK_SIZE	512

DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), TEST_BLOCK_SIZE);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));

void *g_buf;

static int
test_setup(void)
{
	g_buf = spdk_dma_zmalloc(RAID_BDEV_SB_MAX_LENGTH, TEST_BUF_ALIGN, NULL);
	if (!g_buf) {
		return -ENOMEM;
	}

	return 0;
}

static int
test_cleanup(void)
{
	spdk_dma_free(g_buf);

	return 0;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct raid_bdev_superblock *sb = buf;

	CU_ASSERT(offset == 0);
	CU_ASSERT(nbytes / TEST_BLOCK_SIZE == spdk_divide_round_up(sb->length, TEST_BLOCK_SIZE));

	cb(NULL, true, cb_arg);
	return 0;
}

static void
prepare_sb(struct raid_bdev_superblock *sb)
{
	/* prepare a simplest valid sb */
	memset(sb, 0, RAID_BDEV_SB_MAX_LENGTH);
	memcpy(sb->signature, RAID_BDEV_SB_SIG, sizeof(sb->signature));
	sb->version.major = RAID_BDEV_SB_VERSION_MAJOR;
	sb->version.minor = RAID_BDEV_SB_VERSION_MINOR;
	sb->length = sizeof(*sb);
	sb->crc = spdk_crc32c_update(sb, sb->length, 0);
}

static void
save_sb_cb(int status, void *ctx)
{
	int *status_out = ctx;

	*status_out = status;
}

static void
test_raid_bdev_save_base_bdev_superblock(void)
{
	struct raid_bdev_superblock *sb = g_buf;
	int rc;
	int status;

	prepare_sb(sb);

	status = INT_MAX;
	rc = raid_bdev_save_base_bdev_superblock(NULL, NULL, sb, save_sb_cb, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("raid_sb", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid_bdev_save_base_bdev_superblock);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
