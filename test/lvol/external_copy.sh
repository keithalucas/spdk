#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 SUSE LLC.
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

function test_shallow_copy_compare() {
	# Create lvs
	bs_malloc_name=$(rpc_cmd bdev_malloc_create 20 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$bs_malloc_name" lvs_test)

	# Create lvol with 4 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * 4))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	# Fill second and fourth cluster of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1 seek=1
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1 seek=3
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Fill first and third cluster of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=1 seek=2
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Set lvol as read only to perform the copy
	rpc_cmd bdev_lvol_set_read_only "$lvol_uuid"

	# Create external bdev to make a shallow copy of lvol on
	ext_malloc_name=$(rpc_cmd bdev_malloc_create "$lvol_size" $MALLOC_BS)

	# Make a shallow copy of lvol over external bdev
	rpc_cmd bdev_lvol_shallow_copy "$lvol_uuid" "$ext_malloc_name"

	# Create nbd devices of lvol and external bdev for comparison
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$ext_malloc_name" /dev/nbd1

	# Compare lvol and external bdev in first and third cluster
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd0 /dev/nbd1
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd0 /dev/nbd1 "$((LVS_DEFAULT_CLUSTER_SIZE * 2))" "$((LVS_DEFAULT_CLUSTER_SIZE * 2))"

	# Check that second and fourth cluster of external bdev are zero filled
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd1 /dev/zero "$LVS_DEFAULT_CLUSTER_SIZE"
	cmp -n "$LVS_DEFAULT_CLUSTER_SIZE" /dev/nbd1 /dev/zero "$((LVS_DEFAULT_CLUSTER_SIZE * 3))"

	# Stop nbd devices
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_shallow_copy_compare" test_shallow_copy_compare

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
