#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "bdev_longhorn_nvmf.h"


static bool tcp_transport_created = false;

static void
longhorn_tgt_add_transport_done(void *cb_arg, int status)
{

	tcp_transport_created = true;

}



static void
longhorn_subsystem_add_done(struct spdk_nvmf_subsystem *subsystem,
			    void *cb_arg, int status) {
}

void longhorn_nvmf_create_transport(spdk_nvmf_tgt_add_transport_done_fn cb_fn,
                                    void *cb_arg) {
	struct spdk_nvmf_transport_opts opts;
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_transport      *transport;

	spdk_nvmf_transport_opts_init("tcp", &opts, sizeof(opts));
	tgt = spdk_nvmf_get_tgt(NULL);

	transport = spdk_nvmf_transport_create("tcp", &opts);

	if (cb_fn != NULL) {
		spdk_nvmf_tgt_add_transport(tgt, transport, cb_fn, cb_arg);
	} else {
		spdk_nvmf_tgt_add_transport(tgt, transport, 
				    	    longhorn_tgt_add_transport_done, 
					    NULL);
	}
}


void longhorn_nvmf_create_subsystem(const char *nqn) {
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_subsystem      *subsystem;
	

	tgt = spdk_nvmf_get_tgt(NULL);

	subsystem = spdk_nvmf_subsystem_create(tgt, nqn, SPDK_NVMF_SUBTYPE_NVME,
                                               0);

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);

	spdk_nvmf_subsystem_start(subsystem, longhorn_subsystem_add_done, NULL);

}

static void populate_tcp_trid(struct spdk_nvme_transport_id *trid, const char *addr, uint16_t port) {
	snprintf(trid->trstring, SPDK_NVMF_TRSTRING_MAX_LEN, "TCP");

	trid->trtype = SPDK_NVME_TRANSPORT_TCP; trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;

	snprintf(trid->traddr, SPDK_NVMF_TRADDR_MAX_LEN, "%s", addr);
	snprintf(trid->trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN, "%"PRIu16, port);

}


static void add_listener_cb(void *cb_arg, int status) {
	struct spdk_nvme_transport_id *trid = cb_arg;

	free(trid);
}

static void add_listener_resume_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status) {
}

static void add_listener_pause_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status) {
	struct spdk_nvme_transport_id *trid = cb_arg;

	spdk_nvmf_subsystem_add_listener(subsystem, trid, add_listener_cb, trid);

	spdk_nvmf_subsystem_resume(subsystem, add_listener_resume_cb, NULL);
}


void longhorn_nvmf_subsystem_add_listener(const char *nqn, const char *addr, uint16_t port) { 
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_subsystem      *subsystem;
	struct spdk_nvme_transport_id *trid;
	
	tgt = spdk_nvmf_get_tgt(NULL);

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, nqn);

	trid = calloc(1, sizeof(*trid));
	populate_tcp_trid(trid, addr, port);

	spdk_nvmf_subsystem_pause(subsystem, 0, add_listener_pause_cb, trid);
}

static void add_ns_resume_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status) {
}

static void add_ns_pause_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status) {
	char *bdev_name = cb_arg;
        struct spdk_nvmf_ns_opts ns_opts;

        spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));

	spdk_nvmf_subsystem_add_ns_ext(subsystem, bdev_name, &ns_opts, sizeof(ns_opts), NULL);

	free(bdev_name);

	spdk_nvmf_subsystem_resume(subsystem, add_ns_resume_cb, NULL);
}

void longhorn_nvmf_subsystem_add_ns(const char *nqn, const char *bdev_name) { 
	struct spdk_nvmf_tgt *tgt = NULL;
	struct spdk_nvmf_subsystem      *subsystem;
	
	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, nqn);

	spdk_nvmf_subsystem_pause(subsystem, 0, add_ns_pause_cb, bdev_name);
}

struct longhorn_publish_nvmf_ctx {
	longhorn_publish_nvmf_cb cb_fn;
	void *cb_arg;
};

void longhorn_publish_nvmf(const char *bdev_name, const char *nqn, const char *addr, uint16_t port, longhorn_publish_nvmf_cb cb_fn, void *cb_arg) { 
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_subsystem      *subsystem;
        struct spdk_nvmf_ns_opts ns_opts;
        struct spdk_nvmf_listen_opts listen_opts;
        struct spdk_nvme_transport_id *trid;

	tgt = spdk_nvmf_get_tgt(NULL);

	subsystem = spdk_nvmf_subsystem_create(tgt, nqn, SPDK_NVMF_SUBTYPE_NVME,
                                               0);

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);


        spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));

	spdk_nvmf_subsystem_add_ns_ext(subsystem, bdev_name, &ns_opts, sizeof(ns_opts), NULL);

	trid = calloc(1, sizeof(*trid));
	populate_tcp_trid(trid, addr, port);

	spdk_nvmf_listen_opts_init(&listen_opts, sizeof(listen_opts));
	spdk_nvmf_tgt_listen_ext(tgt, trid, &listen_opts);
	spdk_nvmf_subsystem_add_listener(subsystem, trid, add_listener_cb, trid);

	spdk_nvmf_subsystem_start(subsystem, longhorn_subsystem_add_done, NULL);

}

#define NVME_MAX_BDEVS_PER_RPC 128

struct longhorn_attach_nvmf_ctx {
	uint32_t count;
        size_t bdev_cnt;
        const char *names[NVME_MAX_BDEVS_PER_RPC];
	struct spdk_nvme_ctrlr_opts opts;
	longhorn_attach_nvmf_cb cb_fn;
       	void *cb_arg;
};


static void longhorn_wait_for_examine_cb(void *cb_ctx) {
	struct longhorn_attach_nvmf_ctx *ctx = cb_ctx;

	ctx->cb_fn(ctx->names, ctx->bdev_cnt, 0, ctx->cb_arg);

	free(ctx);
}


static void longhorn_nvme_create_cb(void *cb_ctx, size_t bdev_cnt, int rc) {
	struct longhorn_attach_nvmf_ctx *ctx = cb_ctx;

	if (rc < 0) {
		ctx->cb_fn(NULL, 0, rc, ctx->cb_arg);
		free(ctx);
	} else {
		ctx->bdev_cnt = bdev_cnt;
		spdk_bdev_wait_for_examine(longhorn_wait_for_examine_cb, ctx);
	}
}

void longhorn_attach_nvmf(const char *bdev_name_prefix, const char *nqn, 
			  const char *addr, uint16_t port,
			  longhorn_attach_nvmf_cb cb_fn, void *cb_arg) {
        struct spdk_nvme_transport_id *trid;
	size_t len;
	struct spdk_nvme_host_id hostid = {};
        uint32_t prchk_flags = 0;
	struct longhorn_attach_nvmf_ctx *ctx;


	trid = calloc(1, sizeof(*trid));
	populate_tcp_trid(trid, addr, port);

	len = strlen(nqn);
	memcpy(trid->subnqn, nqn, len + 1);

	
	ctx = calloc(1, sizeof(*ctx));

	ctx->count = NVME_MAX_BDEVS_PER_RPC;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->opts, sizeof(ctx->opts));

	bdev_nvme_create(trid, &hostid, bdev_name_prefix, ctx->names, ctx->count, 
			 prchk_flags, longhorn_nvme_create_cb, ctx, &ctx->opts);

}

static char *external_addr = NULL;

struct longhorn_set_external_addr_ctx {
	char *addr;
	longhorn_set_external_addr_cb cb_fn;
	void *cb_arg;
};


static void
longhorn_external_addr_cb(void *cb_arg, int status)
{
	struct longhorn_set_external_addr_ctx *ctx = cb_arg;

	tcp_transport_created = true;

	ctx->cb_fn(ctx->addr, ctx->cb_arg);
	free(ctx);
}


void longhorn_set_external_addr(const char *addr, 
				longhorn_set_external_addr_cb cb_fn, 
				void *cb_arg) 
{
	external_addr = strdup(addr);
	
	if (tcp_transport_created) {
		cb_fn(external_addr, cb_arg);
	} else {
		struct longhorn_set_external_addr_ctx *ctx =
			calloc(1, sizeof(struct longhorn_set_external_addr_ctx));

		ctx->addr = external_addr;
		ctx->cb_fn = cb_fn;
		ctx->cb_arg = cb_arg;

		longhorn_nvmf_create_transport(longhorn_external_addr_cb,
					       ctx);
	}


}

char *
longhorn_generate_replica_nqn(const char *lvs, const char *name) {
	char *nqn = spdk_sprintf_alloc(REPLICA_FORMAT, lvs, name);
	return nqn;
}



