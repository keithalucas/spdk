#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

function test_snapshot_compare_with_lvol_bdev() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create two lvol bdevs
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 6)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	lvol_uuid1=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test1 "$lvol_size_mb" -t)
	lvol_uuid2=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test2 "$lvol_size_mb")

	# Fill thin provisioned lvol bdev with 50% of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid1" /dev/nbd0
	count=$((lvol_size / LVS_DEFAULT_CLUSTER_SIZE / 2))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	# Fill whole thick provisioned lvol bdev
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid2" /dev/nbd0
	count=$((lvol_size / LVS_DEFAULT_CLUSTER_SIZE))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots of lvol bdevs
	snapshot_uuid1=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test1 lvol_snapshot1)
	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test2 lvol_snapshot2)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid1" /dev/nbd0
	# Try to perform write operation on created snapshot
	# Check if filling snapshot of lvol bdev fails
	count=$((lvol_size / LVS_DEFAULT_CLUSTER_SIZE))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count && false
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Declare nbd devices as vars for an easy cross-reference
	local lvol_nbd1=/dev/nbd0 lvol_nbd2=/dev/nbd1
	local snapshot_nbd1=/dev/nbd2 snapshot_nbd2=/dev/nbd3

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid1" "$lvol_nbd1"
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid2" "$lvol_nbd2"
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid1" "$snapshot_nbd1"
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid2" "$snapshot_nbd2"
	# Compare every lvol bdev with corresponding snapshot and check that data are the same
	cmp "$lvol_nbd1" "$snapshot_nbd1"
	cmp "$lvol_nbd2" "$snapshot_nbd2"

	# Fill second half of thin provisioned lvol bdev
	count=$((lvol_size / LVS_DEFAULT_CLUSTER_SIZE / 2))
	dd if=/dev/urandom of="$lvol_nbd1" oflag=direct seek=$count bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count

	# Compare thin provisioned lvol bdev with its snapshot and check if it fails
	cmp "$lvol_nbd1" "$snapshot_nbd1" && false

	# clean up
	for bdev in "${!lvol_nbd@}" "${!snapshot_nbd@}"; do
		nbd_stop_disks "$DEFAULT_RPC_ADDR" "${!bdev}"
	done

	rpc_cmd bdev_lvol_delete "$lvol_uuid1"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid1" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid1"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid1" && false
	rpc_cmd bdev_lvol_delete "$lvol_uuid2"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid2" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid2"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Check that when writing to lvol bdev
# creating snapshot ends with success
function test_create_snapshot_with_io() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol bdev
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 2)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)

	# Run fio in background that writes to lvol bdev
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc" "--time_based --runtime=16" &
	fio_proc=$!
	sleep 4
	# Create snapshot of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)
	wait $fio_proc

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Check that creating snapshot of snapshot will fail
function test_create_snapshot_of_snapshot() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol bdev
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 3)))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Create snapshot of previously created snapshot
	# and check if operation will fail
	rpc_cmd bdev_lvol_snapshot lvs_test/lvol_snapshot lvol_snapshot2 && false

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Check if only clone of snapshot can be created.
# Check if writing to one clone doesn't affect other clone
# Check if relations between clones and snapshots are properly set in configuration
function test_clone_snapshot_relations() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 6)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Fill lvol bdev with 100% of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# An attempt to create a clone from lvol that is rw capable should fail
	rpc_cmd bdev_lvol_clone lvs_test/lvol_test clone_test && false

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Create again clone of lvol bdev and check if it fails
	rpc_cmd bdev_lvol_clone lvs_test/lvol_test clone_test && false

	# Create two clones of snapshot and check if it ends with success
	clone_uuid1=$(rpc_cmd bdev_lvol_clone lvs_test/lvol_snapshot clone_test1)
	clone_uuid2=$(rpc_cmd bdev_lvol_clone lvs_test/lvol_snapshot clone_test2)

	# Perform write operation to first clone
	# Change first half of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$clone_uuid1" /dev/nbd0
	fill_size=$((lvol_size / 2))
	run_fio_test /dev/nbd0 0 $fill_size "write" "0xaa"

	# Compare snapshot with second clone. Data on both bdevs should be the same
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid" /dev/nbd1
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$clone_uuid2" /dev/nbd2
	sleep 1
	cmp /dev/nbd1 /dev/nbd2
	# Compare snapshot with first clone
	cmp /dev/nbd0 /dev/nbd1 && false

	snapshot_bdev=$(rpc_cmd bdev_get_bdevs -b "lvs_test/lvol_snapshot")
	clone_bdev1=$(rpc_cmd bdev_get_bdevs -b "lvs_test/clone_test1")
	clone_bdev2=$(rpc_cmd bdev_get_bdevs -b "lvs_test/lvol_test")

	# Check snapshot consistency
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$snapshot_bdev")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot_bdev")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clones|sort' <<< "$snapshot_bdev")" = "$(jq '.|sort' <<< '["lvol_test", "clone_test1", "clone_test2"]')" ]

	# Check first clone consistency
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$clone_bdev1")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$clone_bdev1")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$clone_bdev1")" = '"lvol_snapshot"' ]

	# Check second clone consistency
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$clone_bdev2")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$clone_bdev2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$clone_bdev2")" = '"lvol_snapshot"' ]

	# Destroy first clone and check if it is deleted from snapshot
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$clone_uuid1"
	snapshot_bdev=$(rpc_cmd bdev_get_bdevs -b "lvs_test/lvol_snapshot")
	[ "$(jq '.[].driver_specific.lvol.clones|sort' <<< "$snapshot_bdev")" = "$(jq '.|sort' <<< '["lvol_test", "clone_test2"]')" ]

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete "$clone_uuid2"
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Testing usage of bdev_lvol_inflate
function test_clone_inflate() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 4)))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Fill lvol bdev with 100% of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $((lvol_size_mb * 1024 * 1024)) "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Create clone of snapshot
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]

	# Fill part of clone with data of known pattern
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	first_fill=0
	second_fill=$((lvol_size_mb * 1024 * 1024 * 3 / 4))
	run_fio_test /dev/nbd0 $first_fill $((1024 * 1024)) "write" "0xdd"
	run_fio_test /dev/nbd0 $second_fill $((1024 * 1024)) "write" "0xdd"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Do inflate
	rpc_cmd bdev_lvol_inflate lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "false" ]

	# Delete snapshot
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"

	# Check data consistency
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 $first_fill $((1024 * 1024)) "read" "0xdd"
	run_fio_test /dev/nbd0 $(((first_fill + 1) * 1024 * 1024)) $((second_fill - 1024 * 1024)) "read" "0xcc"
	run_fio_test /dev/nbd0 $second_fill $((1024 * 1024)) "read" "0xdd"
	run_fio_test /dev/nbd0 $((second_fill + 1024 * 1024)) $((lvol_size_mb * 1024 * 1024 - (second_fill + 1024 * 1024))) "read" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Create chain of snapshot<-snapshot2<-lvol_test lvol bdevs.
# Decouple lvol_test twice and delete the remaining snapshot lvol.
# Each time check consistency of snapshot-clone relations and written data.
function test_clone_decouple_parent() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$((5 * LVS_DEFAULT_CLUSTER_SIZE_MB))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Decouple_parent should fail on lvol bdev without a parent
	rpc_cmd bdev_lvol_decouple_parent lvs_test/lvol_test && false

	# Fill first four out of 5 clusters of clone with data of known pattern
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	begin_fill=0
	end_fill=$((lvol_size_mb * 4 * 1024 * 1024 / 5))
	run_fio_test /dev/nbd0 $begin_fill $end_fill "write" "0xdd"

	# Create snapshot (snapshot<-lvol_bdev)
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Fill second and fourth cluster of clone with data of known pattern
	start_fill=$((lvol_size_mb * 1024 * 1024 / 5))
	fill_range=$start_fill
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xcc"
	start_fill=$((lvol_size_mb * 3 * 1024 * 1024 / 5))
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xcc"

	# Create snapshot (snapshot<-snapshot2<-lvol_bdev)
	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot2)

	# Fill second cluster of clone with data of known pattern
	start_fill=$fill_range
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xee"

	# Check data consistency
	pattern=("0xdd" "0xee" "0xdd" "0xcc" "0x00")
	for i in "${!pattern[@]}"; do
		start_fill=$((lvol_size_mb * i * 1024 * 1024 / 5))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" "${pattern[i]}"
	done

	# Decouple_parent of lvol bdev resulting in two relation chains:
	#  - snapshot<-lvol_bdev
	#  - snapshot<-snapshot2
	rpc_cmd bdev_lvol_decouple_parent lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	snapshot2=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$snapshot2")" = "true" ]

	# Delete second snapshot
	rpc_cmd bdev_lvol_delete "$snapshot_uuid2"

	# Check data consistency
	for i in "${!pattern[@]}"; do
		start_fill=$((lvol_size_mb * i * 1024 * 1024 / 5))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" "${pattern[i]}"
	done

	# Decouple_parent of lvol bdev again resulting in two relation chains:
	#  - lvol_bdev
	#  - snapshot<-snapshot2
	rpc_cmd bdev_lvol_decouple_parent lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot")" = "false" ]

	# Delete first snapshot
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"

	# Check data consistency
	for i in "${!pattern[@]}"; do
		start_fill=$((lvol_size_mb * i * 1024 * 1024 / 5))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" "${pattern[i]}"
	done

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Create chain of snapshot<-snapshot2<-lvol_test lvol bdevs.
# Detach lvol_test twice and delete the remaining snapshot lvol.
# Each time check consistency of snapshot-clone relations and written data.
function test_clone_detach_parent() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$((5 * LVS_DEFAULT_CLUSTER_SIZE_MB))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Detach_parent should fail on lvol bdev without a parent
	rpc_cmd bdev_lvol_detach_parent lvs_test/lvol_test && false

	# Fill first four out of 5 clusters of clone with data of known pattern
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	begin_fill=0
	end_fill=$((lvol_size_mb * 4 * 1024 * 1024 / 5))
	run_fio_test /dev/nbd0 $begin_fill $end_fill "write" "0xdd"

	# Create snapshot (snapshot<-lvol_bdev)
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Fill second and fourth cluster of clone with data of known pattern
	start_fill=$((lvol_size_mb * 1024 * 1024 / 5))
	fill_range=$start_fill
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xcc"
	start_fill=$((lvol_size_mb * 3 * 1024 * 1024 / 5))
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xcc"

	# Create snapshot (snapshot<-snapshot2<-lvol_bdev)
	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot2)

	# Fill second cluster of clone with data of known pattern
	start_fill=$fill_range
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xee"

	# Check data consistency
	pattern=("0xdd" "0xee" "0xdd" "0xcc" "0x00")
	for i in "${!pattern[@]}"; do
		start_fill=$((lvol_size_mb * i * 1024 * 1024 / 5))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" "${pattern[i]}"
	done

	# Detach_parent of lvol bdev resulting in two relation chains:
	#  - snapshot<-lvol_bdev
	#  - snapshot<-snapshot2
	rpc_cmd bdev_lvol_detach_parent lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	snapshot2=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$snapshot2")" = "true" ]

	# Delete second snapshot
	rpc_cmd bdev_lvol_delete "$snapshot_uuid2"

	# Check data consistency
	pattern=("0xdd" "0xee" "0xdd" "0xdd" "0x00")
	for i in "${!pattern[@]}"; do
		start_fill=$((lvol_size_mb * i * 1024 * 1024 / 5))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" "${pattern[i]}"
	done

	# Detach_parent of lvol bdev again resulting in two relation chains:
	#  - lvol_bdev
	#  - snapshot<-snapshot2
	rpc_cmd bdev_lvol_detach_parent lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot")" = "false" ]

	# Delete first snapshot
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"

	# Check data consistency
	pattern=("0x00" "0xee" "0x00" "0x00" "0x00")
	for i in "${!pattern[@]}"; do
		start_fill=$((lvol_size_mb * i * 1024 * 1024 / 5))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" "${pattern[i]}"
	done

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Set lvol bdev as read only and perform clone on it.
function test_lvol_bdev_readonly() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 2)))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Set lvol bdev as read only
	rpc_cmd bdev_lvol_set_read_only lvs_test/lvol_test

	# Try to perform write operation on lvol marked as read only
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc" && false
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create clone of lvol set to read only
	clone_uuid=$(rpc_cmd bdev_lvol_clone lvs_test/lvol_test clone_test)

	# Try to perform write operation on lvol clone
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$clone_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Clean up
	rpc_cmd bdev_lvol_delete "$clone_uuid"
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Check if it is possible to delete snapshot with clone
function test_delete_snapshot_with_clone() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 2)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Perform write operation on lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc"

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Fill first half of lvol bdev
	half_size=$((lvol_size / 2 - 1))
	run_fio_test /dev/nbd0 0 $half_size "write" "0xee"

	# Check if snapshot was unchanged
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid" /dev/nbd1
	run_fio_test /dev/nbd1 0 $half_size "read" "0xcc"

	# Verify lvol bdev
	run_fio_test /dev/nbd0 0 $half_size "read" "0xee"
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "true" ]

	# Delete snapshot - should succeed
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"

	# Check data consistency
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "false" ]
	run_fio_test /dev/nbd0 0 $half_size "read" "0xee"
	run_fio_test /dev/nbd0 $((half_size + 1)) $half_size "read" "0xcc"

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Check if it is possible to delete snapshot with one snapshot on it
function test_delete_snapshot_with_snapshot() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 5)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Perform write operation on lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc"

	# Create snapshot of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$lvol")" = '"lvol_snapshot"' ]

	# Fill second 1/3 of lvol bdev
	first_part=$((lvol_size / 3))
	second_part=$((lvol_size * 2 / 3))
	run_fio_test /dev/nbd0 $first_part $((second_part - first_part)) "write" "0xee"

	# Check if snapshot was unchanged
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid" /dev/nbd1
	run_fio_test /dev/nbd1 0 $lvol_size "read" "0xcc"

	# Create second snapshot of lvol_bdev
	# First snapshot becomes snapshot of second snapshot
	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot2)
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	snapshot2=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2")
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$snapshot2")" = '"lvol_snapshot"' ]
	[ "$(jq '.[].driver_specific.lvol.clones|sort' <<< "$snapshot2")" = "$(jq '.|sort' <<< '["lvol_test"]')" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$snapshot2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clones|sort' <<< "$snapshot")" = "$(jq '.|sort' <<< '["lvol_snapshot2"]')" ]

	# Verify snapshots
	run_fio_test /dev/nbd1 0 $size "read" "0xcc"
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid2" /dev/nbd2
	run_fio_test /dev/nbd2 0 $((first_part - 1)) "read" "0xcc"
	run_fio_test /dev/nbd2 $first_part $((second_part - first_part)) "read" "0xee"
	run_fio_test /dev/nbd2 $second_part $((lvol_size - second_part)) "read" "0xcc"

	# Verify lvol bdev
	run_fio_test /dev/nbd0 $first_part $((second_part - first_part)) "read" "0xee"
	run_fio_test /dev/nbd0 $second_part $((lvol_size - second_part)) "read" "0xcc"
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$lvol")" = '"lvol_snapshot2"' ]

	# Fill third part of lvol bdev
	run_fio_test /dev/nbd0 $second_part $((lvol_size - second_part)) "write" "0xdd"

	# Verify snapshots
	run_fio_test /dev/nbd1 0 $size "read" "0xcc"
	run_fio_test /dev/nbd0 $second_part $((lvol_size - second_part)) "read" "0xdd"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1

	# Delete snapshot - should succeed
	rpc_cmd bdev_lvol_delete "$snapshot_uuid2"

	# Check data consistency
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$lvol")" = '"lvol_snapshot"' ]
	[ "$(jq '.[].driver_specific.lvol.clones|sort' <<< "$snapshot")" = "$(jq '.|sort' <<< '["lvol_test"]')" ]
	run_fio_test /dev/nbd0 $first_part $((second_part - first_part)) "read" "0xee"
	run_fio_test /dev/nbd0 $second_part $((lvol_size - second_part)) "read" "0xdd"

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Test for destroying lvol bdevs in particular order.
function test_bdev_lvol_delete_ordering() {
	local snapshot_name=snapshot snapshot_uuid
	local clone_name=clone clone_uuid

	local bdev_uuid
	local lbd_name=lbd_test
	local lvstore_uuid lvstore_name=lvs_name
	local malloc_dev
	local size

	malloc_dev=$(rpc_cmd bdev_malloc_create 256 "$MALLOC_BS")
	lvstore_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_dev" "$lvstore_name")

	get_lvs_jq bdev_lvol_get_lvstores -u "$lvstore_uuid"
	[[ ${jq_out["uuid"]} == "$lvstore_uuid" ]]
	[[ ${jq_out["name"]} == "$lvstore_name" ]]
	[[ ${jq_out["base_bdev"]} == "$malloc_dev" ]]

	size=$((jq_out["free_clusters"] * jq_out["cluster_size"] / 4 / 1024 ** 2))

	bdev_uuid=$(rpc_cmd bdev_lvol_create -t -u "$lvstore_uuid" "$lbd_name" "$size")

	get_bdev_jq bdev_get_bdevs -b "$bdev_uuid"

	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot "${jq_out["name"]}" "$snapshot_name")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$snapshot_name"
	[[ ${jq_out["name"]} == "$snapshot_uuid" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$snapshot_name" ]]

	clone_uuid=$(rpc_cmd bdev_lvol_clone "$lvstore_name/$snapshot_name" "$clone_name")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$clone_name"
	[[ ${jq_out["name"]} == "$clone_uuid" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$clone_name" ]]

	# Try to destroy snapshot with clones and check if it fails
	rpc_cmd bdev_lvol_delete "$snapshot_uuid" && false

	# cleanup logical volumes
	rpc_cmd bdev_lvol_delete "$bdev_uuid"
	rpc_cmd bdev_lvol_delete "$clone_uuid"
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"

	# cleanup lvstore
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvstore_uuid"

	# cleanup malloc dev
	rpc_cmd bdev_malloc_delete "$malloc_dev"

	check_leftover_devices
}

function test_lvol_set_parent_from_snapshot() {
	local vol_size_mb=20
	local vol_size=$((vol_size_mb * 1024 * 1024))
	local vol_blocks_count=$((vol_size / MALLOC_BS))
	local three_clusters_size=$((LVS_DEFAULT_CLUSTER_SIZE * 3))
	local three_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 3 / MALLOC_BS))
	local two_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 2 / MALLOC_BS))

	# Create the lvstore on a malloc device.
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create volume: lvol1
	# New state:
	#    lvol1
	lvol1_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol1 "$vol_size_mb")

	# Perform write operation over lvol1
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol1_uuid" /dev/nbd1
	run_fio_test /dev/nbd1 0 $vol_size "write" "0xaa"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	sleep 1

	# Create a temp volume: lvol2_temp
	# New state:
	#    lvol1
	#    lvol2_temp
	lvol2_temp_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol2_temp "$vol_size_mb")

	# Perform write operation over lvol2_temp
	# Calculate md5 of last 2 clusters of lvol2_temp
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_temp_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $vol_size "write" "0xbb"
	md5_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Make a snapshot of lvol2_temp: snap2
	# New state:
	#    lvol1
	#    snap2  <-- lvol2_temp
	snap2_uuid=$(rpc_cmd bdev_lvol_snapshot "$lvol2_temp_uuid" snap2)

	# Make a snapshot of lvol1: snap1
	# New state:
	#    snap1  <-- lvol1
	#    snap2  <-- lvol2_temp
	snap1_uuid=$(rpc_cmd bdev_lvol_snapshot "$lvol1_uuid" snap1)

	# Create another clone of snap1: lvol2
	# New state:
	#    snap1  <-- lvol1
	#          `<-- lvol2
	#    snap2  <-- lvol2_temp
	lvol2_uuid=$(rpc_cmd bdev_lvol_clone "$snap1_uuid" lvol2)

	# Perform write operation over the first 3 clusters of lvol2
	# Calculate md5sum of the first 3 clusters and of last 2 clusters of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $three_clusters_size "write" "0xcc"
	md5_lvol2_1=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol2_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Change parent of lvol2
	# New state:
	#    snap1  <-- lvol1
	#    snap2  <-- lvol2_temp
	#          `<-- lvol2
	rpc_cmd bdev_lvol_set_parent "$lvol2_uuid" "$snap2_uuid"

	# Check lvol2 consistency
	clone_bdev=$(rpc_cmd bdev_get_bdevs -b "$lvol2_uuid")
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$clone_bdev")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$clone_bdev")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$clone_bdev")" = '"snap2"' ]
	[ "$(jq '.[].driver_specific.lvol.esnap_clone' <<< "$clone_bdev")" = "false" ]

	# Try again with aliases instead uuid
	NOT rpc_cmd bdev_lvol_set_parent lvs_test/lvol2 lvs_test/snap2

	# Delete lvol2_temp
	# New state:
	#    snap1  <-- lvol1
	#    snap2  <-- lvol2
	rpc_cmd bdev_lvol_delete "$lvol2_temp_uuid"

	# Calculate again md5 of the first 3 clusters and of last 2 clusters of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	md5_lvol2_1_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol2_2_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Check that first three clusters of lvol2 didn't change anche that the last 2 clusters changed
	[[ $md5_lvol2_1 == "$md5_lvol2_1_new" ]]
	[[ $md5_lvol2_2 != "$md5_lvol2_2_new" ]]
	[[ $md5_lvol2_2_new == "$md5_2" ]]

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol1_uuid"
	rpc_cmd bdev_lvol_delete "$snap1_uuid"
	rpc_cmd bdev_lvol_delete "$lvol2_uuid"
	rpc_cmd bdev_lvol_delete "$snap2_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_lvol_set_parent_from_esnap() {
	local vol_size_mb=20
	local vol_size=$((vol_size_mb * 1024 * 1024))
	local vol_blocks_count=$((vol_size / MALLOC_BS))
	local three_clusters_size=$((LVS_DEFAULT_CLUSTER_SIZE * 3))
	local three_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 3 / MALLOC_BS))
	local two_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 2 / MALLOC_BS))

	# Create the lvstore on a malloc device.
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create a bdev that will be the external snapshot
	# State:
	#    esnap1
	rpc_cmd bdev_malloc_create -b esnap1 "$vol_size_mb" $MALLOC_BS

	# Perform write operation over the external snapshot
	nbd_start_disks "$DEFAULT_RPC_ADDR" esnap1 /dev/nbd1
	run_fio_test /dev/nbd1 0 $vol_size "write" "0xaa"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	sleep 1

	# Create a temp volume: lvol2_temp
	# New state:
	#    esnap1
	#    lvol2_temp
	lvol2_temp_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol2_temp "$vol_size_mb")

	# Perform write operation over lvol2_temp
	# Calculate md5 of last 2 clusters of lvol2_temp
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_temp_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $vol_size "write" "0xbb"
	md5_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Make a snapshot of lvol2_temp: snap2
	# New state:
	#    esnap1
	#    snap2  <-- lvol2_temp
	snap2_uuid=$(rpc_cmd bdev_lvol_snapshot "$lvol2_temp_uuid" snap2)

	# Create an esnap clone: lvol2
	# New state:
	#    esnap1 <-- lvol2
	#    snap2  <-- lvol2_temp
	lvol2_uuid=$(rpc_cmd bdev_lvol_clone_bdev esnap1 lvs_test lvol2)

	# Perform write operation over the first 3 clusters of lvol2
	# Calculate md5sum of the first 3 clusters and of last 2 clusters of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $three_clusters_size "write" "0xcc"
	md5_lvol2_1=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol2_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Change parent of lvol2
	# New state:
	#    esnap1
	#    snap2  <-- lvol2
	#          `<-- lvol2_temp
	rpc_cmd bdev_lvol_set_parent "$lvol2_uuid" "$snap2_uuid"

	# Check lvol2 consistency
	clone_bdev=$(rpc_cmd bdev_get_bdevs -b "$lvol2_uuid")
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$clone_bdev")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$clone_bdev")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$clone_bdev")" = '"snap2"' ]
	[ "$(jq '.[].driver_specific.lvol.esnap_clone' <<< "$clone_bdev")" = "false" ]

	# Try again with aliases instead uuid
	NOT rpc_cmd bdev_lvol_set_parent lvs_test/lvol2 lvs_test/snap2

	# Delete lvol2_temp
	# New state:
	#    esnap1
	#    snap2  <-- lvol2
	rpc_cmd bdev_lvol_delete "$lvol2_temp_uuid"

	# Calculate again md5 of the first 3 clusters and of last 2 clusters of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	md5_lvol2_1_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol2_2_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Check that first three clusters of lvol2 didn't change anche that the last 2 clusters changed
	[[ $md5_lvol2_1 == "$md5_lvol2_1_new" ]]
	[[ $md5_lvol2_2 != "$md5_lvol2_2_new" ]]
	[[ $md5_lvol2_2_new == "$md5_2" ]]

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol2_uuid"
	rpc_cmd bdev_lvol_delete "$snap2_uuid"
	rpc_cmd bdev_malloc_delete esnap1
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_lvol_set_parent_from_none() {
	local vol_size_mb=20
	local vol_size=$((vol_size_mb * 1024 * 1024))
	local three_clusters_size=$((LVS_DEFAULT_CLUSTER_SIZE * 3))
	local three_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 3 / MALLOC_BS))
	local two_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 2 / MALLOC_BS))

	# Create the lvstore on a malloc device.
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create a temp volume: lvol2_temp
	# New state:
	#    lvol2_temp
	lvol2_temp_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol2_temp "$vol_size_mb")

	# Perform write operation over lvol2_temp
	# Calculate md5 of last 2 clusters of lvol2_temp
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_temp_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $vol_size "write" "0xbb"
	md5_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Make a snapshot of lvol2_temp: snap2
	# New state:
	#    snap2  <-- lvol2_temp
	snap2_uuid=$(rpc_cmd bdev_lvol_snapshot "$lvol2_temp_uuid" snap2)

	# Create another volume: lvol2
	# New state:
	#               lvol2
	#    snap2  <-- lvol2_temp
	lvol2_uuid=$(rpc_cmd bdev_lvol_create -t -u "$lvs_uuid" lvol2 "$vol_size_mb")

	# Perform write operation over the first 3 clusters of lvol2
	# Calculate md5sum of the first 3 clusters and of last 2 clusters of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $three_clusters_size "write" "0xcc"
	md5_lvol2_1=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol2_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Change parent of lvol2
	# New state:
	#    snap2  <-- lvol2_temp
	#          `<-- lvol2
	rpc_cmd bdev_lvol_set_parent "$lvol2_uuid" "$snap2_uuid"

	# Check lvol2 consistency
	clone_bdev=$(rpc_cmd bdev_get_bdevs -b "$lvol2_uuid")
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$clone_bdev")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$clone_bdev")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$clone_bdev")" = '"snap2"' ]
	[ "$(jq '.[].driver_specific.lvol.esnap_clone' <<< "$clone_bdev")" = "false" ]

	# Try again with aliases instead uuid
	NOT rpc_cmd bdev_lvol_set_parent lvs_test/lvol2 lvs_test/snap2

	# Delete lvol2_temp
	# New state:
	#    snap2  <-- lvol2
	rpc_cmd bdev_lvol_delete "$lvol2_temp_uuid"

	# Calculate again md5 of the first 3 clusters and of last 2 clusters of lvol2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol2_uuid" /dev/nbd2
	md5_lvol2_1_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol2_2_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Check that first three clusters of lvol2 didn't change anche that the last 2 clusters changed
	[[ $md5_lvol2_1 == "$md5_lvol2_1_new" ]]
	[[ $md5_lvol2_2 != "$md5_lvol2_2_new" ]]
	[[ $md5_lvol2_2_new == "$md5_2" ]]

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol2_uuid"
	rpc_cmd bdev_lvol_delete "$snap2_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_lvol_set_parent_failed() {
	local vol_size_mb=20

	# Create a lvstore on a malloc device.
	malloc1_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs1_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc1_name" lvs1_test)

	# Create another lvstore on another malloc device.
	malloc2_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs2_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc2_name" lvs2_test)

	# Create a volume on lvol store 1: lvol1
	lvol1_uuid=$(rpc_cmd bdev_lvol_create -t -u "$lvs1_uuid" lvol1 "$vol_size_mb")

	# Create a volume on lvol store 2: lvol2
	lvol2_uuid=$(rpc_cmd bdev_lvol_create -t -u "$lvs2_uuid" lvol2 "$vol_size_mb")

	# Make a snapshot of lvol2: snap2
	snap2_uuid=$(rpc_cmd bdev_lvol_snapshot "$lvol2_uuid" snap2)

	# The setting of snap2 as the parent of lvol2 must fail because they belong to different lvol stores
	NOT rpc_cmd bdev_lvol_set_parent "$lvol1_uuid" "$snap2_uuid"

	# Try to set parent with a parent that is not a lvol
	NOT rpc_cmd bdev_lvol_set_parent "$lvol1_uuid" "$malloc2_name"

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol1_uuid"
	rpc_cmd bdev_lvol_delete "$lvol2_uuid"
	rpc_cmd bdev_lvol_delete "$snap2_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs1_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs2_uuid"
	rpc_cmd bdev_malloc_delete "$malloc1_name"
	rpc_cmd bdev_malloc_delete "$malloc2_name"
	check_leftover_devices
}

function test_lvol_snapshot_checksum() {
	# Create lvs
	bs_malloc_name=$(rpc_cmd bdev_malloc_create 40 $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$bs_malloc_name" lvs_test)

	# Create lvol with 4 cluster
	lvol_size=$((LVS_DEFAULT_CLUSTER_SIZE_MB * 4))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size" -t)

	# Fill second cluster of lvol with data of known pattern
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	start_fill=$((lvol_size * 1024 * 1024 / 4))
	fill_range=$start_fill
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots1 of lvol bdev
	snapshot1_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot1)

	# Register snapshot's checksum
	rpc_cmd bdev_lvol_register_snapshot_checksum "$snapshot1_uuid"

	# Get snapshot1's checksum
	checksum=$(rpc_cmd bdev_lvol_get_snapshot_checksum "$snapshot1_uuid")
	[ "$(jq '.checksum' <<< "$checksum")" == "828947163827886523" ]

	# Fill fourth cluster of lvol with data of known pattern
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	start_fill=$((lvol_size * 3 * 1024 * 1024 / 4))
	fill_range=$((lvol_size * 1024 * 1024 / 4))
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xbb"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots2 of lvol bdev
	snapshot2_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot2)

	# Register snapshot2's checksum
	rpc_cmd bdev_lvol_register_snapshot_checksum "$snapshot2_uuid"

	# Get snapshot2's checksum
	checksum=$(rpc_cmd bdev_lvol_get_snapshot_checksum "$snapshot2_uuid")
	[ "$(jq '.checksum' <<< "$checksum")" == "7161606974051404010" ]

	# Delete snapshot1
	rpc_cmd bdev_lvol_delete "$snapshot1_uuid"

	# Snapshot2 checksum have been removed by previous operation
	NOT rpc_cmd bdev_lvol_get_snapshot_checksum "$snapshot2_uuid"

	# Calculate again snapshot2's checksum because it now contains also snapshot1's data
	rpc_cmd bdev_lvol_register_snapshot_checksum "$snapshot2_uuid"

	# Get snapshot2's checksum
	checksum=$(rpc_cmd bdev_lvol_get_snapshot_checksum "$snapshot2_uuid")
	[ "$(jq '.checksum' <<< "$checksum")" == "440587436939872369" ]

	# Clean up
	rpc_cmd bdev_lvol_delete "$snapshot2_uuid"
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$bs_malloc_name"
	check_leftover_devices
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_snapshot_compare_with_lvol_bdev" test_snapshot_compare_with_lvol_bdev
run_test "test_create_snapshot_with_io" test_create_snapshot_with_io
run_test "test_create_snapshot_of_snapshot" test_create_snapshot_of_snapshot
run_test "test_clone_snapshot_relations" test_clone_snapshot_relations
run_test "test_clone_inflate" test_clone_inflate
run_test "test_clone_decouple_parent" test_clone_decouple_parent
run_test "test_clone_detach_parent" test_clone_detach_parent
run_test "test_lvol_bdev_readonly" test_lvol_bdev_readonly
run_test "test_delete_snapshot_with_clone" test_delete_snapshot_with_clone
run_test "test_delete_snapshot_with_snapshot" test_delete_snapshot_with_snapshot
run_test "test_bdev_lvol_delete_ordering" test_bdev_lvol_delete_ordering
run_test "test_lvol_set_parent_from_snapshot" test_lvol_set_parent_from_snapshot
run_test "test_lvol_set_parent_from_esnap" test_lvol_set_parent_from_esnap
run_test "test_lvol_set_parent_from_none" test_lvol_set_parent_from_none
run_test "test_lvol_set_parent_failed" test_lvol_set_parent_failed
run_test "test_lvol_snapshot_checksum" test_lvol_snapshot_checksum

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
