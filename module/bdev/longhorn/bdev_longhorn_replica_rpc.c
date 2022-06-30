#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk_internal/lvolstore.h"
#include "bdev_longhorn_replica.h"
#include "bdev_longhorn_lvol.h"
#include "bdev_longhorn_nvmf.h"

struct rpc_longhorn_replica {
	char *name;
	uint64_t size;
	char *lvs;
	char *addr;
	uint16_t port;
};

static const struct spdk_json_object_decoder rpc_longhorn_replica_create_decoders[] = {
	{"name", offsetof(struct rpc_longhorn_replica, name), spdk_json_decode_string, false},
	{"size", offsetof(struct rpc_longhorn_replica, size), spdk_json_decode_uint64, false},
	{"lvs", offsetof(struct rpc_longhorn_replica, lvs), spdk_json_decode_string, false},
	{"addr", offsetof(struct rpc_longhorn_replica, addr), spdk_json_decode_string, false},
	{"port", offsetof(struct rpc_longhorn_replica, port), spdk_json_decode_uint16, false},
};

static void
rpc_longhorn_replica_create_cb(struct spdk_lvol_store *lvs, 
			   const char *name, const char *nqn, void *arg) {
	struct spdk_jsonrpc_request *request = arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_longhorn_replica_create(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params) 
{
	struct rpc_longhorn_replica req = {};
	struct spdk_lvol_store *lvs = NULL;

	if (spdk_json_decode_object(params, rpc_longhorn_replica_create_decoders,
				    SPDK_COUNTOF(rpc_longhorn_replica_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return;
	}

	lvs = longhorn_get_lvol_store_by_name(req.lvs);

	if (lvs == NULL) {
		SPDK_ERRLOG("cannot find lvs: %s\n", req.lvs);
		return;
	}

	bdev_longhorn_replica_create(lvs, req.name, req.size, req.addr, req.port,
				     rpc_longhorn_replica_create_cb, request);

}

SPDK_RPC_REGISTER("longhorn_replica_create", rpc_longhorn_replica_create, SPDK_RPC_RUNTIME)


struct rpc_longhorn_replica_stop {
	char *name;
	char *lvs;
};

static const struct spdk_json_object_decoder rpc_longhorn_replica_stop_decoders[] = {
	{"name", offsetof(struct rpc_longhorn_replica_stop, name), spdk_json_decode_string, false},
	{"lvs", offsetof(struct rpc_longhorn_replica_stop, lvs), spdk_json_decode_string, false},
};


static void
rpc_longhorn_replica_stop_cb(struct spdk_nvmf_subsystem *subsystem,
                	     void *arg, int status) {
	struct spdk_jsonrpc_request *request = arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_longhorn_replica_stop(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params) 
{
	struct rpc_longhorn_replica_stop req = {};
	struct spdk_nvmf_tgt *tgt;
        struct spdk_nvmf_subsystem      *subsystem;
	char *nqn = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_longhorn_replica_stop_decoders,
				    SPDK_COUNTOF(rpc_longhorn_replica_stop_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return;
	}

	tgt = spdk_nvmf_get_tgt(NULL);
	nqn = longhorn_generate_replica_nqn(req.lvs, req.name);


        subsystem = spdk_nvmf_tgt_find_subsystem(tgt, nqn);

	if (subsystem != NULL) {
        	SPDK_DEBUGLOG("Removing subsystem: %s\n", nqn);
        	rc = spdk_nvmf_subsystem_stop(subsystem,
                                      	      rpc_longhorn_replica_stop_cb,
                                      	      request);

		if (rc == -EBUSY) {
			SPDK_ERRLOG("Subsystem currently in another state change try again later.\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 	 "Subsystem currently in another state change try again later.");
        	} else if (rc != 0) {
                	SPDK_ERRLOG("Unable to change state on subsystem. rc=%d\n", rc);
                	spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                     	     "Unable to change state on subsystem. rc=%d", rc);
        	}
	} else {
        	SPDK_DEBUGLOG("%s/%s not published on NVMeOF\n", req.lvs, req.name);
		spdk_jsonrpc_send_bool_response(request, true);
	}


	free(nqn);


}


SPDK_RPC_REGISTER("longhorn_replica_stop", rpc_longhorn_replica_stop, SPDK_RPC_RUNTIME)

//SPDK_RPC_REGISTER("longhorn_replica_remove", rpc_longhorn_replica_remove, SPDK_RPC_RUNTIME)

 
struct rpc_longhorn_replica_snapshot {
	char *name;
	char *snapshot;
	char *lvs;
};

static const struct spdk_json_object_decoder rpc_longhorn_replica_snapshot_decoders[] = {
	{"name", offsetof(struct rpc_longhorn_replica_snapshot, name), spdk_json_decode_string, false},
	{"snapshot", offsetof(struct rpc_longhorn_replica_snapshot, snapshot), spdk_json_decode_string, false},
	{"lvs", offsetof(struct rpc_longhorn_replica_snapshot, lvs), spdk_json_decode_string, false},
};


static void
rpc_longhorn_replica_do_snapshot(struct spdk_jsonrpc_request *request,
			    	 const struct spdk_json_val *params) 
{
	struct rpc_longhorn_replica_snapshot req = {};

	if (spdk_json_decode_object(params, rpc_longhorn_replica_snapshot_decoders,
				    SPDK_COUNTOF(rpc_longhorn_replica_snapshot_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return;
	}

}

SPDK_RPC_REGISTER("longhorn_replica_snapshot", rpc_longhorn_replica_do_snapshot, SPDK_RPC_RUNTIME)


struct rpc_longhorn_set_external_addr {
	char *addr;
};

static const struct spdk_json_object_decoder rpc_longhorn_set_external_addr_decoders[] = {
	{"addr", offsetof(struct rpc_longhorn_set_external_addr, addr), spdk_json_decode_string, false},
};

static void _longhorn_set_external_addr_cmd_cb(const char *addr, void *arg) {
	struct spdk_jsonrpc_request *request = arg;

	spdk_jsonrpc_send_bool_response(request, true);

}

static void
rpc_longhorn_set_external_addr_cmd(struct spdk_jsonrpc_request *request,
			    	   const struct spdk_json_val *params) 
{
	struct rpc_longhorn_set_external_addr req = {};

	if (spdk_json_decode_object(params, rpc_longhorn_set_external_addr_decoders,
				    SPDK_COUNTOF(rpc_longhorn_set_external_addr_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return;
	}


	longhorn_set_external_addr(req.addr, _longhorn_set_external_addr_cmd_cb, request);

}


SPDK_RPC_REGISTER("longhorn_set_external_address", rpc_longhorn_set_external_addr_cmd, SPDK_RPC_RUNTIME)
