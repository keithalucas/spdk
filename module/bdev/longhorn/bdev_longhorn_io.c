/*-
 *   BSD LICENSE
 *
 *   Copyright (c) SUSE
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
 *     * Neither the name of SUSE nor the names of its
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

#include "bdev_longhorn.h"
#include "bdev_longhorn_impl.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/log.h"



/*
 * brief:
 * longhorn_bdev_io_completion function is called by lower layers to notify longhorn
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdev io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context (parent longhorn_bdev_io)
 * returns:
 * none
 */
static void
longhorn_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct longhorn_bdev_io *longhorn_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		//SPDK_ERRLOG("io op success\n");
		longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		//SPDK_ERRLOG("io op failure\n");
		longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
longhorn_base_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct longhorn_bdev_io *longhorn_io = cb_arg;

	longhorn_bdev_io_complete_part(longhorn_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);

	spdk_bdev_free_io(bdev_io);
}

static void
_longhorn_submit_rw_request(void *_longhorn_io)
{
	struct longhorn_bdev_io *longhorn_io = _longhorn_io;

	longhorn_submit_rw_request(longhorn_io);
}

static struct longhorn_base_io_channel *
_longhorn_get_next_readable_base_channel(struct longhorn_bdev_io_channel *longhorn_ch) {
	/* Get next channel */
	if (longhorn_ch->last_read_io_ch) {
		longhorn_ch->last_read_io_ch = TAILQ_NEXT(longhorn_ch->last_read_io_ch, channels);
	}

	/* Search for a RW replica */
	while (longhorn_ch->last_read_io_ch && 
	       longhorn_ch->last_read_io_ch->base_info->state != LONGHORN_BASE_BDEV_RW) {
		longhorn_ch->last_read_io_ch = TAILQ_NEXT(longhorn_ch->last_read_io_ch, channels);
	}

	/* We found a RW replica before the end of the list */
	if (longhorn_ch->last_read_io_ch) {
		return longhorn_ch->last_read_io_ch;
	}
	     
	/* Search for a RW replica from the beginning of the list */
	longhorn_ch->last_read_io_ch = TAILQ_FIRST(&longhorn_ch->base_channels);

	while (longhorn_ch->last_read_io_ch && 
	       longhorn_ch->last_read_io_ch->base_info->state != LONGHORN_BASE_BDEV_RW) {
		longhorn_ch->last_read_io_ch = TAILQ_NEXT(longhorn_ch->last_read_io_ch, channels);
	}

	return longhorn_ch->last_read_io_ch;
}


static void
longhorn_submit_read_request(struct longhorn_bdev_io *longhorn_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(longhorn_io);
	struct longhorn_bdev_io_channel	*longhorn_ch = longhorn_io->longhorn_ch;
	struct longhorn_bdev		*longhorn_bdev = longhorn_io->longhorn_bdev;
	int				ret = 0;
	struct longhorn_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;
	struct longhorn_base_io_channel	*base_channel;

	assert(longhorn_ch != NULL);

	base_channel = _longhorn_get_next_readable_base_channel(longhorn_ch);

	if (!base_channel) {
                SPDK_ERRLOG("bdev io submit with no base devices, it should not happen\n");
		return;
	}

	base_ch = base_channel->base_channel;
	base_info = base_channel->base_info;

	//SPDK_ERRLOG("longhorn_submit_read_request base_info %p\n", base_info);

        ret = spdk_bdev_readv_blocks(base_info->desc, base_ch,
                                      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
                                      bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, longhorn_bdev_io_completion,
                                      longhorn_io);


      
        if (ret == -ENOMEM) {
                longhorn_bdev_queue_io_wait(longhorn_io, base_info->bdev, base_ch,
                                        _longhorn_submit_rw_request);
		return;
        } 

#if 0
	atomic_fetch_add(&longhorn_bdev->io_ops, 1);
	atomic_fetch_add(&longhorn_ch->io_ops, 1);
	longhorn_io->submitted = true;
#endif
	
	if (ret != 0) {
                SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
                assert(false);
                longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
        }
}

static void
longhorn_submit_write_request(struct longhorn_bdev_io *longhorn_io)
{

	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(longhorn_io);
	struct longhorn_bdev_io_channel	*longhorn_ch = longhorn_io->longhorn_ch;
	struct longhorn_bdev		*longhorn_bdev = longhorn_io->longhorn_bdev;
	//uint8_t				pd_idx;
	int				ret = 0;
	struct longhorn_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;
	struct longhorn_base_io_channel *base_channel;

	assert(longhorn_ch != NULL);


	if (longhorn_io->base_bdev_io_remaining == 0) {
		longhorn_io->base_bdev_io_remaining = longhorn_ch->num_channels;
	}


	TAILQ_FOREACH(base_channel, &longhorn_ch->base_channels, channels) {
                base_ch = base_channel->base_channel;
		base_info = base_channel->base_info;
                
		ret = spdk_bdev_writev_blocks(base_info->desc, base_ch,
					      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					      bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, longhorn_base_io_complete,
					      longhorn_io);
               
                if (ret == -ENOMEM) {
                        SPDK_ERRLOG("enqueuing bdev io submit due to ENOMEM\n");
                        longhorn_bdev_queue_io_wait(longhorn_io, base_info->bdev, base_ch,
                                                _longhorn_submit_rw_request);
                } else if (ret != 0) {
                        SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
                        assert(false);
                        longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
                }

		atomic_fetch_add(&longhorn_bdev->io_ops, 1);
		atomic_fetch_add(&longhorn_ch->io_ops, 1);
		longhorn_io->submitted = true;

        }

}
/*
 * brief:
 * longhorn_submit_rw_request function is used to submit I/O to the correct
 * member disk for longhorn bdevs.
 * params:
 * longhorn_io
 * returns:
 * none
 */
void
longhorn_submit_rw_request(struct longhorn_bdev_io *longhorn_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(longhorn_io);

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
                longhorn_submit_read_request(longhorn_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
                longhorn_submit_write_request(longhorn_io);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
		assert(0);
	}
}


static void
_longhorn_submit_null_payload_request(void *_longhorn_io)
{
	struct longhorn_bdev_io *longhorn_io = _longhorn_io;

	longhorn_submit_null_payload_request(longhorn_io);
}


/*
 * brief:
 * longhorn_submit_null_payload_request function submits the next batch of
 * io requests with range but without payload, like FLUSH and UNMAP, to member disks;
 * it will submit as many as possible unless one base io request fails with -ENOMEM,
 * in which case it will queue itself for later submission.
 * params:
 * bdev_io - pointer to parent bdev_io on longhorn bdev device
 * returns:
 * none
 */
void
longhorn_submit_null_payload_request(struct longhorn_bdev_io *longhorn_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(longhorn_io);
	struct longhorn_bdev_io_channel	*longhorn_ch = longhorn_io->longhorn_ch;
	struct longhorn_bdev		*longhorn_bdev = longhorn_io->longhorn_bdev;
	uint8_t				pd_idx;
	int				ret = 0;
	struct longhorn_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;
	struct longhorn_base_io_channel *base_channel;

	assert(longhorn_ch != NULL);

	if (longhorn_io->base_bdev_io_remaining == 0) {
		longhorn_io->base_bdev_io_remaining = longhorn_bdev->num_base_bdevs;
	}

	TAILQ_FOREACH(base_channel, &longhorn_ch->base_channels, channels) {
        //for (pd_idx = 0; pd_idx < longhorn_bdev->num_base_bdevs; pd_idx++) {
                //base_ch = longhorn_ch->base_channel[pd_idx];
                //base_info = &longhorn_bdev->base_bdev_info[pd_idx];
                base_ch = base_channel->base_channel;
		base_info = base_channel->base_info;

        	switch (bdev_io->type) {
                case SPDK_BDEV_IO_TYPE_UNMAP:
                        ret = spdk_bdev_unmap_blocks(base_info->desc, base_ch,
                                                     bdev_io->u.bdev.offset_blocks,
                                                     bdev_io->u.bdev.num_blocks,
                                                     longhorn_base_io_complete, longhorn_io);
                        break;

                case SPDK_BDEV_IO_TYPE_FLUSH:
                        ret = spdk_bdev_flush_blocks(base_info->desc, base_ch,
                                                     bdev_io->u.bdev.offset_blocks,
                                                     bdev_io->u.bdev.num_blocks,
                                                     longhorn_base_io_complete, longhorn_io);
                        break;

                default:
                        SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", bdev_io->type);
                        assert(false);
                        ret = -EIO;
                }


               

                if (ret == -ENOMEM) {
                        longhorn_bdev_queue_io_wait(longhorn_io, base_info->bdev, base_ch,
                                                _longhorn_submit_null_payload_request);
			return;
                } 

		atomic_fetch_add(&longhorn_bdev->io_ops, 1);
		atomic_fetch_add(&longhorn_ch->io_ops, 1);
		longhorn_io->submitted = true;
		
		if (ret != 0) {
                        SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
                        assert(false);
                        longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
                } else {
                        SPDK_ERRLOG("success\n");
		}

        }
}

int longhorn_start(struct longhorn_bdev *longhorn_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	uint64_t min_blocklen = UINT64_MAX;
	struct longhorn_base_bdev_info *base_info;

	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
		/* Calculate minimum block count and length from all base bdevs */

		min_blockcnt = spdk_min(min_blockcnt, base_info->bdev->blockcnt);
                min_blocklen = spdk_min(min_blocklen, base_info->bdev->blocklen);
	}

	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
                if (base_info->bdev->blockcnt != min_blockcnt) {
                	SPDK_ERRLOG("Not all disks on RAID 1 has same block count");
                        return -EINVAL;
                }

                if (base_info->bdev->blocklen != min_blocklen) {
                	SPDK_ERRLOG("Not all disks on RAID 1 has same block length");
                        return -EINVAL;
                }
        }


	longhorn_bdev->bdev.blockcnt = min_blockcnt;

	if (longhorn_bdev->num_base_bdevs > 1) {
		longhorn_bdev->bdev.split_on_optimal_io_boundary = true;
	} else {
		/* Do not need to split reads/writes on single bdev RAID modules. */
		longhorn_bdev->bdev.split_on_optimal_io_boundary = false;
	}

	return 0;
}

static void
longhorn_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        struct longhorn_bdev_io *longhorn_io = cb_arg;

        spdk_bdev_free_io(bdev_io);

        longhorn_bdev_io_complete_part(longhorn_io, 1, success ?
                                   SPDK_BDEV_IO_STATUS_SUCCESS :
                                   SPDK_BDEV_IO_STATUS_FAILED);
}

static void
longhorn_bdev_submit_reset_request(struct longhorn_bdev_io *longhorn_io);

static void
_longhorn_bdev_submit_reset_request(void *_longhorn_io)
{
        struct longhorn_bdev_io *longhorn_io = _longhorn_io;

        longhorn_bdev_submit_reset_request(longhorn_io);
}



/*
 * brief:
 * longhorn_bdev_submit_reset_request function submits reset requests
 * to member disks; it will submit as many as possible unless a reset fails with -ENOMEM, in
 * which case it will queue it for later submission
 * params:
 * longhorn_io
 * returns:
 * none
 */
static void
longhorn_bdev_submit_reset_request(struct longhorn_bdev_io *longhorn_io)
{
        struct longhorn_bdev_io_channel *longhorn_ch = longhorn_io->longhorn_ch;
        struct longhorn_bdev            *longhorn_bdev = longhorn_io->longhorn_bdev;
        int                             ret;
        struct longhorn_base_bdev_info  *base_info;
        struct spdk_io_channel          *base_ch;
        struct longhorn_base_io_channel *base_channel;


        if (longhorn_io->base_bdev_io_remaining == 0) {
                longhorn_io->base_bdev_io_remaining = longhorn_bdev->num_base_bdevs;
        }

        TAILQ_FOREACH(base_channel, &longhorn_ch->base_channels, channels) {
                base_ch = base_channel->base_channel;
                base_info = base_channel->base_info;

                ret = spdk_bdev_reset(base_info->desc, base_ch,
                                      longhorn_base_bdev_reset_complete, longhorn_io);
                if (ret == 0) {
                        longhorn_io->base_bdev_io_submitted++;
                } else if (ret == -ENOMEM) {
                        longhorn_bdev_queue_io_wait(longhorn_io, base_info->bdev, base_ch,
                                                _longhorn_bdev_submit_reset_request);
                        return;
                } else {
                        SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
                        assert(false);
                        longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
                        return;
                }
        }
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to longhorn bdev io channel
 * bdev_io - pointer to parent bdev_io on longhorn bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
static void
longhorn_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
                     bool success)
{
        struct longhorn_bdev_io *longhorn_io = (struct longhorn_bdev_io *)bdev_io->driver_ctx;

        if (!success) {
                longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
        }

        longhorn_submit_rw_request(longhorn_io);
}


/*
 * brief:
 * longhorn_bdev_submit_request function is the submit_request function pointer of
 * longhorn bdev function table. This is used to submit the io on longhorn_bdev to below
 * layers.
 * params:
 * ch - pointer to longhorn bdev io channel
 * bdev_io - pointer to parent bdev_io on longhorn bdev device
 * returns:
 * none
 */
static void
_longhorn_bdev_submit_request(struct longhorn_bdev_io *longhorn_io)

{
	struct spdk_bdev_io             *bdev_io = spdk_bdev_io_from_ctx(longhorn_io);

        switch (bdev_io->type) {
        case SPDK_BDEV_IO_TYPE_READ:
                spdk_bdev_io_get_buf(bdev_io, longhorn_bdev_get_buf_cb,
                                     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
                break;
        case SPDK_BDEV_IO_TYPE_WRITE:
                longhorn_submit_rw_request(longhorn_io);
                break;

        case SPDK_BDEV_IO_TYPE_RESET:
                longhorn_bdev_submit_reset_request(longhorn_io);
                break;

        case SPDK_BDEV_IO_TYPE_FLUSH:
        case SPDK_BDEV_IO_TYPE_UNMAP:
                longhorn_submit_null_payload_request(longhorn_io);
                break;

        default:
                SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
                longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
                break;
        }
}

void
longhorn_pause_queue_io_wait(struct longhorn_bdev_io *longhorn_io) 
{
        longhorn_io->waitq_entry.cb_arg = longhorn_io;

	longhorn_io->longhorn_ch->queue_len++;
	SPDK_ERRLOG("adding to queue: %d\n", longhorn_io->longhorn_ch->queue_len);

        TAILQ_INSERT_TAIL(&longhorn_io->longhorn_ch->io_wait_queue, &longhorn_io->waitq_entry, link);
}

void
longhorn_pause_queue_playback(struct longhorn_bdev_io_channel *longhorn_ch)
{
        while (!TAILQ_EMPTY(&longhorn_ch->io_wait_queue)) {
                struct spdk_bdev_io_wait_entry *entry;

                entry = TAILQ_FIRST(&longhorn_ch->io_wait_queue);
                TAILQ_REMOVE(&longhorn_ch->io_wait_queue, entry, link);
                _longhorn_bdev_submit_request(entry->cb_arg);

		longhorn_ch->queue_len--;
	SPDK_ERRLOG("removing to queue: %d\n", longhorn_ch->queue_len);
        }
}

void
longhorn_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
        struct longhorn_bdev_io *longhorn_io = (struct longhorn_bdev_io *)bdev_io->driver_ctx;

        longhorn_io->longhorn_bdev = bdev_io->bdev->ctxt;
        longhorn_io->longhorn_ch = spdk_io_channel_get_ctx(ch);
        longhorn_io->base_bdev_io_remaining = 0;
        longhorn_io->base_bdev_io_submitted = 0;
        longhorn_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (longhorn_io->longhorn_ch->paused) {
		longhorn_pause_queue_io_wait(longhorn_io);
		return;
	}


	_longhorn_bdev_submit_request(longhorn_io);

}
