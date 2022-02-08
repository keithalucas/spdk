#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_longhorn.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"
#include "lib/blob/blobstore.h"
#include "bdev_longhorn_rebuild.h"
 
#define ALIGN_4K 4096

struct longhorn_blob_info_context {
	struct spdk_blob_store *bs;
	void (*callback)(struct longhorn_blob_info *info, void *cb_arg);
	void *cb_arg;
};


static void longhorn_blob_opened(void *arg, struct spdk_blob *blob, int bserrno) {
	struct longhorn_blob_info_context *ctx = arg;
	struct longhorn_blob_info info;
	size_t len;

	if (!blob) {
		(*ctx->callback)(NULL, ctx->cb_arg);
		free(ctx);
		return;
	}

	spdk_blob_get_xattr_value(blob, "name", &(info.name), &len);

	info.num_clusters = blob->active.num_clusters;
	info.allocated_clusters = longhorn_get_allocated_clusters(blob);

	info.table = calloc(1, sizeof(uint32_t) * info.allocated_clusters);

	longhorn_export_allocated_clusters(blob, info.table);

	(*ctx->callback)(&info, ctx->cb_arg);

	free(info.table);

	if (blob->parent_id) {
		spdk_bs_open_blob(ctx->bs, blob->parent_id, longhorn_blob_opened, ctx);
	} else {
		(*ctx->callback)(NULL, ctx->cb_arg);
		free(ctx);
	}
}

void longhorn_get_blob_info(struct spdk_blob_store *bs, uint64_t blob_id, void (*callback)(struct longhorn_blob_info *info, void *cb_arg), void *cb_arg) {
	struct longhorn_blob_info_context *ctx;
	
	ctx = calloc(1, sizeof(*ctx));
	ctx->bs = bs;
	ctx->callback = callback;
	ctx->cb_arg = cb_arg;

	spdk_bs_open_blob(ctx->bs, blob_id, longhorn_blob_opened, ctx);
}


int bdev_longhorn_lookup_name(const char *name, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg) {
	struct spdk_lvol_store *lvs;
        struct lvol_store_bdev *lvs_bdev;

	lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		printf("lvs: %s\n", lvs_bdev->lvs->name);

		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}

	return 0;
}

struct lvs_name *lvs_get_parent(const char *name)
{
	struct spdk_bdev *bdev = NULL;
	struct spdk_lvol *lvol = NULL;
	spdk_blob_id parent_id;

	bdev = spdk_bdev_get_by_name(name);

	if (bdev != NULL) {
        	lvol = vbdev_lvol_get_from_bdev(bdev);

		if (lvol != NULL) {
			parent_id = lvol->blob->parent_id;
		}
	}
	

	return NULL;
}

struct longhorn_import_context {
	char *name;
	char *lvs;
	char *file;
	struct lvol_store_bdev *lvs_bdev;

	FILE *fp;

	uint64_t blob_id;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;


	uint64_t num_clusters;
        uint32_t cluster_size;
        uint32_t io_unit_size;
        uint64_t current_cluster;
        uint64_t allocated_clusters;

	uint32_t *cluster_table;
	uint8_t *cluster;
	uint32_t current;

};

static void free_longhorn_import_context(struct longhorn_import_context *ctx) {
	if (ctx) {
		if (ctx->file) {
			free(ctx->file);
		}

		if (ctx->lvs) {
			free(ctx->lvs);
		}

		if (ctx->name) {
			free(ctx->name);
		}

		if (ctx->fp) {
			fclose(ctx->fp);
		}

		free(ctx);
	}
}

static void
write_next_cluster(void *arg1, int bserrno) {
	struct longhorn_import_context *ctx = arg1;
	ssize_t nread;
	uint64_t offset;
	

	if (bserrno) {
		printf("error: %d\n", bserrno);
                fclose(ctx->fp);

		return;
	}

	if (ctx->current >= ctx->allocated_clusters) {
		free_longhorn_import_context(ctx);
		
		printf("Import complete\n");
		return;
	}


	nread = fread(ctx->cluster, 1, ctx->cluster_size, ctx->fp);

	if (nread > 0) {
		offset = ctx->cluster_table[ctx->current] * ctx->cluster_size / ctx->io_unit_size;

		ctx->current++;

		spdk_blob_io_write(ctx->blob, ctx->channel, ctx->cluster, offset,
                                   ctx->cluster_size / ctx->io_unit_size, write_next_cluster, ctx);
	}
}


static void 
longhorn_import_blob(struct spdk_blob *blob, 
		     struct longhorn_import_context *ctx) {
	uint64_t blob_id = spdk_blob_get_id(blob);
	long offset;

	fread(&ctx->num_clusters, sizeof (uint64_t), 1, ctx->fp);
        fread(&ctx->allocated_clusters, sizeof (uint64_t), 1, ctx->fp);
        fread(&ctx->cluster_size, sizeof (uint32_t), 1, ctx->fp);
        fread(&ctx->io_unit_size, sizeof (uint32_t), 1, ctx->fp);


	ctx->cluster_table = calloc(sizeof (uint32_t), ctx->allocated_clusters);
	ctx->cluster = spdk_malloc(ctx->cluster_size,  ALIGN_4K, NULL,
                                   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	fread(ctx->cluster_table, sizeof(uint32_t), ctx->allocated_clusters, ctx->fp);

	offset = ftell(ctx->fp);

        if (offset % ctx->io_unit_size != 0) {
                fseek(ctx->fp, ctx->io_unit_size - offset % ctx->io_unit_size, SEEK_CUR);
        }

	ctx->channel = spdk_bs_alloc_io_channel(ctx->blob->bs);
	write_next_cluster(ctx, 0);
	
	printf("here\n");

}

static void
blob_import_iterator_cb(void *arg1, struct spdk_blob *blob, int bserrno) {
	struct longhorn_import_context *ctx = arg1;
	struct spdk_xattr_names *names;
	char *xattr_name = NULL;
	uint64_t blob_id;

        const void *value;
        size_t value_len;
        unsigned int i;


	if (bserrno) {
		if (blob_id != 0) {
			//longhorn_import_context(blob_id, ctx);
		} else {
			free_longhorn_import_context(ctx);
		}
		return;
	}

	blob_id = spdk_blob_get_id(blob);
	spdk_blob_get_xattr_names(blob, &names);

	for (i = 0; i < spdk_xattr_names_get_count(names); i++) {
		xattr_name = spdk_xattr_names_get_name(names, i);

		if (strcmp(xattr_name, "name") == 0) {
			spdk_blob_get_xattr_value(blob, xattr_name, 
						  &value, &value_len);

			if (strncmp(value, ctx->name, strlen(ctx->name)) == 0) {
				/* Found our blob. */
				printf("found blob %s\n", ctx->name);
				ctx->blob_id = blob_id;

				ctx->blob = blob;
			        longhorn_import_blob(blob, ctx);
				return;

			} else {
				printf("%s != %s\n", (char *)value, ctx->name);
			}
		}
	}


	spdk_bs_iter_next(ctx->lvs_bdev->lvs->blobstore, 
			  blob,
			  blob_import_iterator_cb,
			  ctx);
}






int bdev_longhorn_import(const char *name, const char *lvs, const char *file) {
	struct lvol_store_bdev *lvs_bdev = NULL;;
	struct longhorn_import_context *ctx = NULL;
	FILE *fp;

	lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		if (strcmp(lvs_bdev->lvs->name, lvs) != 0) {
			lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
		} else {
			printf("found lvs %s\n", lvs);
			break;
		}
	}

	if (lvs_bdev != NULL) {
		fp = fopen(file, "r");

		if (fp == NULL) return -1;

		ctx = calloc(1, sizeof(struct longhorn_import_context));

		ctx->name = strdup(name);
		ctx->lvs = strdup(lvs);
		ctx->file = strdup(file);

		ctx->lvs_bdev = lvs_bdev;

		ctx->fp = fp;

		spdk_bs_iter_first(ctx->lvs_bdev->lvs->blobstore, 
				   blob_import_iterator_cb, ctx);

		return 0;
	}


	return -1;
}

static void reopen_blob_cb(void *arg, struct spdk_blob *blob, int bserrno) {
	struct spdk_lvol *parent_lvol = arg;

	if (blob != NULL) {
		//parent_lvol->blob = blob;
	}
}

void bdev_longhorn_md_sync_complete(void *cb_arg, int bserrno)
{
	struct spdk_lvol *parent_lvol = cb_arg;

	if (bserrno != 0) {
		printf("metadata sync failed: %s\n", strerror(bserrno));
	} else {
		//spdk_bs_open_blob(parent_lvol->lvol_store->blobstore, parent_lvol->blob->id, reopen_blob_cb, parent_lvol);


		printf("metadata sync succeeded\n");
	}
}


int bdev_longhorn_link(const char *child, const char *parent)
{
	struct spdk_lvol_store *lvs = NULL;
	struct spdk_bdev *parent_bdev = NULL;
	struct spdk_bdev *child_bdev = NULL;
	struct spdk_lvol *parent_lvol = NULL;
	struct spdk_lvol *child_lvol = NULL;
	int bserrno;


	parent_bdev = spdk_bdev_get_by_name(parent);
	child_bdev = spdk_bdev_get_by_name(child);

	if (parent_bdev == NULL) {
		printf("can't find bdev for %s\n", parent);
		return;
	}

	if (child_bdev == NULL) {
		printf("can't find bdev for %s\n", child);
		return;
	}

        parent_lvol = vbdev_lvol_get_from_bdev(parent_bdev);
        child_lvol = vbdev_lvol_get_from_bdev(child_bdev);

	if (parent_lvol == NULL) {
		printf("can't find lvol for %s\n", parent);
		return;
	}

	if (child_lvol == NULL) {
		printf("can't find lvol for %s\n", child);
		return;
	}

	bserrno = spdk_blob_set_internal_xattr(parent_lvol->blob, BLOB_SNAPSHOT, &child_lvol->blob->id, sizeof(spdk_blob_id));	

	printf("syncing metadata\n");
	spdk_blob_sync_md(parent_lvol->blob, bdev_longhorn_md_sync_complete, parent_lvol);


	return 0;
}

struct snapshot_rpc {
	char *name;
	uint64_t num_clusters;
	uint32_t allocated_clusters;
	uint32_t *active_clusters;
};

#define MAX_SNAPSHOTS 256
struct snapshots_rpc {
	size_t num_snapshots;
	struct snapshot_rpc snapshots[MAX_SNAPSHOTS];
};


struct children_rpc {
	char *name;
	uint64_t cluster_size;
	uint32_t io_unit_size;

	struct snapshots_rpc snapshots;

};

static int json_decode_clusters(const struct spdk_json_val *val, void *out) {
	uint32_t *clusters = out;
	struct snapshot_rpc *snapshot = SPDK_CONTAINEROF(clusters, struct snapshot_rpc, active_clusters);
	size_t dummy;
	int error;
	uint32_t i;

	printf("name = %s\n", snapshot->name);
	printf("num_clusters = %lu\n", snapshot->num_clusters);
	printf("allocated_clusters = %u\n", snapshot->allocated_clusters);
	snapshot->active_clusters = calloc(sizeof(uint32_t), snapshot->allocated_clusters);

	error = spdk_json_decode_array(val, spdk_json_decode_uint32, snapshot->active_clusters, snapshot->allocated_clusters, &dummy, sizeof(uint32_t));


	for (int i = 0; i < snapshot->allocated_clusters; ++i) {
		printf("%u\n", snapshot->active_clusters[i]);
	}

	return error;
}


static const struct spdk_json_object_decoder rpc_snapshot_decoders[] = {
        {"name", offsetof(struct snapshot_rpc, name), spdk_json_decode_string},
	{"num_clusters", offsetof(struct snapshot_rpc, num_clusters), spdk_json_decode_uint64},
	{"allocated_clusters", offsetof(struct snapshot_rpc, allocated_clusters), spdk_json_decode_uint32},
        {"active_clusters", offsetof(struct snapshot_rpc, active_clusters), json_decode_clusters},
};


static int json_decode_snapshot(const struct spdk_json_val *val, void *out) {
	int error;

	error = spdk_json_decode_object(val, rpc_snapshot_decoders,
					SPDK_COUNTOF(rpc_snapshot_decoders),
					out);

	return error;
}

static int json_decode_snapshots(const struct spdk_json_val *val, void *out) {
	struct snapshots_rpc *snapshots = out;
	int error = 0;

	error = spdk_json_decode_array(val, json_decode_snapshot, snapshots->snapshots, MAX_SNAPSHOTS, &snapshots->num_snapshots, sizeof(struct snapshot_rpc));
	
	return error;
}

static const struct spdk_json_object_decoder rpc_replica_decoders[] = {
        {"name", offsetof(struct children_rpc, name), spdk_json_decode_string},
	{"cluster_size", offsetof(struct children_rpc, cluster_size), spdk_json_decode_uint64},
	{"io_unit_size", offsetof(struct children_rpc, io_unit_size), spdk_json_decode_uint32},
        {"snapshots", offsetof(struct children_rpc, snapshots), json_decode_snapshots},
};

	
static void receive_children(const char *addr,
			     const char *command,
			     int32_t id,
			     struct spdk_json_val *result,
			     struct spdk_json_val *error,
			     void *arg) {
	int i = 0;
	char *data = (char *)result->start;
	uint64_t blob_id;
	struct spdk_json_val *value;
	struct children_rpc children = {};

	printf("received response. %ld, %s\n", result->len, data);

	if (spdk_json_decode_object(result, rpc_replica_decoders,
				    SPDK_COUNTOF(rpc_replica_decoders),
				    &children)) {
		printf("error decoding\n");
	}



	
#if 0
	if (result->type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		value = spdk_json_object_first(result);

		while (value != NULL) {
			if (spdk_json_decode_uint64(value, &blob_id) == 0) {
				printf("%016lx %lu\n", blob_id, blob_id);
			}
			
			value = spdk_json_next(value);
		}
	}
#endif


}

struct rebuild_context {
	struct spdk_lvol_store *lvs;
	char *prefix;
};

static void receive_replicas(const char *addr,
			     const char *command,
			     int32_t id,
			     struct spdk_json_val *result,
			     struct spdk_json_val *error,
			     void *arg) {
	int i = 0;
	char *data = (char *)result->start;
	uint64_t blob_id;
	struct spdk_json_val *value;
	struct children_rpc children = {};
	struct rebuild_context *ctx = arg;
	char *bdev_name;
	char *last_bdev_name = NULL;;
	
	printf("receive_replicas");

	printf("received response. %ld, %s\n", result->len, data);

	if (spdk_json_decode_object(result, rpc_replica_decoders,
				    SPDK_COUNTOF(rpc_replica_decoders),
				    &children)) {
		printf("error decoding\n");
	}

	printf("num of snapshots %d\n", children.snapshots.num_snapshots);

	for (i = children.snapshots.num_snapshots - 1; i >= 0; --i) {
		bdev_name = spdk_sprintf_alloc("%s%s", ctx->prefix, children.snapshots.snapshots[i].name);

		printf("syncing %s\n", children.snapshots.snapshots[i].name);

		longhorn_snapshot_bdev_sync(bdev_name,
					    children.snapshots.snapshots[i].name,
					    ctx->lvs,
					    children.snapshots.snapshots[i].num_clusters,
					    children.snapshots.snapshots[i].allocated_clusters,
					    children.cluster_size,
					    children.io_unit_size,
					    children.snapshots.snapshots[i].active_clusters);

		if (last_bdev_name) {
			bdev_longhorn_link(bdev_name, last_bdev_name);
			free(last_bdev_name);
		}

		last_bdev_name = bdev_name;

	}

	if (last_bdev_name) {
		free(last_bdev_name);
	}
}



void bdev_longhorn_get_children_remote(const char *address, 
				       uint16_t port, 
				       const char *name) {
	char *addr = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;

	addr = spdk_sprintf_alloc("%s:%d", address, port);

	printf("%s:%d:%s\n", address, port, name);

	json_remote_client(addr);

	request = spdk_jsonrpc_client_create_request();

	w = spdk_jsonrpc_begin_request(request, 1, "lvol_list_children");
	spdk_json_write_name(w, "params");
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "name");
	spdk_json_write_string(w, name);
	spdk_json_write_object_end(w);
	
        spdk_jsonrpc_end_request(request, w);

	json_remote_client_send_command(addr, "lvol_list_children", 
					1, request, receive_children, NULL);


	free(addr);
}


void bdev_longhorn_rebuild_remote(const char *address, 
			          uint16_t port, 
			          const char *name,
				  char *remote_prefix,
				  struct spdk_lvol_store *lvs) {
	char *addr = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;
	struct rebuild_context *ctx;
	struct spdk_lvol_store *store;

	addr = spdk_sprintf_alloc("%s:%d", address, port);

	printf("%s:%d:%s\n", address, port, name);

	json_remote_client(addr);

	request = spdk_jsonrpc_client_create_request();

	w = spdk_jsonrpc_begin_request(request, 1, "lvol_list_children");
	spdk_json_write_name(w, "params");
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "name");
	spdk_json_write_string(w, name);
	spdk_json_write_object_end(w);
	
        spdk_jsonrpc_end_request(request, w);

	ctx = calloc(1, sizeof(*ctx));
	ctx->prefix = strdup(remote_prefix);
	ctx->lvs = lvs;

	json_remote_client_send_command(addr, "lvol_list_children", 
					1, request, receive_replicas, ctx);


	free(addr);
}

