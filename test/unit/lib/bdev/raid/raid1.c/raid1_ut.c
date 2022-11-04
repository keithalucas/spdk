/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/env.h"

#include "common/lib/ut_multithread.c"

#include "bdev/raid/raid1.c"
#include "../common.c"

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB_V(raid_bdev_module_stop_done, (struct raid_bdev *raid_bdev));
DEFINE_STUB(raid_bdev_io_complete_part, bool, (struct raid_bdev_io *raid_io, uint64_t completed,
		enum spdk_bdev_io_status status), true);
DEFINE_STUB_V(raid_bdev_queue_io_wait, (struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
					struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn));
DEFINE_STUB(spdk_bdev_readv_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, void *md,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, void *md,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_readv_blocks_ext, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts), 0);
DEFINE_STUB(spdk_bdev_writev_blocks_ext, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts), 0);

static int
test_setup(void)
{
	uint8_t num_base_bdevs_values[] = { 2, 3 };
	uint64_t base_bdev_blockcnt_values[] = { 1, 1024, 1024 * 1024 };
	uint32_t base_bdev_blocklen_values[] = { 512, 4096 };
	uint8_t *num_base_bdevs;
	uint64_t *base_bdev_blockcnt;
	uint32_t *base_bdev_blocklen;
	struct raid_params params;
	uint64_t params_count;
	int rc;

	params_count = SPDK_COUNTOF(num_base_bdevs_values) *
		       SPDK_COUNTOF(base_bdev_blockcnt_values) *
		       SPDK_COUNTOF(base_bdev_blocklen_values);
	rc = raid_test_params_alloc(params_count);
	if (rc) {
		return rc;
	}

	ARRAY_FOR_EACH(num_base_bdevs_values, num_base_bdevs) {
		ARRAY_FOR_EACH(base_bdev_blockcnt_values, base_bdev_blockcnt) {
			ARRAY_FOR_EACH(base_bdev_blocklen_values, base_bdev_blocklen) {
				params.num_base_bdevs = *num_base_bdevs;
				params.base_bdev_blockcnt = *base_bdev_blockcnt;
				params.base_bdev_blocklen = *base_bdev_blocklen;
				params.strip_size = 0;
				params.md_len = 0;
				raid_test_params_add(&params);
			}
		}
	}

	return 0;
}

static int
test_cleanup(void)
{
	raid_test_params_free();
	return 0;
}

static struct raid1_info *
create_raid1(struct raid_params *params)
{
	struct raid_bdev *raid_bdev = raid_test_create_raid_bdev(params, &g_raid1_module);

	SPDK_CU_ASSERT_FATAL(raid1_start(raid_bdev) == 0);

	return raid_bdev->module_private;
}

static void
delete_raid1(struct raid1_info *r1info)
{
	struct raid_bdev *raid_bdev = r1info->raid_bdev;

	raid1_stop(raid_bdev);

	raid_test_delete_raid_bdev(raid_bdev);
}

static void
test_raid1_start(void)
{
	struct raid_params *params;

	RAID_PARAMS_FOR_EACH(params) {
		struct raid1_info *r1info;

		r1info = create_raid1(params);

		SPDK_CU_ASSERT_FATAL(r1info != NULL);

		CU_ASSERT_EQUAL(r1info->raid_bdev->level, RAID1);
		CU_ASSERT_EQUAL(r1info->raid_bdev->bdev.blockcnt, params->base_bdev_blockcnt);
		CU_ASSERT_PTR_EQUAL(r1info->raid_bdev->module, &g_raid1_module);

		delete_raid1(r1info);
	}
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	free(bdev_io);
}

void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);

	spdk_bdev_free_io(bdev_io);
}

static struct raid_bdev_io *
get_raid_io(struct raid1_info *r1info, struct raid_bdev_io_channel *raid_ch,
	    enum spdk_bdev_io_type io_type, uint64_t num_blocks)
{
	struct spdk_bdev_io *bdev_io;
	struct raid_bdev_io *raid_io;

	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);

	bdev_io->bdev = &r1info->raid_bdev->bdev;
	bdev_io->type = io_type;
	bdev_io->u.bdev.offset_blocks = 0;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->internal.cb = NULL;
	bdev_io->internal.caller_ctx = NULL;

	raid_io = (void *)bdev_io->driver_ctx;
	raid_io->raid_bdev = r1info->raid_bdev;
	raid_io->raid_ch = raid_ch;

	return raid_io;
}

static void
run_for_each_raid1_config(void (*test_fn)(struct raid_bdev *raid_bdev,
			  struct raid_bdev_io_channel *raid_ch))
{
	struct raid_params *params;

	RAID_PARAMS_FOR_EACH(params) {
		struct raid1_info *r1info;
		struct raid_bdev_io_channel raid_ch = { 0 };
		int i;

		r1info = create_raid1(params);

		raid_ch.num_channels = params->num_base_bdevs;
		raid_ch.base_channel = calloc(params->num_base_bdevs, sizeof(struct spdk_io_channel *));
		SPDK_CU_ASSERT_FATAL(raid_ch.base_channel != NULL);
		for (i = 0; i < raid_ch.num_channels; i++) {
			raid_ch.base_channel[i] = calloc(1, sizeof(*raid_ch.base_channel));
		}

		raid_ch.module_channel = raid1_get_io_channel(r1info->raid_bdev);
		SPDK_CU_ASSERT_FATAL(raid_ch.module_channel);

		test_fn(r1info->raid_bdev, &raid_ch);

		spdk_put_io_channel(raid_ch.module_channel);
		poll_threads();

		for (i = 0; i < raid_ch.num_channels; i++) {
			free(raid_ch.base_channel[i]);
		}
		free(raid_ch.base_channel);

		delete_raid1(r1info);
	}
}

static void
__test_raid1_read_balancing(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid1_info *r1info = raid_bdev->module_private;
	struct raid_bdev_io *raid_io;
	struct raid1_io_channel *raid1_ch = spdk_io_channel_get_ctx(raid_ch->module_channel);
	uint8_t overloaded_ch_idx = 0;
	uint64_t big_io_blocks = 256;
	uint64_t small_io_blocks = 4;
	bool found_greater = false;

	raid_io = get_raid_io(r1info, raid_ch, SPDK_BDEV_IO_TYPE_READ, big_io_blocks);
	raid1_submit_rw_request(raid_io);
	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	overloaded_ch_idx = raid1_ch->base_bdev_read_idx;

	do {
		raid_io = get_raid_io(r1info, raid_ch, SPDK_BDEV_IO_TYPE_READ, small_io_blocks);
		raid1_submit_rw_request(raid_io);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} while (raid1_ch->base_bdev_read_idx != overloaded_ch_idx);

	for (uint8_t i = 0; i < raid_ch->num_channels; i++) {
		if (i == overloaded_ch_idx) {
			continue;
		}

		if (raid1_ch->base_bdev_read_bw[i] >= raid1_ch->base_bdev_read_bw[overloaded_ch_idx] -
		    small_io_blocks) {
			found_greater = true;
			break;
		}
	}

	CU_ASSERT_TRUE(found_greater);
}

static void
test_raid1_read_balancing(void)
{
	run_for_each_raid1_config(__test_raid1_read_balancing);
}

static void
__test_raid1_read_balancing_limit_reset(struct raid_bdev *raid_bdev,
					struct raid_bdev_io_channel *raid_ch)
{
	struct raid1_info *r1info = raid_bdev->module_private;
	struct raid_bdev_io *raid_io;
	struct raid1_io_channel *raid1_ch = spdk_io_channel_get_ctx(raid_ch->module_channel);
	uint64_t read_io_blocks = 64;

	raid1_ch->base_bdev_max_read_bw = UINT64_MAX - (read_io_blocks / 2);
	for (uint8_t i = 0; i < raid_ch->num_channels; i++) {
		raid1_ch->base_bdev_read_bw[i] = UINT64_MAX - (read_io_blocks / 2);
	}

	raid_io = get_raid_io(r1info, raid_ch, SPDK_BDEV_IO_TYPE_READ, read_io_blocks);
	raid1_submit_rw_request(raid_io);
	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);

	for (uint8_t i = 0; i < raid_ch->num_channels; i++) {
		if (i == raid1_ch->base_bdev_read_idx) {
			continue;
		}

		CU_ASSERT_EQUAL(raid1_ch->base_bdev_read_bw[i], 0);
	}
}

static void
test_raid1_read_balancing_limit_reset(void)
{
	run_for_each_raid1_config(__test_raid1_read_balancing_limit_reset);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("raid1", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid1_start);
	CU_ADD_TEST(suite, test_raid1_read_balancing);
	CU_ADD_TEST(suite, test_raid1_read_balancing_limit_reset);

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
