#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"
#include "lib/blob/blobstore.h"
#include "bdev_longhorn_lvol.h"

#define ALIGN_4K 4096

struct spdk_lvol_store *
longhorn_get_lvol_store_by_name(const char *name)
{
        struct spdk_lvol_store *lvs = NULL;
        struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

        while (lvs_bdev != NULL) {
                lvs = lvs_bdev->lvs;
		printf("lvs->name = %s\n", lvs->name);
                if (strncmp(lvs->name, name, sizeof(lvs->name)) == 0) {
                        return lvs;
                }
                lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
        }
        return NULL;
}

enum longhorn_lvol_state {
	LVOL_SEND_NAME,
	LVOL_SEND_HEADER,
	LVOL_SEND_TABLE,
	LVOL_SEND_CLUSTER
};

struct longhorn_lvol_context {
	int fd;
	bool *busy;
	spdk_blob_id blob_id;
	char name[256];
	
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	uint32_t *table;
	struct longhorn_lvol_header header;

	uint64_t io_units_per_cluster;

	uint8_t *cluster;

	enum longhorn_lvol_state state;

	size_t pos;

};



uint64_t longhorn_get_allocated_clusters(struct spdk_blob *blob) {
	uint64_t allocated_clusters = 0;
	uint64_t i = 0;

	for (i = 0; i < blob->active.num_clusters; ++i) {
		if (blob->active.clusters[i] != 0) {
			++allocated_clusters;
		}
	}

	return allocated_clusters;
}

void longhorn_export_allocated_clusters(struct spdk_blob *blob, uint32_t *table) {
	uint64_t i = 0;
	uint64_t pos = 0;

	for (i = 0; i < blob->active.num_clusters; ++i) {
		if (blob->active.clusters[i] != 0) {
			table[pos++] = i;
		}
	}

}

static uint64_t longhorn_get_cluster_offset(struct longhorn_lvol_context *ctx) {
	uint64_t offset = ctx->table[ctx->pos++] * ctx->io_units_per_cluster;

	return offset;
}

static void longhorn_cluster_read_cb(void *arg1, int bserrno) {
	struct longhorn_lvol_context *ctx = arg1;
	

	if (bserrno) {
		return;
	}

	printf("writing cluster %u\n",  ctx->table[ctx->pos]);
	write(ctx->fd, ctx->cluster, ctx->header.cluster_size);
	printf("wrote cluster %u\n",  ctx->table[ctx->pos]);

	if (ctx->pos < ctx->header.allocated_clusters) {
		spdk_blob_io_read(ctx->blob, ctx->channel, ctx->cluster,
				  longhorn_get_cluster_offset(ctx), 
				  ctx->io_units_per_cluster,
				  longhorn_cluster_read_cb,
				  ctx);
	} else {
		/* Complete */
		//ctx->busy = 0;
	}
}

static void async_write(void *ptr, size_t size, 
			struct longhorn_lvol_context *ctx,
			void (*next)(struct longhorn_lvol_context *arg)
			) {


}
	
static void longhorn_blob_header(void *arg) {
	struct longhorn_lvol_context *ctx = arg;
}

	


static void longhorn_blob_opened(void *arg, struct spdk_blob *blob, int bserrno) {
	struct longhorn_lvol_context *ctx = arg;
	char *name;
	size_t len;

	ctx->blob = blob;

	spdk_blob_get_xattr_value(blob, "name", &name, &len);

	printf("name = %s\n", name);

	strncpy(ctx->name, name, len);

	write(ctx->fd, ctx->name, sizeof(ctx->name));

	ctx->header.num_clusters = blob->active.num_clusters;
	ctx->header.allocated_clusters = longhorn_get_allocated_clusters(blob);
	ctx->header.cluster_size = ctx->bs->cluster_sz;
	ctx->header.io_unit_size = ctx->bs->io_unit_size;

	write(ctx->fd, &ctx->header, sizeof(ctx->header));

	ctx->table = calloc(1, sizeof(uint32_t) * ctx->header.allocated_clusters);
	longhorn_export_allocated_clusters(blob, ctx->table);

	printf("writing table\n");
	write(ctx->fd, ctx->table, sizeof(uint32_t) * ctx->header.allocated_clusters);
	printf("wrote table\n");
	

	ctx->cluster = spdk_malloc(ctx->header.cluster_size,  ALIGN_4K, NULL,
                                   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	ctx->io_units_per_cluster = ctx->header.cluster_size / ctx->header.io_unit_size;
	ctx->channel = spdk_bs_alloc_io_channel(ctx->bs);

	if (ctx->pos < ctx->header.allocated_clusters) {
		spdk_blob_io_read(ctx->blob, ctx->channel, ctx->cluster,
				  longhorn_get_cluster_offset(ctx), 
				  ctx->io_units_per_cluster,
				  longhorn_cluster_read_cb,
				  ctx);
	}

}

static void longhorn_lvol_handle_state(struct longhorn_lvol_context *ctx) {
	switch (ctx->state) {
	case LVOL_SEND_NAME:
		break;
	case LVOL_SEND_HEADER:
		break;
	case LVOL_SEND_TABLE:
		break;
	case LVOL_SEND_CLUSTER:
		break;
	}
}

	

void longhorn_lvol_transmit(int fd, uint64_t blob_id, struct spdk_blob_store *bs, bool *busy) {
	struct longhorn_lvol_context *ctx;

	ctx = calloc(1, sizeof(struct longhorn_lvol_context));

	ctx->fd = fd;
	ctx->blob_id = (spdk_blob_id) blob_id;
	ctx->bs = bs;

	spdk_bs_open_blob(bs, ctx->blob_id, longhorn_blob_opened, ctx);
}


