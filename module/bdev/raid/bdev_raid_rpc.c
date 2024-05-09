/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_raid.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/bit_array.h"

#define RPC_MAX_BASE_BDEVS 255

/*
 * Input structure for bdev_raid_get_bdevs RPC
 */
struct rpc_bdev_raid_get_bdevs {
	/* category - all or online or configuring or offline */
	char *category;
};

/*
 * brief:
 * free_rpc_bdev_raid_get_bdevs function frees RPC bdev_raid_get_bdevs related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_raid_get_bdevs(struct rpc_bdev_raid_get_bdevs *req)
{
	free(req->category);
}

/*
 * Decoder object for RPC get_raids
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_get_bdevs_decoders[] = {
	{"category", offsetof(struct rpc_bdev_raid_get_bdevs, category), spdk_json_decode_string},
};

/*
 * brief:
 * rpc_bdev_raid_get_bdevs function is the RPC for rpc_bdev_raid_get_bdevs. This is used to list
 * all the raid bdev names based on the input category requested. Category should be
 * one of "all", "online", "configuring" or "offline". "all" means all the raids
 * whether they are online or configuring or offline. "online" is the raid bdev which
 * is registered with bdev layer. "configuring" is the raid bdev which does not have
 * full configuration discovered yet. "offline" is the raid bdev which is not
 * registered with bdev as of now and it has encountered any error or user has
 * requested to offline the raid.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_get_bdevs(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_get_bdevs   req = {};
	struct spdk_json_write_ctx  *w;
	struct raid_bdev            *raid_bdev;
	enum raid_bdev_state        state;

	if (spdk_json_decode_object(params, rpc_bdev_raid_get_bdevs_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_get_bdevs_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	state = raid_bdev_str_to_state(req.category);
	if (state == RAID_BDEV_STATE_MAX && strcmp(req.category, "all") != 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	/* Get raid bdev list based on the category requested */
	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (raid_bdev->state == state || state == RAID_BDEV_STATE_MAX) {
			char uuid_str[SPDK_UUID_STRING_LEN];

			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", raid_bdev->bdev.name);
			spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &raid_bdev->bdev.uuid);
			spdk_json_write_named_string(w, "uuid", uuid_str);
			raid_bdev_write_info_json(raid_bdev, w);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_raid_get_bdevs(&req);
}
SPDK_RPC_REGISTER("bdev_raid_get_bdevs", rpc_bdev_raid_get_bdevs, SPDK_RPC_RUNTIME)

/*
 * Base bdevs in RPC bdev_raid_create
 */
struct rpc_bdev_raid_create_base_bdevs {
	/* Number of base bdevs */
	size_t           num_base_bdevs;

	/* List of base bdevs names */
	char             *base_bdevs[RPC_MAX_BASE_BDEVS];
};

/*
 * Input structure for RPC rpc_bdev_raid_create
 */
struct rpc_bdev_raid_create {
	/* Raid bdev name */
	char                                 *name;

	/* RAID strip size in KB */
	uint32_t                             strip_size_kb;

	/* RAID raid level */
	enum raid_level                      level;

	/* Base bdevs information */
	struct rpc_bdev_raid_create_base_bdevs base_bdevs;

	/* UUID for this raid bdev */
	struct spdk_uuid		     uuid;

	/* If set, information about raid bdev will be stored in superblock on each base bdev */
	bool                                 superblock_enabled;

	/* Enable the recording of a delta bitmap for faulty base bdevs */
	bool				     delta_bitmap_enabled;
};

/*
 * Decoder function for RPC bdev_raid_create to decode raid level
 */
static int
decode_raid_level(const struct spdk_json_val *val, void *out)
{
	int ret;
	char *str = NULL;
	enum raid_level level;

	ret = spdk_json_decode_string(val, &str);
	if (ret == 0 && str != NULL) {
		level = raid_bdev_str_to_level(str);
		if (level == INVALID_RAID_LEVEL) {
			ret = -EINVAL;
		} else {
			*(enum raid_level *)out = level;
		}
	}

	free(str);
	return ret;
}

/*
 * Decoder function for RPC bdev_raid_create to decode base bdevs list
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_raid_create_base_bdevs *base_bdevs = out;
	return spdk_json_decode_array(val, spdk_json_decode_string, base_bdevs->base_bdevs,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(char *));
}

/*
 * Decoder object for RPC bdev_raid_create
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_raid_create, name), spdk_json_decode_string},
	{"strip_size_kb", offsetof(struct rpc_bdev_raid_create, strip_size_kb), spdk_json_decode_uint32, true},
	{"raid_level", offsetof(struct rpc_bdev_raid_create, level), decode_raid_level},
	{"base_bdevs", offsetof(struct rpc_bdev_raid_create, base_bdevs), decode_base_bdevs},
	{"uuid", offsetof(struct rpc_bdev_raid_create, uuid), spdk_json_decode_uuid, true},
	{"superblock", offsetof(struct rpc_bdev_raid_create, superblock_enabled), spdk_json_decode_bool, true},
	{"delta_bitmap", offsetof(struct rpc_bdev_raid_create, delta_bitmap_enabled), spdk_json_decode_bool, true},
};

struct rpc_bdev_raid_create_ctx {
	struct rpc_bdev_raid_create req;
	struct raid_bdev *raid_bdev;
	struct spdk_jsonrpc_request *request;
	uint8_t remaining;
	int status;
};

static void
free_rpc_bdev_raid_create_ctx(struct rpc_bdev_raid_create_ctx *ctx)
{
	struct rpc_bdev_raid_create *req;
	size_t i;

	if (!ctx) {
		return;
	}

	req = &ctx->req;

	free(req->name);
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		free(req->base_bdevs.base_bdevs[i]);
	}

	free(ctx);
}

static void
rpc_bdev_raid_create_add_base_bdev_cb(void *_ctx, int status)
{
	struct rpc_bdev_raid_create_ctx *ctx = _ctx;

	if (status != 0) {
		ctx->status = status;
	}

	assert(ctx->remaining != 0);
	if (--ctx->remaining > 0) {
		return;
	}

	if (ctx->status != 0) {
		raid_bdev_delete(ctx->raid_bdev, NULL, NULL);
		spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
						     "Failed to create RAID bdev %s: %s",
						     ctx->req.name,
						     spdk_strerror(-ctx->status));
	} else {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

	free_rpc_bdev_raid_create_ctx(ctx);
}

/*
 * brief:
 * rpc_bdev_raid_create function is the RPC for creating RAID bdevs. It takes
 * input as raid bdev name, raid level, strip size in KB and list of base bdev names.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_create	*req;
	struct raid_bdev		*raid_bdev;
	int				rc;
	size_t				i;
	struct rpc_bdev_raid_create_ctx *ctx;
	uint8_t				num_base_bdevs;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}
	req = &ctx->req;

	if (spdk_json_decode_object(params, rpc_bdev_raid_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_create_decoders),
				    req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}
	num_base_bdevs = req->base_bdevs.num_base_bdevs;

	for (i = 0; i < num_base_bdevs; i++) {
		if (strlen(req->base_bdevs.base_bdevs[i]) == 0) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "The base bdev name cannot be empty: %s",
							     spdk_strerror(EINVAL));
			goto cleanup;
		}
	}

	rc = raid_bdev_create(req->name, req->strip_size_kb, num_base_bdevs,
			      req->level, req->superblock_enabled, &req->uuid,
			      req->delta_bitmap_enabled, &raid_bdev);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create RAID bdev %s: %s",
						     req->name, spdk_strerror(-rc));
		goto cleanup;
	}

	ctx->raid_bdev = raid_bdev;
	ctx->request = request;
	ctx->remaining = num_base_bdevs;

	assert(num_base_bdevs > 0);

	for (i = 0; i < num_base_bdevs; i++) {
		const char *base_bdev_name = req->base_bdevs.base_bdevs[i];

		rc = raid_bdev_add_base_bdev(raid_bdev, base_bdev_name,
					     rpc_bdev_raid_create_add_base_bdev_cb, ctx);
		if (rc == -ENODEV) {
			SPDK_DEBUGLOG(bdev_raid, "base bdev %s doesn't exist now\n", base_bdev_name);
			assert(ctx->remaining > 1 || i + 1 == num_base_bdevs);
			rpc_bdev_raid_create_add_base_bdev_cb(ctx, 0);
		} else if (rc != 0) {
			SPDK_DEBUGLOG(bdev_raid, "Failed to add base bdev %s to RAID bdev %s: %s",
				      base_bdev_name, req->name, spdk_strerror(-rc));
			ctx->remaining -= (num_base_bdevs - i - 1);
			rpc_bdev_raid_create_add_base_bdev_cb(ctx, rc);
			break;
		}
	}
	return;
cleanup:
	free_rpc_bdev_raid_create_ctx(ctx);
}
SPDK_RPC_REGISTER("bdev_raid_create", rpc_bdev_raid_create, SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC deleting a raid bdev
 */
struct rpc_bdev_raid_delete {
	/* raid bdev name */
	char *name;
};

/*
 * brief:
 * free_rpc_bdev_raid_delete function is used to free RPC bdev_raid_delete related parameters
 * params:
 * req - pointer to RPC request
 * params:
 * none
 */
static void
free_rpc_bdev_raid_delete(struct rpc_bdev_raid_delete *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC raid_bdev_delete
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_raid_delete, name), spdk_json_decode_string},
};

struct rpc_bdev_raid_delete_ctx {
	struct rpc_bdev_raid_delete req;
	struct spdk_jsonrpc_request *request;
};

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the deletion of the raid bdev.
 * returns:
 * none
 */
static void
bdev_raid_delete_done(void *cb_arg, int rc)
{
	struct rpc_bdev_raid_delete_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to delete raid bdev %s (%d): %s\n",
			    ctx->req.name, rc, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	spdk_jsonrpc_send_bool_response(request, true);
exit:
	free_rpc_bdev_raid_delete(&ctx->req);
	free(ctx);
}

/*
 * brief:
 * rpc_bdev_raid_delete function is the RPC for deleting a raid bdev. It takes raid
 * name as input and delete that raid bdev including freeing the base bdev
 * resources.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_delete_ctx *ctx;
	struct raid_bdev *raid_bdev;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_raid_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_delete_decoders),
				    &ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	raid_bdev = raid_bdev_find_by_name(ctx->req.name);
	if (raid_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
						     "raid bdev %s not found",
						     ctx->req.name);
		goto cleanup;
	}

	ctx->request = request;

	raid_bdev_delete(raid_bdev, bdev_raid_delete_done, ctx);

	return;

cleanup:
	free_rpc_bdev_raid_delete(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_raid_delete", rpc_bdev_raid_delete, SPDK_RPC_RUNTIME)

/*
 * Base bdevs in RPC bdev_raid_add_base_bdev
 */
struct rpc_bdev_raid_add_base_bdev {
	/* Base bdev name */
	char			*base_bdev;

	/* Raid bdev name */
	char			*raid_bdev;
};

/*
 * brief:
 * free_rpc_bdev_raid_add_base_bdev function is to free RPC
 * bdev_raid_add_base_bdev related parameters.
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_raid_add_base_bdev(struct rpc_bdev_raid_add_base_bdev *req)
{
	free(req->base_bdev);
	free(req->raid_bdev);
}

/*
 * Decoder object for RPC bdev_raid_add_base_bdev
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_add_base_bdev_decoders[] = {
	{"base_bdev", offsetof(struct rpc_bdev_raid_add_base_bdev, base_bdev), spdk_json_decode_string},
	{"raid_bdev", offsetof(struct rpc_bdev_raid_add_base_bdev, raid_bdev), spdk_json_decode_string},
};

static void
rpc_bdev_raid_add_base_bdev_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, status, "Failed to add base bdev to RAID bdev: %s",
						     spdk_strerror(-status));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_raid_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

/*
 * brief:
 * bdev_raid_add_base_bdev function is the RPC for adding base bdev to a raid bdev.
 * It takes base bdev and raid bdev names as input.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_add_base_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_add_base_bdev req = {};
	struct raid_bdev *raid_bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_raid_add_base_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_add_base_bdev_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	raid_bdev = raid_bdev_find_by_name(req.raid_bdev);
	if (raid_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV, "raid bdev %s is not found in config",
						     req.raid_bdev);
		goto cleanup;
	}

	rc = raid_bdev_add_base_bdev(raid_bdev, req.base_bdev, rpc_bdev_raid_add_base_bdev_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to add base bdev %s to RAID bdev %s: %s",
						     req.base_bdev, req.raid_bdev,
						     spdk_strerror(-rc));
		goto cleanup;
	}

cleanup:
	free_rpc_bdev_raid_add_base_bdev(&req);
}
SPDK_RPC_REGISTER("bdev_raid_add_base_bdev", rpc_bdev_raid_add_base_bdev, SPDK_RPC_RUNTIME)

/*
 * Decoder object for RPC bdev_raid_remove_base_bdev
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_remove_base_bdev_decoders[] = {
	{"name", 0, spdk_json_decode_string},
};

static void
rpc_bdev_raid_remove_base_bdev_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, status, "Failed to remove base bdev from raid bdev");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

/*
 * brief:
 * bdev_raid_remove_base_bdev function is the RPC for removing base bdev from a raid bdev.
 * It takes base bdev name as input.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_remove_base_bdev(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct spdk_bdev_desc *desc;
	char *name = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_raid_remove_base_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_remove_base_bdev_decoders),
				    &name)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_bdev_open_ext(name, false, rpc_bdev_raid_event_cb, NULL, &desc);
	free(name);
	if (rc != 0) {
		goto err;
	}

	rc = raid_bdev_remove_base_bdev(spdk_bdev_desc_get_bdev(desc), rpc_bdev_raid_remove_base_bdev_done,
					request);
	spdk_bdev_close(desc);
	if (rc != 0) {
		goto err;
	}

	return;
err:
	rpc_bdev_raid_remove_base_bdev_done(request, rc);
}
SPDK_RPC_REGISTER("bdev_raid_remove_base_bdev", rpc_bdev_raid_remove_base_bdev, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_raid_options_decoders[] = {
	{"process_window_size_kb", offsetof(struct spdk_raid_bdev_opts, process_window_size_kb), spdk_json_decode_uint32, true},
	{"process_max_bandwidth_mb_sec", offsetof(struct spdk_raid_bdev_opts, process_max_bandwidth_mb_sec), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_raid_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_raid_bdev_opts opts;
	int rc;

	raid_bdev_get_opts(&opts);
	if (params && spdk_json_decode_object(params, rpc_bdev_raid_options_decoders,
					      SPDK_COUNTOF(rpc_bdev_raid_options_decoders),
					      &opts)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = raid_bdev_set_opts(&opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}

	return;
}
SPDK_RPC_REGISTER("bdev_raid_set_options", rpc_bdev_raid_set_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC rpc_bdev_raid_grow_base_bdev
 */
struct rpc_bdev_raid_grow_base_bdev {
	/* Raid bdev name */
	char *raid_bdev_name;

	/* Base bdev name */
	char *base_bdev_name;
};

/*
 * brief:
 * free_rpc_bdev_raid_grow_base_bdev frees RPC bdev_raid_grow_base_bdev related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_raid_grow_base_bdev(struct rpc_bdev_raid_grow_base_bdev *req)
{
	free(req->raid_bdev_name);
	free(req->base_bdev_name);
}

/*
 * Decoder object for RPC bdev_raid_grow_base_bdev
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_grow_base_bdev_decoders[] = {
	{"raid_name", offsetof(struct rpc_bdev_raid_grow_base_bdev, raid_bdev_name), spdk_json_decode_string},
	{"base_name", offsetof(struct rpc_bdev_raid_grow_base_bdev, base_bdev_name), spdk_json_decode_string},
};

struct rpc_bdev_raid_grow_base_bdev_ctx {
	struct rpc_bdev_raid_grow_base_bdev req;
	struct spdk_jsonrpc_request *request;
};

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the growing a base bdev.
 * returns:
 * none
 */
static void
bdev_raid_grow_base_bdev_done(void *cb_arg, int rc)
{
	struct rpc_bdev_raid_grow_base_bdev_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to grow raid %s adding base bdev %s (%d): %s\n",
			    ctx->req.raid_bdev_name, ctx->req.base_bdev_name, rc, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	spdk_jsonrpc_send_bool_response(request, true);
exit:
	free_rpc_bdev_raid_grow_base_bdev(&ctx->req);
	free(ctx);
}

/*
 * brief:
 * bdev_raid_grow_base_bdev is the RPC to add a base bdev to a raid bdev, growing the raid's size
 * if there isn't an empty base bdev slot. It takes raid bdev name and base bdev name as input.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_grow_base_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_grow_base_bdev_ctx *ctx;
	struct raid_bdev *raid_bdev;
	struct spdk_bdev *base_bdev;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_raid_grow_base_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_grow_base_bdev_decoders),
				    &ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	raid_bdev = raid_bdev_find_by_name(ctx->req.raid_bdev_name);
	if (raid_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
						     "raid bdev %s not found",
						     ctx->req.raid_bdev_name);
		goto cleanup;
	}

	base_bdev = spdk_bdev_get_by_name(ctx->req.base_bdev_name);
	if (base_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
						     "base bdev %s not found",
						     ctx->req.base_bdev_name);
		goto cleanup;
	}

	ctx->request = request;

	rc = raid_bdev_grow_base_bdev(raid_bdev, ctx->req.base_bdev_name, bdev_raid_grow_base_bdev_done,
				      ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to grow raid %s adding base bdev %s: %s",
						     ctx->req.raid_bdev_name, ctx->req.base_bdev_name,
						     spdk_strerror(-rc));
		goto cleanup;
	}

	return;

cleanup:
	free_rpc_bdev_raid_grow_base_bdev(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_raid_grow_base_bdev", rpc_bdev_raid_grow_base_bdev, SPDK_RPC_RUNTIME)

/* delta bitmap */

/* Structure to decode the input parameters for delta bitmap RPC methods. */
static const struct spdk_json_object_decoder rpc_bdev_raid_base_bdev_delta_bitmap_decoders[] = {
	{"base_bdev_name", 0, spdk_json_decode_string},
};

struct rpc_bdev_raid_delta_bitmap_ctx {
	char *base_bdev_name;
	struct spdk_jsonrpc_request *request;
};

static void
rpc_bdev_raid_get_base_bdev_delta_bitmap(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	char *base_bdev_name = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_bit_array *delta_bitmap;
	char *encoded;
	uint64_t region_size;
	int rc;

	rc = spdk_json_decode_object(params, rpc_bdev_raid_base_bdev_delta_bitmap_decoders,
				     SPDK_COUNTOF(rpc_bdev_raid_base_bdev_delta_bitmap_decoders),
				     &base_bdev_name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delta_bitmap = raid_bdev_get_base_bdev_delta_bitmap(base_bdev_name);
	if (delta_bitmap == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	encoded = spdk_bit_array_to_base64_string(delta_bitmap);
	if (encoded == NULL) {
		SPDK_ERRLOG("Failed to encode delta map to base64 string\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		goto cleanup;
	}

	region_size = raid_bdev_region_size_base_bdev_delta_bitmap(base_bdev_name);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint64(w, "region_size", region_size);
	spdk_json_write_named_string(w, "delta_bitmap", encoded);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

	free(encoded);

cleanup:
	free(base_bdev_name);
}
SPDK_RPC_REGISTER("bdev_raid_get_base_bdev_delta_bitmap", rpc_bdev_raid_get_base_bdev_delta_bitmap,
		  SPDK_RPC_RUNTIME)

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the delta bitmap stopping.
 * returns:
 * none
 */
static void
bdev_raid_stop_base_bdev_delta_bitmap_done(void *cb_arg, int rc)
{
	struct rpc_bdev_raid_delta_bitmap_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to stop base bdev %s delta map: %s",
			    ctx->base_bdev_name, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	spdk_jsonrpc_send_bool_response(request, true);
exit:
	free(ctx->base_bdev_name);
	free(ctx);
}

static void
rpc_bdev_raid_stop_base_bdev_delta_bitmap(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_delta_bitmap_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_raid_base_bdev_delta_bitmap_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_base_bdev_delta_bitmap_decoders),
				    &ctx->base_bdev_name)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->request = request;

	rc = raid_bdev_stop_base_bdev_delta_bitmap(ctx->base_bdev_name,
			bdev_raid_stop_base_bdev_delta_bitmap_done, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to stop base bdev %s delta map: %s",
						     ctx->base_bdev_name, spdk_strerror(-rc));
		goto cleanup;
	}

	return;

cleanup:
	if (ctx->base_bdev_name) {
		free(ctx->base_bdev_name);
	}
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_raid_stop_base_bdev_delta_bitmap",
		  rpc_bdev_raid_stop_base_bdev_delta_bitmap,
		  SPDK_RPC_RUNTIME)

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the faulty base bdev clearing.
 * returns:
 * none
 */
static void
bdev_raid_clear_base_bdev_faulty_state_done(void *cb_arg, int rc)
{
	struct rpc_bdev_raid_delta_bitmap_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to clear base bdev %s faulty state: %s",
			    ctx->base_bdev_name, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	spdk_jsonrpc_send_bool_response(request, true);
exit:
	free(ctx->base_bdev_name);
	free(ctx);
}

static void
rpc_bdev_raid_clear_base_bdev_faulty_state(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_delta_bitmap_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_raid_base_bdev_delta_bitmap_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_base_bdev_delta_bitmap_decoders),
				    &ctx->base_bdev_name)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->request = request;

	rc = raid_bdev_clear_base_bdev_faulty_state(ctx->base_bdev_name,
			bdev_raid_clear_base_bdev_faulty_state_done,
			ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to clear base bdev %s faulty state: %s",
						     ctx->base_bdev_name, spdk_strerror(-rc));
		goto cleanup;
	}

	return;

cleanup:
	if (ctx->base_bdev_name) {
		free(ctx->base_bdev_name);
	}
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_raid_clear_base_bdev_faulty_state",
		  rpc_bdev_raid_clear_base_bdev_faulty_state, SPDK_RPC_RUNTIME)
