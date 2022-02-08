#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "bdev_longhorn_lvol.h"
#include "bdev_longhorn_replica.h"
#include "bdev_longhorn_nvmf.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"
#include "lib/blob/blobstore.h"


void bdev_longhorn_replica_detect(const char *name) {	
	struct spdk_lvol_store *lvs = longhorn_get_lvol_store_by_name(name);


}

void bdev_longhorn_replica_get_info(const char *name) {
        struct spdk_bdev *bdev = NULL;
        struct spdk_lvol *lvol = NULL;
	spdk_blob_id parent_id;

	bdev = spdk_bdev_get_by_name(name);

	if (bdev != NULL) {
		lvol = vbdev_lvol_get_from_bdev(bdev);

		
	}

}

struct longhorn_replica_create_context {
	char *name;
	char *bdev_name;

	struct spdk_lvol_store *lvs;

	char *addr;
	uint16_t port;
	char *nqn;

	longhorn_replica_create_cb cb_fn;
	void *cb_arg;
};

static void longhorn_replica_create_context_free(struct longhorn_replica_create_context *ctx) 
{
	free(ctx->name);
	free(ctx->addr);
	free(ctx->nqn);
	free(ctx);
}

static void longhorn_replica_publish_complete_cb(void *arg) {
	struct longhorn_replica_create_context *ctx = arg;

	ctx->cb_fn(ctx->lvs, ctx->name, ctx->nqn, ctx->cb_arg);
	longhorn_replica_create_context_free(ctx);
}

static void longhorn_replica_create_complete_cb(void *arg,
						struct spdk_lvol *lvol,
						int volerrno)
{

	struct longhorn_replica_create_context *ctx = arg;
	

	if (ctx->addr && ctx->addr[0] != '\0') {
		longhorn_publish_nvmf(lvol->bdev->name, ctx->nqn, ctx->addr, ctx->port,
			      	      longhorn_replica_publish_complete_cb, ctx);
	} else {
		ctx->cb_fn(ctx->lvs, ctx->name, ctx->nqn, ctx->cb_arg);
		longhorn_replica_create_context_free(ctx);
	}

}

void bdev_longhorn_replica_create(struct spdk_lvol_store *lvs, 
				  const char *name,
				  uint64_t size,
				  const char *addr,
				  uint16_t port,
				  longhorn_replica_create_cb cb_fn,
				  void *cb_arg)
{
	struct longhorn_replica_create_context *ctx;
	struct spdk_bdev *bdev;

	/* TODO Lookup name to see if it exists.  */

	ctx = calloc(1, sizeof(*ctx));

	ctx->name = strdup(name);
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->lvs = lvs;
	ctx->addr = strdup(addr);
	ctx->port = port;


	ctx->bdev_name = spdk_sprintf_alloc("%s/%s", lvs->name, name);
	ctx->nqn = spdk_sprintf_alloc(REPLICA_FORMAT, lvs->name, name);

	bdev = spdk_bdev_get_by_name(ctx->bdev_name);
	if (bdev != NULL) {
		longhorn_replica_create_complete_cb(ctx,
						    vbdev_lvol_get_from_bdev(bdev),
						    0);
		
									 

	} else {

		vbdev_lvol_create(lvs, name, size, true, LVOL_CLEAR_WITH_DEFAULT,  
			  	  longhorn_replica_create_complete_cb, ctx);
	}
	
}

void bdev_longhorn_replica_snapshot(struct spdk_lvol_store *lvs, 
                                    const char *name,
				    const char *snapshot) 
{
	//vbdev_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
         //                  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg
}
