#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/jsonrpc.h"
#include "spdk/thread.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"
#include "lib/blob/blobstore.h"

#define ALIGN_4K 4096


struct sync_context {
	struct spdk_bdev *snapshot_bdev;
	struct spdk_bdev_desc *snapshot_desc;
	struct spdk_io_channel *snapshot_channel;

        uint64_t num_clusters;
        uint64_t allocated_clusters;
        uint32_t cluster_size;
        uint32_t io_unit_size;
	uint32_t *table;

	uint64_t io_units_per_cluster;

	uint8_t *cluster;
	uint64_t pos;
	struct iovec iov;

	int iovcnt;

	struct spdk_lvol *lvol;
	struct spdk_io_channel *channel;

};

static void longhorn_snapshot_read_cluster(struct sync_context *ctx);

static uint64_t longhorn_get_cluster_offset(struct sync_context *ctx) {
        uint64_t offset = ctx->table[ctx->pos] * ctx->io_units_per_cluster;

        return offset;
}

static void lvol_close_cb(void *cb_arg, int lvolerrno) {
}

static void longhorn_snapshot_write_cluster_cb(void *cb_arg, int bserrno) {
	struct sync_context *ctx = cb_arg;

	if (++ctx->pos < ctx->allocated_clusters) {
		longhorn_snapshot_read_cluster(ctx);
	} else {
		/* done */
		spdk_bdev_close(ctx->snapshot_desc);
		spdk_lvol_close(ctx->lvol, lvol_close_cb,  NULL);
		free(ctx->table);
		free(ctx);
	}
}

static void longhorn_snapshot_read_cluster_cb(struct spdk_bdev_io *bdev_io,
                bool success,
                void *cb_arg)
{
	struct sync_context *ctx = cb_arg;


	spdk_blob_io_write(ctx->lvol->blob, ctx->channel,
			   ctx->cluster, longhorn_get_cluster_offset(ctx),
			   ctx->io_units_per_cluster,
			   longhorn_snapshot_write_cluster_cb,
			   ctx);

}

static void longhorn_snapshot_read_cluster(struct sync_context *ctx)  {


	printf("reading %lu (cluster %u (%u)) (size = %u \n", ctx->table[ctx->pos] * ctx->cluster_size, ctx->table[ctx->pos], ctx->pos, ctx->cluster_size);

	spdk_bdev_read(ctx->snapshot_desc, ctx->snapshot_channel, 
			ctx->cluster, 
			ctx->table[ctx->pos] * ctx->cluster_size,
			ctx->cluster_size,
			longhorn_snapshot_read_cluster_cb,
			ctx);

}

 
static void longhorn_snapshot_lvol_create_complete_cb(void *arg, 
						      struct spdk_lvol *lvol, 
						      int lvolerrno) {
	struct sync_context *ctx = arg;

	ctx->lvol = lvol;

	ctx->pos = 0;
	printf("lvol created\n");

	longhorn_snapshot_read_cluster(ctx);
}

void snapshot_bdev_event_cb(enum spdk_bdev_event_type type, 
			    struct spdk_bdev *bdev, 
			    void *event_ctx)
{
}

void longhorn_snapshot_bdev_sync(const char *snapshot_bdev_name, 
				 const char *name, 
				 struct spdk_lvol_store *lvs,
				 uint64_t num_clusters,
				 uint64_t allocated_clusters,
				 uint32_t cluster_size,
				 uint32_t io_unit_size,
				 uint32_t *table) 
{
	struct sync_context *ctx;

	ctx = calloc(1, sizeof(struct sync_context));

	spdk_bdev_open_ext(snapshot_bdev_name, false, snapshot_bdev_event_cb,
			   ctx, &ctx->snapshot_desc);

	ctx->snapshot_bdev = spdk_bdev_desc_get_bdev(ctx->snapshot_desc);
	ctx->snapshot_channel = spdk_bdev_get_io_channel(ctx->snapshot_desc);

	ctx->cluster = spdk_malloc(cluster_size,  ALIGN_4K, NULL,
                                   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);


	ctx->channel = spdk_bs_alloc_io_channel(lvs->blobstore);

	ctx->num_clusters = num_clusters;
	ctx->allocated_clusters = allocated_clusters;
	ctx->cluster_size = cluster_size;
	ctx->io_unit_size = io_unit_size;
	ctx->table = table;

	ctx->io_units_per_cluster = ctx->cluster_size / ctx->io_unit_size;

	vbdev_lvol_create(lvs, name, ctx->num_clusters * ctx->cluster_size, true, LVOL_CLEAR_WITH_DEFAULT, longhorn_snapshot_lvol_create_complete_cb, ctx);
}


