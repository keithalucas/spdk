#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_longhorn.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "bdev_longhorn_snapshot.h"

struct rpc_longhorn_volume_snapshot {
	char *name;
	char *snapshot_name;
};

static void
free_rpc_longhorn_volume_snapshot(struct rpc_longhorn_volume_snapshot *req) {
	free(req->name);
	free(req->snapshot_name);
}

static const struct spdk_json_object_decoder rpc_longhorn_volume_snapshot_decoders[] = {
	{"name", offsetof(struct rpc_longhorn_volume_snapshot, name), spdk_json_decode_string},
	{"snapshot_name", offsetof(struct rpc_longhorn_volume_snapshot, snapshot_name), spdk_json_decode_string},
};

static void
rpc_longhorn_volume_snapshot_cmd(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_longhorn_volume_snapshot req = {};

	if (spdk_json_decode_object(params, rpc_longhorn_volume_snapshot_decoders,
				    SPDK_COUNTOF(rpc_longhorn_volume_snapshot_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "longhorn spdk_json_decode_object failed");
		return;
	}

	if (longhorn_volume_snapshot(req.name, req.snapshot_name)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "unable to perform snapshot");
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}

	free_rpc_longhorn_volume_snapshot(&req);
}

SPDK_RPC_REGISTER("longhorn_volume_snapshot", rpc_longhorn_volume_snapshot_cmd, SPDK_RPC_RUNTIME)

struct rpc_longhorn_bdev_compare {
	char *bdev1;
	char *bdev2;
};

static void
free_rpc_longhorn_bdev_compare(struct rpc_longhorn_bdev_compare *req) {
	free(req->bdev1);
	free(req->bdev2);
}

static const struct spdk_json_object_decoder rpc_longhorn_bdev_compare_decoders[] = {
	{"bdev1", offsetof(struct rpc_longhorn_bdev_compare, bdev1), spdk_json_decode_string},
	{"bdev2", offsetof(struct rpc_longhorn_bdev_compare, bdev2), spdk_json_decode_string},
};


static void compare_blocks_json(int status, struct block_diff *diff, void *arg)
{
	struct spdk_jsonrpc_request *request = arg;
	struct spdk_json_write_ctx *w;
	struct block *block;

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint64(w, "block_size", diff->blocksize);
	spdk_json_write_named_uint64(w, "size1", diff->size1);
	spdk_json_write_named_uint64(w, "size2", diff->size2);

	spdk_json_write_named_array_begin(w, "blocks");

	TAILQ_FOREACH(block, &diff->blocks, next) {
		spdk_json_write_uint64(w, block->block);
	}

        spdk_json_write_array_end(w);

        spdk_json_write_object_end(w);

        spdk_jsonrpc_end_result(request, w);
}

static void
rpc_longhorn_bdev_compare(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_longhorn_bdev_compare req = {};

	if (spdk_json_decode_object(params, rpc_longhorn_bdev_compare_decoders, 
				    SPDK_COUNTOF(rpc_longhorn_bdev_compare_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "longhorn spdk_json_decode_object failed");
		return;
	}

	bdev_longhorn_compare(req.bdev1, req.bdev2, 4096, compare_blocks_json, request);

	free_rpc_longhorn_bdev_compare(&req);
}

SPDK_RPC_REGISTER("longhorn_bdev_compare", rpc_longhorn_bdev_compare, SPDK_RPC_RUNTIME)
