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

NUM_CLUSTERS=10
LVS_DEFAULT_CLUSTER_SIZE_BTYE=$((LVS_DEFAULT_CLUSTER_SIZE_MB*1024*1024))

function verify() {
	local fragmap="$1"
	local expected_cluster_size="$2"
	local expected_num_clusters="$3"
	local expected_num_allocated_clusters="$4"
	local expected_fragmap="$5"

	[ "$(jq '.cluster_size' <<< "$fragmap")" == "$expected_cluster_size" ]
	[ "$(jq '.num_clusters' <<< "$fragmap")" == "$expected_num_clusters" ]
	[ "$(jq '.num_allocated_clusters' <<< "$fragmap")" == "$expected_num_allocated_clusters" ]
	[ "$(jq -r '.fragmap' <<< "$fragmap")" == "$expected_fragmap" ]
}

function test_fragmap_empty_lvol() {
	# Create lvs
	malloc_name=$(rpc_cmd bdev_malloc_create 80 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol with 10 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * "$NUM_CLUSTERS"))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0

	# Expected map: 00000000 00000000
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "$NUM_CLUSTERS" 0 "AAA="

	# Stop nbd device
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_fragmap_data_hole() {
	# Create lvs
	malloc_name=$(rpc_cmd bdev_malloc_create 80 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol with 10 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * "$NUM_CLUSTERS"))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0

	# Expected map: 00000001 00000000 (1st cluster is wriiten)

	# Read entire fragmap
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=4096 count=1
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "$NUM_CLUSTERS" "1" "AQA="

	# Read fragmap [0, 5) clusters
	size=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset 0 --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "1" "AQ=="

	# Read fragmap [5, 10) clusters
	offset=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset $offset --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "0" "AA=="

	# Stop nbd device
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_fragmap_hole_data() {
	# Create lvs
	malloc_name=$(rpc_cmd bdev_malloc_create 80 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol with 10 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * "$NUM_CLUSTERS"))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0

	# Expected map: 00000000 00000010 (10th cluster is wriiten)

	# Read entire fragmap
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=4096 count=1 seek=9216
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "$NUM_CLUSTERS" "1" "AAI="

	# Read fragmap [0, 5) clusters
	size=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset 0 --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "0" "AA=="

	# Read fragmap [5, 10) clusters
	offset=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset $offset --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "1" "EA=="

	# Stop nbd device
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_fragmap_hole_data_hole() {
	# Create lvs
	malloc_name=$(rpc_cmd bdev_malloc_create 80 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol with 10 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * "$NUM_CLUSTERS"))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0

	# Expected map: 01100000 00000000

	# Read entire fragmap
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=4096 count=2048 seek=5120
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "$NUM_CLUSTERS" "2" "YAA="

	# Read fragmap [0, 5) clusters
	size=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset 0 --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "0" "AA=="

	# Read fragmap [5, 10) clusters
	offset=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset $offset --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "2" "Aw=="

	# Stop nbd device
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_fragmap_data_hole_data() {
	# Create lvs
	malloc_name=$(rpc_cmd bdev_malloc_create 80 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol with 10 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * "$NUM_CLUSTERS"))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0

	# Expected map: 10000111 00000011

	# Read entire fragmap
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=4096 count=3072 seek=0
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=4096 count=3072 seek=7168
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "$NUM_CLUSTERS" "6" "hwM="

	# Read fragmap [0, 5) clusters
	size=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset 0 --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "3" "Bw=="

	# Read fragmap [5, 10) clusters
	offset=$((LVS_DEFAULT_CLUSTER_SIZE_BTYE*5))
	fragmap=$(rpc_cmd bdev_lvol_get_fragmap --offset $offset --size $size $lvol_uuid)
	verify "$fragmap" "$LVS_DEFAULT_CLUSTER_SIZE_BTYE" "5" "3" "HA=="

	# Stop nbd device
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}


$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_fragmap_empty_lvol" test_fragmap_empty_lvol
run_test "test_fragmap_data_hole" test_fragmap_data_hole
run_test "test_fragmap_hole_data" test_fragmap_hole_data
rrun_test "test_fragmap_hole_data_hole" test_fragmap_hole_data_hole
run_test "test_fragmap_data_hole_data" test_fragmap_data_hole_data

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid