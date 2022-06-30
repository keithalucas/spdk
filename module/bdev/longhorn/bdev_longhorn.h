/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SPDK_BDEV_RAID_INTERNAL_H
#define SPDK_BDEV_RAID_INTERNAL_H

#include <stdatomic.h>
#include "spdk/bdev_module.h"

/*
 * Raid state describes the state of the longhorn. This longhorn bdev can be either in
 * configured list or configuring list
 */
enum longhorn_bdev_state {
	/* longhorn bdev is ready and is seen by upper layers */
	RAID_BDEV_STATE_ONLINE,

	/*
	 * longhorn bdev is configuring, not all underlying bdevs are present.
	 * And can't be seen by upper layers.
	 */
	RAID_BDEV_STATE_CONFIGURING,

	/*
	 * In offline state, longhorn bdev layer will complete all incoming commands without
	 * submitting to underlying base nvme bdevs
	 */
	RAID_BDEV_STATE_OFFLINE,

	/* longhorn bdev max, new states should be added before this */
	RAID_BDEV_MAX
};

enum longhorn_base_bdev_state {
	LONGHORN_BASE_BDEV_RW,
	LONGHORN_BASE_BDEV_WO,
	LONGHORN_BASE_BDEV_ERR
};

enum longhorn_pause_operation {
	LONGHORN_PAUSE_OP_NONE,
	LONGHORN_PAUSE_OP_SNAPSHOT,
	LONGHORN_PAUSE_OP_ADD,
};

/*
 * longhorn_base_bdev_info contains information for the base bdevs which are part of some
 * longhorn. This structure contains the per base bdev information. Whatever is
 * required per base device for longhorn bdev will be kept here
 */
struct longhorn_base_bdev_info {
	/* pointer to base spdk bdev */
	struct spdk_bdev	*bdev;

	/* pointer to base bdev descriptor opened by longhorn bdev */
	struct spdk_bdev_desc	*desc;


	//struct spdk_io_channel  *base_channel;

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool			remove_scheduled;

	/* thread where base device is opened */
	struct spdk_thread	*thread;

	enum longhorn_base_bdev_state state;

	bool is_local;
	char *lvs;
	char *remote_addr;
	uint16_t nvmf_port;
	uint16_t comm_port;
	char *bdev_name;
	char *remote_nqn;
	
	TAILQ_ENTRY(longhorn_base_bdev_info) infos;
};

/*
 * longhorn_bdev_io is the context part of bdev_io. It contains the information
 * related to bdev_io for a longhorn bdev
 */
struct longhorn_bdev_io {
	/* The longhorn bdev associated with this IO */
	struct longhorn_bdev *longhorn_bdev;

	/* WaitQ entry, used only in waitq logic */
	struct spdk_bdev_io_wait_entry	waitq_entry;

	/* Context of the original channel for this IO */
	struct longhorn_bdev_io_channel	*longhorn_ch;

	/* Used for tracking progress on io requests sent to member disks. */
	uint64_t			base_bdev_io_remaining;
	uint8_t				base_bdev_io_submitted;
	uint8_t				base_bdev_io_status;

	bool submitted;

};

TAILQ_HEAD(base_bdevs, longhorn_base_bdev_info);

struct longhorn_base_io_channel {
	struct spdk_io_channel	*base_channel;

	struct longhorn_base_bdev_info *base_info;

	TAILQ_ENTRY(longhorn_base_io_channel) channels;
};

/*
 * longhorn_bdev_io_channel is the context of spdk_io_channel for longhorn bdev device. It
 * contains the relationship of longhorn bdev io channel with base bdev io channels.
 */
struct longhorn_bdev_io_channel {
	struct longhorn_bdev *longhorn_bdev;
	/* Array of IO channels of base bdevs */
	//struct spdk_io_channel	**base_channel;

	/* Number of IO channels */
	uint8_t			num_channels;

	struct spdk_thread *thread;
	bool paused;
	bool pause_complete;
	bool deleted;

	atomic_int                      io_ops;

	TAILQ_HEAD(, longhorn_base_io_channel) base_channels;

	TAILQ_ENTRY(longhorn_bdev_io_channel) channels;

	struct longhorn_base_io_channel *last_read_io_ch;

	TAILQ_HEAD(, spdk_bdev_io_wait_entry)   io_wait_queue;

	uint32_t queue_len;
};

TAILQ_HEAD(io_channels, longhorn_bdev_io_channel);

typedef void (*longhorn_pause_cb)(struct longhorn_bdev *bdev, void *arg);
struct longhorn_pause_cb_entry {
	longhorn_pause_cb cb_fn;
	void *cb_arg;

	TAILQ_ENTRY(longhorn_pause_cb_entry) link;
};

/*
 * longhorn_bdev is the single entity structure which contains SPDK block device
 * and the information related to any longhorn bdev either configured or
 * in configuring list. io device is created on this.
 */
struct longhorn_bdev {
	/* longhorn bdev device, this will get registered in bdev layer */
	struct spdk_bdev		bdev;
	char *name; 
	char *address;
	char *nqn;

	/* link of longhorn bdev to link it to configured, configuring or offline list */
	TAILQ_ENTRY(longhorn_bdev)		state_link;

	/* link of longhorn bdev to link it to global longhorn bdev list */
	TAILQ_ENTRY(longhorn_bdev)		global_link;

	TAILQ_HEAD(, longhorn_pause_cb_entry) pause_cbs;

	/* pointer to config file entry */
	struct longhorn_bdev_config		*config;


	/* array of base bdev info */
	struct longhorn_base_bdev_info	*base_bdev_info;

	pthread_mutex_t                 base_bdevs_mutex;
	struct base_bdevs               base_bdevs_head;
	struct io_channels              io_channel_head;

	uint32_t num_io_channels;

	struct longhorn_base_bdev_info  *last_read_info;

	/* block length bit shift for optimized calculation */
	uint32_t			blocklen_shift;

	/* state of longhorn bdev */
	enum longhorn_bdev_state		state;

	/* number of base bdevs comprising longhorn bdev  */
	uint8_t				num_base_bdevs;

	/* number of base bdevs discovered */
	uint8_t				num_base_bdevs_discovered;

	/* Set to true if destruct is called for this longhorn bdev */
	bool				destruct_called;

	/* Set to true if destroy of this longhorn bdev is started. */
	bool				destroy_started;

	bool op_in_progress;
	enum longhorn_pause_operation   pause_op;


	bool configured;

	atomic_int                      io_ops;
	atomic_int                      channels_to_pause;

};

/* TAIL heads for various longhorn bdev lists */
TAILQ_HEAD(longhorn_configured_tailq, longhorn_bdev);
TAILQ_HEAD(longhorn_configuring_tailq, longhorn_bdev);
TAILQ_HEAD(longhorn_all_tailq, longhorn_bdev);
TAILQ_HEAD(longhorn_offline_tailq, longhorn_bdev);

extern struct longhorn_configured_tailq	g_longhorn_bdev_configured_list;
extern struct longhorn_configuring_tailq	g_longhorn_bdev_configuring_list;
extern struct longhorn_all_tailq		g_longhorn_bdev_list;
extern struct longhorn_offline_tailq	g_longhorn_bdev_offline_list;
extern struct longhorn_config		g_longhorn_config;

typedef void (*longhorn_bdev_destruct_cb)(void *cb_ctx, int rc);

int longhorn_bdev_create(const char *name, const char *address, uint8_t num_base_bdevs);
int longhorn_bdev_add_base_devices(struct longhorn_bdev_config *longhorn_cfg);
void longhorn_bdev_remove_base_devices(const char *longhorn_name,
				   longhorn_bdev_destruct_cb cb_fn, void *cb_ctx);
int longhorn_bdev_config_add(const char *longhorn_name, uint8_t num_base_bdevs,
			 struct longhorn_bdev_config **_longhorn_cfg);
int longhorn_bdev_config_add_base_bdev(struct longhorn_bdev_config *longhorn_cfg,
				   const char *base_bdev_name, uint8_t slot);
void longhorn_bdev_config_cleanup(struct longhorn_bdev_config *longhorn_cfg);
struct longhorn_bdev_config *longhorn_bdev_config_find_by_name(const char *longhorn_name);


bool
longhorn_bdev_io_complete_part(struct longhorn_bdev_io *longhorn_io, uint64_t completed,
			   enum spdk_bdev_io_status status);
void
longhorn_bdev_queue_io_wait(struct longhorn_bdev_io *longhorn_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn);
void
longhorn_bdev_io_complete(struct longhorn_bdev_io *longhorn_io, enum spdk_bdev_io_status status);

int longhorn_bdev_add_base_device(struct longhorn_bdev *longhorn_bdev, struct longhorn_base_bdev_info *base_info);
int longhorn_bdev_remove_replica(char *name, char *lvs, char *addr, uint16_t nvmf_port, uint16_t comm_port);
int
longhorn_bdev_add_replica(const char *name, char *lvs, char *addr, uint16_t nvmf_port, uint16_t comm_port, enum longhorn_base_bdev_state state);

void bdev_longhorn_pause_io(void *cb_arg);
void bdev_longhorn_unpause_io(void *cb_arg);
struct longhorn_bdev *longhorn_bdev_find_by_name(const char *longhorn_name);

int longhorn_unpause(struct longhorn_bdev *longhorn_bdev);
void longhorn_volume_add_pause_cb(struct longhorn_bdev *longhorn_dev,
                                  longhorn_pause_cb cb_fn,
                                  void *cb_arg);
int longhorn_volume_add_replica(char *name, char *lvs, char *addr, uint16_t nvmf_port, uint16_t comm_port);
int
longhorn_bdev_dump_info_json(struct longhorn_bdev *bdev, struct spdk_json_write_ctx *w);

#endif /* SPDK_BDEV_RAID_INTERNAL_H */
