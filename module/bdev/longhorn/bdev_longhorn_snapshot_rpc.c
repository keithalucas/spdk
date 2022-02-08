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

