#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "spdk/stdinc.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/jsonrpc.h"
#include "spdk/env.h"
#include "spdk/init.h"
#include "spdk/thread.h"
#include "bdev_longhorn_rebuild.h"
#include "bdev_longhorn_remote_sync.h"
#include "bdev_longhorn_sync_client.h"
#include "bdev_longhorn_lvol.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"

struct rpc_lvol_list_children {
	char *name;
};

static const struct spdk_json_object_decoder rpc_lvol_list_children_decoders[] = {
	{"name", offsetof(struct rpc_lvol_list_children, name), spdk_json_decode_string, true},
};

struct longhorn_child_blob_context {
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request;
};


static void longhorn_child_blob_info(struct longhorn_blob_info *info, void *cb_arg) {
	struct longhorn_child_blob_context *ctx = cb_arg;
	uint64_t i;

	if (info) {
		spdk_json_write_object_begin(ctx->w);

		spdk_json_write_named_string(ctx->w, "name", info->name);
		spdk_json_write_named_uint64(ctx->w, "num_clusters", info->num_clusters);
		spdk_json_write_named_uint32(ctx->w, "allocated_clusters", info->allocated_clusters);

		spdk_json_write_named_array_begin(ctx->w, "active_clusters");
		for (i = 0; i < info->allocated_clusters; ++i) {
 		       spdk_json_write_uint32(ctx->w, info->table[i]);
	       	}	       
		spdk_json_write_array_end(ctx->w);

		spdk_json_write_object_end(ctx->w);
	} else {
		spdk_json_write_array_end(ctx->w);
		spdk_json_write_object_end(ctx->w);

        	spdk_jsonrpc_end_result(ctx->request, ctx->w);
		free(ctx);
	}

}


static void 
rpc_bdev_lvol_list_children(struct spdk_jsonrpc_request *request, 
			    const struct spdk_json_val *params)
{
	struct rpc_lvol_list_children req = {NULL};
	struct spdk_json_write_ctx *w;
        struct spdk_bdev *bdev = NULL;
        struct spdk_lvol *lvol = NULL;
	struct longhorn_child_blob_context *ctx = NULL;

	if (spdk_json_decode_object(params, rpc_lvol_list_children_decoders,
                                    SPDK_COUNTOF(rpc_lvol_list_children_decoders),
                                    &req)) {
                SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
		return;
        }



	if (!req.name) {
                        spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "Name must be provided");
	}



	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	bdev = spdk_bdev_get_by_name(req.name);

        if (bdev != NULL) {
                lvol = vbdev_lvol_get_from_bdev(bdev);

                if (lvol != NULL) {

			spdk_json_write_named_string(w, "name", req.name);
			spdk_json_write_named_uint64(w, "cluster_size", lvol->lvol_store->blobstore->cluster_sz);
			spdk_json_write_named_uint32(w, "io_unit_size", lvol->lvol_store->blobstore->io_unit_size);


			spdk_json_write_named_array_begin(w, "snapshots");

			ctx = calloc(1, sizeof(*ctx));
			ctx->w = w;
			ctx->request  = request;
			longhorn_get_blob_info(lvol->lvol_store->blobstore, lvol->blob_id, longhorn_child_blob_info, ctx);
#if 0



			while (parent_id != SPDK_BLOBID_INVALID) {
				spdk_json_write_uint64(w, parent_id);
				parent_id = spdk_blob_get_parent_snapshot(lvol->lvol_store->blobstore, parent_id);
			}
#endif
	//spdk_json_write_array_end(w);
                } 
        }

#if 0
	spdk_json_write_object_end(w);



        spdk_jsonrpc_end_result(request, w);
#endif

}
SPDK_RPC_REGISTER("lvol_list_children", rpc_bdev_lvol_list_children, SPDK_RPC_RUNTIME)


struct rpc_lvol_list_children_remote {
	char *address;
	uint16_t *port;
	char *name;
};
static const struct spdk_json_object_decoder rpc_lvol_list_children_remote_decoders[] = {
	{"address", offsetof(struct rpc_lvol_list_children_remote, address), spdk_json_decode_string, true},
        {"port", offsetof(struct rpc_lvol_list_children_remote, port), spdk_json_decode_uint16, true}, 
	{"name", offsetof(struct rpc_lvol_list_children_remote, name), spdk_json_decode_string, true},
};

static void 
rpc_bdev_lvol_list_children_remote(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_lvol_list_children_remote req = {NULL};
	struct spdk_json_write_ctx *w;


	if (spdk_json_decode_object(params, rpc_lvol_list_children_remote_decoders,
                                    SPDK_COUNTOF(rpc_lvol_list_children_remote_decoders),
                                    &req)) {
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
		return;
	}

	bdev_longhorn_get_children_remote(req.address, req.port, req.name);
}

struct rpc_lvol_rebuild_remote {
	char *address;
	uint16_t *port;
	char *name;
	char *remote_prefix;
	char *lvs;

};


static const struct spdk_json_object_decoder rpc_lvol_rebuild_remote_decoders[] = {
	{"address", offsetof(struct rpc_lvol_rebuild_remote, address), spdk_json_decode_string, true},
        {"port", offsetof(struct rpc_lvol_rebuild_remote, port), spdk_json_decode_uint16, true}, 
	{"name", offsetof(struct rpc_lvol_rebuild_remote, name), spdk_json_decode_string, true},
	{"remote_prefix", offsetof(struct rpc_lvol_rebuild_remote, remote_prefix), spdk_json_decode_string, true},
	{"lvs", offsetof(struct rpc_lvol_rebuild_remote, lvs), spdk_json_decode_string, true},
};

static void 
rpc_bdev_lvol_rebuild_remote(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_lvol_rebuild_remote req = {NULL};
	struct spdk_json_write_ctx *w;
	struct spdk_lvol_store *lvs;


	if (spdk_json_decode_object(params, rpc_lvol_rebuild_remote_decoders,
                                    SPDK_COUNTOF(rpc_lvol_rebuild_remote_decoders),
                                    &req)) {
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
		return;
	}

	lvs = longhorn_get_lvol_store_by_name(req.lvs);

	bdev_longhorn_rebuild_remote(req.address, req.port, req.name, req.remote_prefix, lvs);
}


SPDK_RPC_REGISTER("lvol_rebuild_remote", rpc_bdev_lvol_rebuild_remote, SPDK_RPC_RUNTIME)


struct rpc_lvol_import {
	char *name;
	char *lvs;
	char *file;
};

static const struct spdk_json_object_decoder rpc_lvol_import_decoders[] = {
	{"name", offsetof(struct rpc_lvol_import, name), spdk_json_decode_string, true},
	{"lvs", offsetof(struct rpc_lvol_import, lvs), spdk_json_decode_string, true},
        {"file", offsetof(struct rpc_lvol_import, file), spdk_json_decode_string, true}, 
};

static void 
rpc_bdev_lvol_import(struct spdk_jsonrpc_request *request, 
			    const struct spdk_json_val *params)
{
	struct rpc_lvol_import req = {NULL};
	struct spdk_json_write_ctx *w;


	if (spdk_json_decode_object(params, rpc_lvol_import_decoders,
                                    SPDK_COUNTOF(rpc_lvol_import_decoders),
                                    &req)) {
                SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
		//free_rpc_construct_malloc(&req);
		return;
        }


	if (!req.name) {
                        spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "Name must be provided");
	}

	if (!req.file) {
                        spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "Name must be provided");
	}



	w = spdk_jsonrpc_begin_result(request);
        spdk_json_write_string(w, req.name);
        spdk_json_write_string(w, req.file);
        spdk_json_write_string(w, req.lvs);


        spdk_jsonrpc_end_result(request, w);


	bdev_longhorn_import(req.name, req.lvs, req.file);
}

SPDK_RPC_REGISTER("lvol_import", rpc_bdev_lvol_import, SPDK_RPC_RUNTIME)

struct rpc_tcp_json_server {
	char *address;
	uint16_t port;
};

static const struct spdk_json_object_decoder rpc_tcp_json_server_decoders[] = {
	{"address", offsetof(struct rpc_tcp_json_server, address), spdk_json_decode_string, true},
        {"port", offsetof(struct rpc_tcp_json_server, port), spdk_json_decode_uint16, true}, 
};

struct tcp_server_entry {
	struct sockaddr_in addr;
	struct spdk_jsonrpc_server *server;
	struct spdk_poller *poller;

	TAILQ_ENTRY(tcp_server_entry) entries;

};

static TAILQ_HEAD(, tcp_server_entry) tcp_servers = TAILQ_HEAD_INITIALIZER(tcp_servers);

static int 
tcp_server_poll(void *arg)
{
	struct tcp_server_entry *entry = arg;
	spdk_jsonrpc_server_poll(entry->server);
	return SPDK_POLLER_BUSY;
}

static void 
rpc_create_tcp_json_server(struct spdk_jsonrpc_request *request, 
			   const struct spdk_json_val *params)
{
	struct rpc_tcp_json_server req = {NULL};
	struct tcp_server_entry *entry;

        if (spdk_json_decode_object(params, rpc_tcp_json_server_decoders,
                                    SPDK_COUNTOF(rpc_tcp_json_server_decoders),
                                    &req)) {
                SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
                return;
        }

	printf("%s:%d\n", req.address, req.port);


	entry = calloc(1, sizeof(struct tcp_server_entry));

	inet_aton(req.address, &entry->addr.sin_addr);
	entry->addr.sin_port = htons(req.port);
	entry->addr.sin_family = AF_INET;

	entry->server = spdk_jsonrpc_server_listen(AF_INET, 0, &entry->addr, 
						    sizeof(struct sockaddr_in), 
						    spdk_rpc_handler);
	entry->poller = SPDK_POLLER_REGISTER(tcp_server_poll, entry, 4000);

	TAILQ_INSERT_TAIL(&tcp_servers, entry, entries);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("tcp_json_server", rpc_create_tcp_json_server, SPDK_RPC_RUNTIME)

struct rpc_link_lvols {
	char *child;
	char *parent;
};

static const struct spdk_json_object_decoder rpc_link_lvols_decoder[] = {
	{"child", offsetof(struct rpc_link_lvols, child), spdk_json_decode_string, true},
	{"parent", offsetof(struct rpc_link_lvols, parent), spdk_json_decode_string, true},
};

static void 
rpc_lvol_link(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_link_lvols req = {NULL};

	if (spdk_json_decode_object(params, rpc_link_lvols_decoder,
                                    SPDK_COUNTOF(rpc_link_lvols_decoder),
                                    &req)) {
                SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
		return;
        }

	printf("%s:%s\n", req.child, req.parent);

	bdev_longhorn_link(req.child, req.parent);
}

SPDK_RPC_REGISTER("link_lvols", rpc_lvol_link, SPDK_RPC_RUNTIME)

struct rpc_tcp_sync_server {
	char *address;
	uint16_t port;
	char *lvs;
};

static const struct spdk_json_object_decoder rpc_tcp_sync_server_decoders[] = {
	{"address", offsetof(struct rpc_tcp_sync_server, address), spdk_json_decode_string, true},
        {"port", offsetof(struct rpc_tcp_sync_server, port), spdk_json_decode_uint16, true}, 
        {"lvs", offsetof(struct rpc_tcp_sync_server, lvs), spdk_json_decode_string, true}, 
};

static void 
rpc_create_tcp_sync_server(struct spdk_jsonrpc_request *request, 
			   const struct spdk_json_val *params)
{
	struct rpc_tcp_sync_server req = {NULL};
	struct spdk_lvol_store *lvs;

        if (spdk_json_decode_object(params, rpc_tcp_sync_server_decoders,
                                    SPDK_COUNTOF(rpc_tcp_sync_server_decoders),
                                    &req)) {
                SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
                return;
        }


	lvs = longhorn_get_lvol_store_by_name(req.lvs);

	longhorn_remote_sync_server(req.address, req.port, lvs);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("tcp_sync_server", rpc_create_tcp_sync_server, SPDK_RPC_RUNTIME)

struct rpc_sync_client {
	char *address;
	uint16_t port;
	uint64_t blob_id;
	char *lvs;
};

static const struct spdk_json_object_decoder rpc_sync_client_decoders[] = {
	{"address", offsetof(struct rpc_sync_client, address), spdk_json_decode_string, true},
        {"port", offsetof(struct rpc_sync_client, port), spdk_json_decode_uint16, true}, 
        {"blob_id", offsetof(struct rpc_sync_client, blob_id), spdk_json_decode_uint64, true}, 
        {"lvs", offsetof(struct rpc_sync_client, lvs), spdk_json_decode_string, true}, 
};

static void 
rpc_create_sync_client(struct spdk_jsonrpc_request *request, 
			   const struct spdk_json_val *params)
{
	struct rpc_sync_client req = {NULL};
	struct spdk_lvol_store *lvs;

        if (spdk_json_decode_object(params, rpc_sync_client_decoders,
                                    SPDK_COUNTOF(rpc_sync_client_decoders),
                                    &req)) {
                SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
                spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                                 "spdk_json_decode_object failed");
                return;
        }

	lvs = longhorn_get_lvol_store_by_name(req.lvs);

	longhorn_sync_client(req.address, req.port, req.blob_id, lvs);

}

SPDK_RPC_REGISTER("sync_client", rpc_create_sync_client, SPDK_RPC_RUNTIME)
