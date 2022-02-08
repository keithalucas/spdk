#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/jsonrpc.h"
#include "spdk/thread.h"
#include "spdk/lvol.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"
#include "bdev_longhorn.h"
#include "bdev_longhorn_remote.h"
#include "bdev_longhorn_snapshot.h"

struct remote_snapshot_context {
	spdk_lvol_op_complete cb_fn;
        void *cb_arg;
};

static void remote_snapshot_complete(const char *addr,
                             const char *command,
                             int32_t id,
                             struct spdk_json_val *result,
                             struct spdk_json_val *error,
                             void *arg)
{
        struct remote_snapshot_context *ctx = arg;

	if (error != NULL) {
		printf("error not null\n");
	}else {
		printf("error null\n");
	}

	ctx->cb_fn(ctx->cb_arg, 0);

	free(ctx);
}

void bdev_longhorn_snapshot_remote(const char *addr,
                                   const char *name,
                                   const char *lvs,
                                   const char *snapshot,
                                   spdk_lvol_op_complete cb_fn, 
				   void *cb_arg) {
	char *remote_name;

        struct spdk_json_write_ctx *w;
        struct spdk_jsonrpc_client_request *request;
        struct remote_snapshot_context *ctx;
        struct spdk_lvol_store *store;

        remote_name = spdk_sprintf_alloc("%s/%s", lvs, name);

        json_remote_client(addr);

        request = spdk_jsonrpc_client_create_request();

        w = spdk_jsonrpc_begin_request(request, 1, "bdev_lvol_snapshot");
        spdk_json_write_name(w, "params");
        spdk_json_write_object_begin(w);
        spdk_json_write_name(w, "lvol_name");
        spdk_json_write_string(w, remote_name);
        spdk_json_write_name(w, "snapshot_name");
        spdk_json_write_string(w, snapshot);
        spdk_json_write_object_end(w);

        spdk_jsonrpc_end_request(request, w);

	free(remote_name);

        ctx = calloc(1, sizeof(*ctx));
        ctx->cb_fn = cb_fn;
        ctx->cb_arg = cb_arg;

        json_remote_client_send_command(addr, "bdev_lvol_snapshot",
                                        1, request, 
					remote_snapshot_complete, ctx);


}

struct compare_ctx {
	struct block_diff diff;

	compare_bdev_cb cb_fn;
	void *cb_arg;

	struct spdk_bdev *bdev1;
	struct spdk_bdev *bdev2;

	struct spdk_bdev_desc *desc1;
	struct spdk_bdev_desc *desc2;

	struct spdk_io_channel *channel1;
	struct spdk_io_channel *channel2;

	uint8_t *block1;
	uint8_t *block2;

	uint64_t block_num;

	uint64_t size1;
	uint64_t size2;

	uint64_t total_blocks;
};

void compare_bdev_event_cb(enum spdk_bdev_event_type type,
                            struct spdk_bdev *bdev,
                            void *event_ctx)
{

}

static void compare_free_ctx(struct compare_ctx *ctx) {
	free(ctx);
}

static void read_bdev1(struct compare_ctx *ctx);

static void read_bdev2_cb(struct spdk_bdev_io *bdev_io, 
			  bool success, 
			  void *cb_arg) {

	struct compare_ctx *ctx = cb_arg;
	struct block *bad_block;

	if (success) {
		if (memcpy(ctx->block1, ctx->block2, ctx->diff.blocksize) != 0) {
			bad_block = calloc(1, sizeof(*bad_block));
			bad_block->block = ctx->block_num;

			TAILQ_INSERT_TAIL(&ctx->diff.blocks, bad_block, next);

			ctx->diff.num_diff++;
		}

		ctx->block_num++;

		if (ctx->block_num >= ctx->total_blocks) {
			ctx->cb_fn(ctx->diff.num_diff == 0,
				   &ctx->diff, ctx->cb_arg);
			compare_free_ctx(ctx);
		} else {
			read_bdev1(ctx);
		}
	} else {
		ctx->cb_fn(-1, &ctx->diff, ctx->cb_arg);
		compare_free_ctx(ctx);
	}

}

static void read_bdev1_cb(struct spdk_bdev_io *bdev_io, 
			  bool success, 
			  void *cb_arg) {
	struct compare_ctx *ctx = cb_arg;

	if (success) {
		spdk_bdev_read(ctx->desc2, ctx->channel2, ctx->block2,
		       	       ctx->block_num * ctx->diff.blocksize,
		       	       ctx->diff.blocksize, read_bdev2_cb, ctx);
	} else {
		ctx->cb_fn(-1, &ctx->diff, ctx->cb_arg);
		compare_free_ctx(ctx);
	}
}

static void read_bdev1(struct compare_ctx *ctx) {
	spdk_bdev_read(ctx->desc1, ctx->channel1, ctx->block1,
		       ctx->block_num * ctx->diff.blocksize,
		       ctx->diff.blocksize, read_bdev1_cb, ctx);
}

static uint64_t bdev_get_size(struct spdk_bdev *bdev) {
	return spdk_bdev_get_block_size(bdev) * spdk_bdev_get_num_blocks(bdev);
}


void bdev_longhorn_compare(const char *bdev_name1,
			   const char *bdev_name2,
			   uint64_t blocksize,
			   compare_bdev_cb cb_fn,
			   void *cb_arg) {
	struct compare_ctx *ctx;
	int rc;

	
	ctx = calloc(1, sizeof (*ctx));

	TAILQ_INIT(&ctx->diff.blocks);
	ctx->diff.blocksize = blocksize;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = spdk_bdev_open_ext(bdev_name1, true, compare_bdev_event_cb,
				ctx, &ctx->desc1);

	rc = spdk_bdev_open_ext(bdev_name2, true, compare_bdev_event_cb,
				ctx, &ctx->desc2);

	ctx->bdev1 = spdk_bdev_desc_get_bdev(ctx->desc1);
	ctx->bdev2 = spdk_bdev_desc_get_bdev(ctx->desc2);

	ctx->channel1 = spdk_bdev_get_io_channel(ctx->desc1);
	ctx->channel2 = spdk_bdev_get_io_channel(ctx->desc2);
 

	ctx->block1 = spdk_malloc(blocksize, 4096, NULL,
				  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	ctx->block2 = spdk_malloc(blocksize, 4096, NULL,
				  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);


	ctx->size1 = bdev_get_size(ctx->bdev1);
	ctx->size2 = bdev_get_size(ctx->bdev2);
	
	ctx->total_blocks = spdk_min(ctx->size1, ctx->size2) / blocksize;

}

struct longhorn_bdev_snapshot_ctx {
	uint32_t num_to_snapshot;
	uint32_t snapshots_complete;
	struct longhorn_bdev *longhorn_bdev;
};

static void longhorn_bdev_snapshot_complete(void *cb_arg, 
					    struct spdk_lvol *lvol, 
					    int lvolerrno) 
{
	struct longhorn_bdev_snapshot_ctx *ctx = cb_arg;

	ctx->snapshots_complete++;

	SPDK_ERRLOG("%d snapshots complete %d %s\n", ctx->snapshots_complete, lvolerrno, strerror(-lvolerrno));

	if (ctx->snapshots_complete >= ctx->num_to_snapshot) {
		longhorn_unpause(ctx->longhorn_bdev);

		free(ctx);
	}
}

static void longhorn_bdev_snapshot(struct longhorn_bdev *longhorn_bdev,
                            const char *snapshot)
{

        struct longhorn_base_bdev_info  *base_info;
	struct longhorn_bdev_snapshot_ctx *ctx;
	struct spdk_bdev *bdev;
        struct spdk_lvol *lvol;


        SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_remove_base_devices\n");

	ctx = calloc(1, sizeof (*ctx));
	ctx->num_to_snapshot = longhorn_bdev->num_base_bdevs;
	ctx->snapshots_complete = 0;
	ctx->longhorn_bdev = longhorn_bdev;

        TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
                if (base_info->bdev == NULL) {
                        continue;
                }

                if (base_info->is_local) {
			//bdev = base_info->bdev;
			bdev = spdk_bdev_get_by_name(base_info->bdev_name);
			lvol = vbdev_lvol_get_from_bdev(bdev);

			if (lvol == NULL) {
				SPDK_ERRLOG("lvol null\n");
			}

			spdk_lvol_create_snapshot(lvol, snapshot,
						  longhorn_bdev_snapshot_complete,
						  ctx);


                } else {
			ctx->snapshots_complete++;
		}
        }
}



struct longhorn_snapshot_ctx {
	const char *name;
	const char *snapshot_name;
};

static void longhorn_snapshot_pause_complete(struct longhorn_bdev *bdev, 
					     void *arg) {
	struct longhorn_snapshot_ctx *ctx = arg;

	longhorn_bdev_snapshot(bdev, ctx->snapshot_name);

	free(ctx->name);
	free(ctx->snapshot_name);
	free(ctx);
}

int
longhorn_volume_snapshot(const char *name, const char *snapshot_name) {
	struct longhorn_bdev    *longhorn_bdev;
	struct longhorn_bdev_io_channel *io_channel;
	int                     rc;
	struct longhorn_snapshot_ctx *ctx;

        longhorn_bdev = longhorn_bdev_find_by_name(name);
        if (!longhorn_bdev) {
                SPDK_ERRLOG("Longhorn bdev '%s' is not created yet\n", name);
                return -ENODEV;
        }

	ctx = calloc(1, sizeof(*ctx));
	ctx->name = strdup(name);
	ctx->snapshot_name = strdup(snapshot_name);

        rc = pthread_mutex_trylock(&longhorn_bdev->base_bdevs_mutex);

	longhorn_volume_add_pause_cb(longhorn_bdev, 
				     longhorn_snapshot_pause_complete,
				     ctx);

        if (rc != 0) {
                if (errno == EBUSY) {
                        SPDK_ERRLOG("Longhorn bdev '%s' is busy\n", name);
                }


                return -errno;
        }

	TAILQ_FOREACH(io_channel, &longhorn_bdev->io_channel_head, channels) {
		spdk_thread_send_msg(io_channel->thread, bdev_longhorn_pause_io, io_channel);
	}

	pthread_mutex_unlock(&longhorn_bdev->base_bdevs_mutex);

        return 0;
}


