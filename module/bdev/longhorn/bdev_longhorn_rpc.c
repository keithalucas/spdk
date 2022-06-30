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

#define RPC_MAX_BASE_BDEVS 255
#define BUFSIZE 255
#define ALIGN_4K 4096

/*
 * Input structure for bdev_longhorn_get_bdevs RPC
 */
struct rpc_bdev_longhorn_get_bdevs {
	/* category - all or online or configuring or offline */
	char *category;
};

/*
 * brief:
 * free_rpc_bdev_longhorn_get_bdevs function frees RPC bdev_longhorn_get_bdevs related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_longhorn_get_bdevs(struct rpc_bdev_longhorn_get_bdevs *req)
{
	free(req->category);
}

/*
 * Decoder object for RPC get_longhorns
 */
static const struct spdk_json_object_decoder rpc_bdev_longhorn_get_bdevs_decoders[] = {
	{"category", offsetof(struct rpc_bdev_longhorn_get_bdevs, category), spdk_json_decode_string},
};

/*
 * brief:
 * rpc_bdev_longhorn_get_bdevs function is the RPC for rpc_bdev_longhorn_get_bdevs. This is used to list
 * all the longhorn bdev names based on the input category requested. Category should be
 * one of "all", "online", "configuring" or "offline". "all" means all the longhorns
 * whether they are online or configuring or offline. "online" is the longhorn bdev which
 * is registered with bdev layer. "configuring" is the longhorn bdev which does not have
 * full configuration discovered yet. "offline" is the longhorn bdev which is not
 * registered with bdev as of now and it has encountered any error or user has
 * requested to offline the longhorn.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_longhorn_get_bdevs(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_longhorn_get_bdevs   req = {};
	struct spdk_json_write_ctx  *w;
	struct longhorn_bdev            *longhorn_bdev;

	if (spdk_json_decode_object(params, rpc_bdev_longhorn_get_bdevs_decoders,
				    SPDK_COUNTOF(rpc_bdev_longhorn_get_bdevs_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (!(strcmp(req.category, "all") == 0 ||
	      strcmp(req.category, "online") == 0 ||
	      strcmp(req.category, "configuring") == 0 ||
	      strcmp(req.category, "offline") == 0)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	/* Get longhorn bdev list based on the category requested */
	if (strcmp(req.category, "all") == 0) {
		TAILQ_FOREACH(longhorn_bdev, &g_longhorn_bdev_list, global_link) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", longhorn_bdev->name);
			longhorn_bdev_dump_info_json(longhorn_bdev, w);
			spdk_json_write_object_end(w);
		}
	} else if (strcmp(req.category, "online") == 0) {
		TAILQ_FOREACH(longhorn_bdev, &g_longhorn_bdev_configured_list, state_link) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", longhorn_bdev->name);
			longhorn_bdev_dump_info_json(longhorn_bdev, w);
			spdk_json_write_object_end(w);
		}
	} else if (strcmp(req.category, "configuring") == 0) {
		TAILQ_FOREACH(longhorn_bdev, &g_longhorn_bdev_configuring_list, state_link) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", longhorn_bdev->name);
			longhorn_bdev_dump_info_json(longhorn_bdev, w);
			spdk_json_write_object_end(w);
		}
	} else {
		TAILQ_FOREACH(longhorn_bdev, &g_longhorn_bdev_offline_list, state_link) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", longhorn_bdev->name);
			longhorn_bdev_dump_info_json(longhorn_bdev, w);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_longhorn_get_bdevs(&req);
}
SPDK_RPC_REGISTER("bdev_longhorn_get_bdevs", rpc_bdev_longhorn_get_bdevs, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_longhorn_get_bdevs, get_longhorn_bdevs)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_longhorn_get_bdevs, longhorn_volume_list)

struct longhorn_replica {
	char *addr;
	char *lvs;
	uint16_t nvmf_port;
	uint16_t comm_port;
};

/*
 * Base bdevs in RPC bdev_longhorn_create
 */
struct rpc_bdev_longhorn_create_base_bdevs {
	/* Number of base bdevs */
	size_t           num_base_bdevs;

	struct longhorn_replica             replicas[RPC_MAX_BASE_BDEVS];
};

/*
 * Input structure for RPC rpc_bdev_longhorn_create
 */
struct rpc_bdev_longhorn_create {
	/* longhorn bdev name */
	char                                 *name;
	char                                 *address;

	/* Base bdevs information */
	struct rpc_bdev_longhorn_create_base_bdevs base_bdevs;
};

/*
 * brief:
 * free_rpc_bdev_longhorn_create function is to free RPC bdev_longhorn_create related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_longhorn_create(struct rpc_bdev_longhorn_create *req)
{
	size_t i;

	free(req->name);
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		free(req->base_bdevs.replicas[i].addr);
	}
}


/*
 * Decoder object for RPC bdev_longhorn_create
 */
static const struct spdk_json_object_decoder rpc_bdev_longhorn_create_replica_decoders[] = {

	{"lvs", offsetof(struct longhorn_replica, lvs), spdk_json_decode_string},
	{"addr", offsetof(struct longhorn_replica, addr), spdk_json_decode_string, true},

	{"nvmf_port", offsetof(struct longhorn_replica, nvmf_port), spdk_json_decode_uint16, true},
	{"comm_port", offsetof(struct longhorn_replica, comm_port), spdk_json_decode_uint16, true}
};

static int json_decode_replica(const struct spdk_json_val *val, void *out) {
	int error;
	struct longhorn_replica *replica = out;
	printf("starting json_decode_replica\n");
	printf("type %d\n", val->type);

	error = spdk_json_decode_object(val, rpc_bdev_longhorn_create_replica_decoders, 
				       SPDK_COUNTOF(rpc_bdev_longhorn_create_replica_decoders), out);
	printf("return json_decode_replica: %d\n", error);
	printf("replica lvs: %s\n", replica->lvs);
	printf("replica addr: %s\n", replica->addr);
	printf("replica nvmf port: %u\n", replica->nvmf_port);
	printf("replica comm port: %u\n", replica->comm_port);
	return error;

}
/*
 * Decoder function for RPC bdev_longhorn_create to decode base bdevs list
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_longhorn_create_base_bdevs *base_bdevs = out;
	int error = 0;

	printf("starting decode_base_bdevs\n");
	printf("type %d\n", val->type);
	error = spdk_json_decode_array(val, json_decode_replica, base_bdevs->replicas,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(struct longhorn_replica));
	printf("return decode_base_bdevs: %d\n", error);

	return error;
}

/*
 * Decoder object for RPC bdev_longhorn_create
 */
static const struct spdk_json_object_decoder rpc_bdev_longhorn_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_longhorn_create, name), spdk_json_decode_string},
	{"address", offsetof(struct rpc_bdev_longhorn_create, address), spdk_json_decode_string, true},
	{"replicas", offsetof(struct rpc_bdev_longhorn_create, base_bdevs), decode_base_bdevs},
};

/*
 * brief:
 * rpc_bdev_longhorn_create function is the RPC for creating RAID bdevs. It takes
 * input as longhorn bdev name and list of base bdev names.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_longhorn_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_longhorn_create	req = {};
	//struct longhorn_bdev_config		*longhorn_cfg;
	int				rc;
	size_t				i;
	//struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_bdev_longhorn_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_longhorn_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "longhorn spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = longhorn_bdev_create(req.name, req.address, req.base_bdevs.num_base_bdevs);
	if (rc != 0) {
		//longhorn_bdev_config_cleanup(longhorn_cfg);
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create RAID bdev %s: %s",
						     req.name, spdk_strerror(-rc));
		goto cleanup;
	}

	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {

		//longhorn_bdev_add_base_device(req.name, bdev_name);

		longhorn_bdev_add_replica(req.name, req.base_bdevs.replicas[i].lvs, req.base_bdevs.replicas[i].addr, req.base_bdevs.replicas[i].nvmf_port, req.base_bdevs.replicas[i].comm_port, LONGHORN_BASE_BDEV_RW);

	}



	spdk_jsonrpc_send_bool_response(request, true);




cleanup:
	free_rpc_bdev_longhorn_create(&req);
}
SPDK_RPC_REGISTER("longhorn_volume_create", rpc_bdev_longhorn_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(longhorn_volume_create, bdev_longhorn_create)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(longhorn_volume_create, construct_longhorn_bdev)


/*
 * Input structure for RPC rpc_bdev_longhorn_create
 */
struct rpc_bdev_longhorn_add_replica {
	/* Raid bdev name */
	char                                 *name;

	/* Base bdevs information */
	struct longhorn_replica replica;
};

/*
 * Decoder object for RPC bdev_longhorn_create
 */
static const struct spdk_json_object_decoder rpc_bdev_longhorn_add_replica_decoders[] = {
	{"name", offsetof(struct rpc_bdev_longhorn_add_replica, name), spdk_json_decode_string},
	{"replica", offsetof(struct rpc_bdev_longhorn_add_replica, replica), json_decode_replica},
};

/*
 * brief:
 * rpc_bdev_longhorn_create function is the RPC for creating RAID bdevs. It takes
 * input as longhorn bdev name and list of base bdev names.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_longhorn_add_replica_cmd(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_longhorn_add_replica	req = {};
	//struct longhorn_bdev_config		*longhorn_cfg;
	int				rc;
	size_t				i;
	char *bdev_name;

	printf("type %d\n", params->type);
	if (spdk_json_decode_object(params, rpc_bdev_longhorn_add_replica_decoders,
				    SPDK_COUNTOF(rpc_bdev_longhorn_add_replica_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "longhorn spdk_json_decode_object failed");
	}

	rc = longhorn_volume_add_replica(req.name, req.replica.lvs, req.replica.addr, req.replica.nvmf_port, req.replica.comm_port);

	if (rc != 0) {
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "failed to add replica");
        } else {
                spdk_jsonrpc_send_bool_response(request, true);

        }

}

SPDK_RPC_REGISTER("longhorn_volume_add_replica", rpc_bdev_longhorn_add_replica_cmd, SPDK_RPC_RUNTIME)

static void
rpc_bdev_longhorn_remove_replica_cmd(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_longhorn_add_replica	req = {};
	//struct longhorn_bdev_config		*longhorn_cfg;
	int				rc;
	size_t				i;
	char *bdev_name;

	printf("type %d\n", params->type);
	if (spdk_json_decode_object(params, rpc_bdev_longhorn_add_replica_decoders,
				    SPDK_COUNTOF(rpc_bdev_longhorn_add_replica_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "longhorn spdk_json_decode_object failed");
		return;
	}

	rc = longhorn_bdev_remove_replica(req.name, req.replica.lvs, req.replica.addr, req.replica.nvmf_port, req.replica.comm_port);


	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "failed to remove replica");
	} else {
		spdk_jsonrpc_send_bool_response(request, true);

	}
}

SPDK_RPC_REGISTER("longhorn_volume_remove_replica", rpc_bdev_longhorn_remove_replica_cmd, SPDK_RPC_RUNTIME)


/*
 * Input structure for RPC deleting a longhorn bdev
 */
struct rpc_bdev_longhorn_delete {
	/* longhorn bdev name */
	char *name;
};

/*
 * brief:
 * free_rpc_bdev_longhorn_delete function is used to free RPC bdev_longhorn_delete related parameters
 * params:
 * req - pointer to RPC request
 * params:
 * none
 */
static void
free_rpc_bdev_longhorn_delete(struct rpc_bdev_longhorn_delete *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC longhorn_bdev_delete
 */
static const struct spdk_json_object_decoder rpc_bdev_longhorn_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_longhorn_delete, name), spdk_json_decode_string},
};

struct rpc_bdev_longhorn_delete_ctx {
	struct rpc_bdev_longhorn_delete req;
	//struct longhorn_bdev_config *longhorn_cfg;
	struct spdk_jsonrpc_request *request;
};

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the deletion of the longhorn bdev.
 * returns:
 * none
 */
static void
bdev_longhorn_delete_done(void *cb_arg, int rc)
{
	struct rpc_bdev_longhorn_delete_ctx *ctx = cb_arg;
//	struct longhorn_bdev_config *longhorn_cfg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to delete longhorn bdev %s (%d): %s\n",
			    ctx->req.name, rc, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}


	spdk_jsonrpc_send_bool_response(request, true);
exit:
	free_rpc_bdev_longhorn_delete(&ctx->req);
	free(ctx);
}

/*
 * brief:
 * rpc_bdev_longhorn_delete function is the RPC for deleting a longhorn bdev. It takes longhorn
 * name as input and delete that longhorn bdev including freeing the base bdev
 * resources.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_longhorn_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_longhorn_delete_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_longhorn_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_longhorn_delete_decoders),
				    &ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}
 
	ctx->request = request;

	/* Remove all the base bdevs from this longhorn bdev before deleting the longhorn bdev */
	longhorn_bdev_remove_base_devices(ctx->req.name, bdev_longhorn_delete_done, ctx);
	

	return;

cleanup:
	free_rpc_bdev_longhorn_delete(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_longhorn_delete", rpc_bdev_longhorn_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_longhorn_delete, destroy_longhorn_bdev)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_longhorn_delete, longhorn_volume_stop)

struct cluster_entry {
	int cluster;

	TAILQ_ENTRY(cluster_entry) entries;
};

struct read_sparse_context {
	struct spdk_blob_store *blobstore;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	const char *name;
	uint64_t id;
	uint64_t num_clusters;
	uint32_t cluster_size;
	uint32_t io_unit_size;
	uint64_t current_cluster;
	uint64_t allocated_clusters;
	uint64_t start_offset;

	FILE *fp;
	uint8_t *cluster;

	TAILQ_HEAD(, cluster_entry)  cluster_head;
	struct cluster_entry *current;
};

static void 
read_cluster_cb(void *arg1, int bserrno);
static void blob_it_cb(void *arg1, struct spdk_blob *blob, int bserrno);

static void
read_next_allocated_cluster(struct read_sparse_context *ctx) {
	uint64_t offset;

	if (ctx->current) {
		offset = ctx->current->cluster * ctx->cluster_size / ctx->io_unit_size;

		printf("reading at %" PRIu64 ":%" PRIu64 "\n", offset, ctx->cluster_size);

		spdk_blob_io_read(ctx->blob, ctx->channel, ctx->cluster, offset,
				  ctx->cluster_size / ctx->io_unit_size, read_cluster_cb, ctx);
	} else {
		fclose(ctx->fp);
	}
}

static void 
read_cluster_cb(void *arg1, int bserrno)
{
	struct read_sparse_context *ctx = arg1;
	uint32_t nwritten;

	if (bserrno) {
		printf("error: %d\n", bserrno);
		fclose(ctx->fp);
	        spdk_bs_iter_next(ctx->blobstore, ctx->blob, blob_it_cb, ctx->blobstore);
		return;
	}

	printf("successful read\n");

	nwritten = fwrite(ctx->cluster, 1, ctx->cluster_size, ctx->fp);
	printf("nwritten %u ? %u\n", nwritten, ctx->cluster_size);
	
	if (nwritten != ctx->cluster_size) {
		printf("nwritten not euqal to cluster size\n");
		fclose(ctx->fp);
	        spdk_bs_iter_next(ctx->blobstore, ctx->blob, blob_it_cb, ctx->blobstore);
		return;
	}


	if (++ctx->current_cluster < ctx->allocated_clusters && ctx->current != NULL) {

		struct cluster_entry *tmp = TAILQ_NEXT(ctx->current, entries);
		printf("next cluster = %d\n", tmp->cluster);
		ctx->current = tmp;

		read_next_allocated_cluster(ctx);
	} else {

		printf("complete\n");
		fclose(ctx->fp);
		//spdk_blob_close(ctx->blob, close_cb, ctx);
	        spdk_bs_iter_next(ctx->blobstore, ctx->blob, blob_it_cb, ctx->blobstore);

	}


}


static void open_file(struct read_sparse_context *ctx)
{
	char filename[BUFSIZE] = {'\0'};
	uint64_t blocks;
	struct cluster_entry *entry;
	long offset;

	if (ctx->name) {
		snprintf(filename, BUFSIZE - 1, "%s.kdat", ctx->name);
	} else {
		snprintf(filename, BUFSIZE - 1, "%" PRIx64 ".kdat", ctx->id);
	}

	printf("opening %s\n", filename);
	ctx->fp = fopen(filename, "w");

	if (ctx->fp) {
		fwrite(&ctx->num_clusters, sizeof (uint64_t), 1, ctx->fp);
		fwrite(&ctx->allocated_clusters, sizeof (uint64_t), 1, ctx->fp);
		fwrite(&ctx->cluster_size, sizeof (uint32_t), 1, ctx->fp);
		fwrite(&ctx->io_unit_size, sizeof (uint32_t), 1, ctx->fp);

		TAILQ_FOREACH(entry, &ctx->cluster_head, entries) {
			fwrite(&entry->cluster, sizeof (int), 1, ctx->fp);
		}

		
		offset = ftell(ctx->fp);

		ctx->start_offset = offset;

		if (offset % ctx->io_unit_size != 0) {
			ctx->start_offset = ((offset / ctx->io_unit_size) + 1) *
				ctx->io_unit_size;
		}
		
		fseek(ctx->fp, ctx->start_offset, SEEK_SET);

		ctx->cluster = spdk_malloc(ctx->cluster_size,  ALIGN_4K, NULL,
                                        SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

		ctx->channel = spdk_bs_alloc_io_channel(ctx->blobstore);

		ctx->current = TAILQ_FIRST(&ctx->cluster_head);

		read_next_allocated_cluster(ctx);
	}

}


static void blob_it_cb(void *arg1, struct spdk_blob *blob, int bserrno)
{
	struct spdk_blob_store *blobstore = arg1;
	uint64_t val;
	struct spdk_xattr_names *names;
	const void *value;
	size_t value_len;
	unsigned int i;
	struct cluster_entry *entry;
	struct read_sparse_context *ctx = NULL;
	char *xattr_name = NULL;

	if (bserrno) {
		if (bserrno == -ENOENT) {
			printf("last blob\n");
		} else {
			printf("error blob: %d\n", bserrno);
		}
		return;
	}
	ctx = calloc(1, sizeof(struct read_sparse_context));

	printf("Blob ID: %" PRIx64 "\n", spdk_blob_get_id(blob));
	printf("Blob Parent ID: %" PRIx64 "\n", blob->parent_id);

        val = spdk_blob_get_num_clusters(blob);
        printf("# of clusters: %" PRIu64 "\n", val);
	printf("cluster size: %d\n", blobstore->cluster_sz);
	printf("io unit size: %d\n", blobstore->io_unit_size);

	ctx->id = spdk_blob_get_id(blob);
	ctx->num_clusters = blob->active.num_clusters;
	ctx->cluster_size = blobstore->cluster_sz;
	ctx->io_unit_size = blobstore->io_unit_size;
	ctx->blobstore = blobstore;
	ctx->blob = blob;

        val = spdk_blob_get_num_pages(blob);
        printf("# of pages: %" PRIu64 "\n", val);
        printf("# of pages per cluster: %" PRIu64 "\n", blobstore->pages_per_cluster);

        spdk_blob_get_xattr_names(blob, &names);

        printf("# of xattrs: %d\n", spdk_xattr_names_get_count(names));
        printf("xattrs:\n");
        for (i = 0; i < spdk_xattr_names_get_count(names); i++) {
		xattr_name = spdk_xattr_names_get_name(names, i);

                spdk_blob_get_xattr_value(blob, xattr_name,
                                          &value, &value_len);
                if (value_len > BUFSIZE) {
                        printf("FYI: adjusting size of xattr due to CLI limits.\n");
                        value_len = BUFSIZE + 1;
                }
		
		if (strcmp(xattr_name, "name") == 0) {
			ctx->name = strdup(value);
		}

                printf("\n(%d) Name:%s\n", i, xattr_name);
                printf("(%d) Value:\n", i);
                spdk_log_dump(stdout, "", value, value_len - 1);

        }


	TAILQ_INIT(&ctx->cluster_head);
	for (i = 0; i < blob->active.num_clusters; ++i) {
		if (blob->active.clusters[i] != 0) {
			entry = malloc(sizeof(struct cluster_entry));
			entry->cluster = i;
			TAILQ_INSERT_TAIL(&ctx->cluster_head, entry, entries);
			printf("Cluster %d LBA: %"  PRIu64 "\n", i, blob->active.clusters[i]);
			++ctx->allocated_clusters;
		}
	}

	printf("Allocated clusters: %d\n", ctx->allocated_clusters);
	TAILQ_FOREACH(entry, &ctx->cluster_head, entries) {
		printf("Cluster %d\n", entry->cluster);

	}

	if (ctx->allocated_clusters > 0) {
		open_file(ctx);
	} else {
		free(ctx);
	        spdk_bs_iter_next(blobstore, blob, blob_it_cb, blobstore);
	}



}

struct rpc_lvol_show_blobs_param {
	char *lvs;
};

static const struct spdk_json_object_decoder rpc_lvol_show_blobs_decoders[] = {
	{"lvs", offsetof(struct rpc_lvol_show_blobs_param, lvs), spdk_json_decode_string},
};

static void spdk_bsdump_done(void *arg, int bserrno) {
}
static void
bsdump_print_xattr(FILE *fp, const char *bstype, const char *name, const void *value,
                   size_t value_len)
{
        if (strncmp(bstype, "BLOBFS", SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
                if (strcmp(name, "name") == 0) {
                        fprintf(fp, "%.*s", (int)value_len, (char *)value);
                } else if (strcmp(name, "length") == 0 && value_len == sizeof(uint64_t)) {
                        uint64_t length;

                        memcpy(&length, value, sizeof(length));
                        fprintf(fp, "%" PRIu64, length);
                } else {
                        fprintf(fp, "?");
                }
        } else if (strncmp(bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
                if (strcmp(name, "name") == 0) {
                        fprintf(fp, "%s", (char *)value);
                } else if (strcmp(name, "uuid") == 0 && value_len == sizeof(struct spdk_uuid)) {
                        char uuid[SPDK_UUID_STRING_LEN];

                        spdk_uuid_fmt_lower(uuid, sizeof(uuid), (struct spdk_uuid *)value);
                        fprintf(fp, "%s", uuid);
                } else {
                        fprintf(fp, "?");
                }
        } else {
                fprintf(fp, "?");
        }
}

static void rpc_lvol_show_blobs(struct spdk_jsonrpc_request *request,
                     const struct spdk_json_val *params)
{
	struct spdk_lvol_store *lvs;
	struct lvol_store_bdev *lvs_bdev;
	struct rpc_lvol_show_blobs_param req;

#if 0

	if (spdk_json_decode_object(params, rpc_lvol_show_blobs_decoders,
				    SPDK_COUNTOF(rpc_lvol_show_blobs_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}
#endif

	//lvs = vbdev_get_lvol_store_by_name(req.lvs);
	lvs_bdev = vbdev_lvol_store_first();
	lvs = lvs_bdev->lvs;


	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;

		spdk_bs_iter_first(lvs->blobstore, blob_it_cb, lvs->blobstore);
		 lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
		 //spdk_bs_dump(lvs->blobstore->dev, stdout, bsdump_print_xattr, spdk_bsdump_done, NULL);
	}

		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "egg");
}

SPDK_RPC_REGISTER("lvol_show_blobs", rpc_lvol_show_blobs, SPDK_RPC_RUNTIME)

struct rpc_bdev_longhorn_snapshot {
	char *name;
	char *snapshot;
};

static void
free_rpc_bdev_longhorn_snapshot(struct rpc_bdev_longhorn_snapshot *req)
{
	free(req->name);
	free(req->snapshot);
}

/*
 * Decoder object for RPC longhorn_bdev_delete
 */
static const struct spdk_json_object_decoder rpc_bdev_longhorn_snapshot_decoders[] = {
	{"name", offsetof(struct rpc_bdev_longhorn_snapshot, name), spdk_json_decode_string},
	{"snapshot", offsetof(struct rpc_bdev_longhorn_snapshot, snapshot), spdk_json_decode_string},
};

struct rpc_bdev_longhorn_snapshot_ctx {
	char *name;
	char *snapshot;
	struct spdk_jsonrpc_request *request;
};

static void rpc_longhorn_snapshot_complete(void *cb_arg) {
	struct rpc_bdev_longhorn_snapshot_ctx *ctx = cb_arg;
}



static void rpc_longhorn_snapshot_cmd(struct spdk_jsonrpc_request *request, 
				      const struct spdk_json_val *params)
{
	struct rpc_bdev_longhorn_snapshot req;
	struct rpc_bdev_longhorn_snapshot_ctx *ctx;
	//struct longhorn_bdev_config *longhorn_cfg;

	if (spdk_json_decode_object(params, rpc_bdev_longhorn_snapshot_decoders,
				    SPDK_COUNTOF(rpc_bdev_longhorn_snapshot_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}


#if 0
	longhorn_cfg = longhorn_bdev_config_find_by_name(req.name);
	if (longhorn_cfg == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, ENODEV,
						     "longhorn bdev %s is not found in config",
						     req.name);
//goto cleanup;
	}
#endif


	ctx = calloc(1, sizeof(*ctx));
	




}

SPDK_RPC_REGISTER("bdev_longhorn_snapshot", rpc_longhorn_snapshot_cmd, SPDK_RPC_RUNTIME)


