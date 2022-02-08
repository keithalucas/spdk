#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/jsonrpc.h"
#include "spdk/thread.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"
#include "lib/blob/blobstore.h"

#include "bdev_longhorn_lvol.h"

#include <sys/types.h> 
#include <sys/socket.h>
#include <errno.h>

#define ALIGN_4K 4096

enum state {
	NAME,
	HEADER,
	TABLE,
	DATA,
	DONE
};

struct longhorn_sync_client_context {
	uint64_t blob_id;
	int fd;

	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	struct spdk_io_channel *channel;



	enum state state;
	size_t remaining; 
	char name[256];
	struct longhorn_lvol_header header;

	uint32_t *table;

	uint8_t *current;

        uint64_t io_units_per_cluster;

        uint8_t *cluster;
	uint64_t pos;

	bool write_in_progress;

	struct spdk_poller *poller;
};

static int longhorn_read(struct longhorn_sync_client_context *ctx) {
	ssize_t nread;

	if (ctx->remaining == 0) return 1;

	printf("reading %d\n", ctx->remaining);
	nread = read(ctx->fd, ctx->current, ctx->remaining);
	printf("read %d\n", nread);

	if (nread > 0) {
		ctx->remaining -= nread;
		ctx->current += nread;
	} else if (nread < 0) {
		perror("read");
		return -1;
	}

	return ctx->remaining == 0;
}

static int longhorn_handle_name(struct longhorn_sync_client_context *ctx) {
	if (longhorn_read(ctx)) {
		printf("received name  = %s\n", ctx->name);

		ctx->state = HEADER;
		ctx->remaining = sizeof(ctx->header);
		ctx->current = &ctx->header;
	}

	return 0;
}

static void longhorn_lvol_create_complete_cb(void *arg, struct spdk_lvol *lvol, int lvolerrno) {
	struct longhorn_sync_client_context *ctx = arg;

	ctx->channel = spdk_bs_alloc_io_channel(ctx->lvs->blobstore);
	ctx->lvol = lvol;
}



static int longhorn_handle_header(struct longhorn_sync_client_context *ctx) {
	if (longhorn_read(ctx)) {
		printf("allocated clusters  = %lu\n", ctx->header.allocated_clusters);
		printf("num cluster  = %lu\n", ctx->header.num_clusters);

		vbdev_lvol_create(ctx->lvs, ctx->name, ctx->header.num_clusters * ctx->header.cluster_size, true, LVOL_CLEAR_WITH_DEFAULT, longhorn_lvol_create_complete_cb, ctx);

		ctx->cluster = spdk_malloc(ctx->header.cluster_size,  ALIGN_4K, NULL,
                                   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

		ctx->io_units_per_cluster = ctx->header.cluster_size / ctx->header.io_unit_size;


		ctx->state = TABLE;
		ctx->remaining = sizeof(uint32_t) * ctx->header.num_clusters;
		ctx->table = calloc(1, ctx->remaining);
		ctx->current = ctx->table;

	}

	return 0;
}

static int longhorn_handle_table(struct longhorn_sync_client_context *ctx) { 

	if (longhorn_read(ctx)) {
		printf("read table\n");
		ctx->state = DATA;
		ctx->remaining = ctx->header.cluster_size;
		ctx->current = ctx->cluster;


	}

	return 0;
}


static void longhorn_write_cb(void *arg, int bserrno) {
	struct longhorn_sync_client_context *ctx = arg;

	if (bserrno) {
		ctx->state = DONE;
		return;
	}

	if (++ctx->pos >= ctx->header.allocated_clusters) {
		printf("copy complete\n");
		ctx->state = DONE;
		return;
	}

	ctx->current = ctx->cluster;
	ctx->remaining = ctx->header.cluster_size;
	ctx->write_in_progress = false;
}


static int longhorn_handle_data(struct longhorn_sync_client_context *ctx) {
	uint64_t offset = ctx->table[ctx->pos] * ctx->io_units_per_cluster;

	if (longhorn_read(ctx)) {
		if (ctx->lvol != NULL && !ctx->write_in_progress) {
			printf("writing cluster %lu\n", ctx->table[ctx->pos]);
			ctx->write_in_progress = true;

			spdk_blob_io_write(ctx->lvol->blob, ctx->channel,
					   ctx->cluster, offset,
					   ctx->io_units_per_cluster,
					   longhorn_write_cb, ctx);
		}
	}

	return 0;
}
					   



static int longhorn_sync_client_poll(void *arg) {
	struct longhorn_sync_client_context *context = arg;
	struct timeval timeout = {0, 0};
	fd_set rdset;

	FD_ZERO(&rdset);
	
	FD_SET(context->fd, &rdset);

	if (select(context->fd + 1, &rdset, NULL, NULL, &timeout) > 0) {
		printf("client readable\n");
		switch (context->state) {
		case NAME: 
			longhorn_handle_name(context);
			break;
		case HEADER:
			longhorn_handle_header(context);
			break;
		case TABLE:
			longhorn_handle_table(context);
			break;
		case DATA:
			longhorn_handle_data(context);
			break;
		case DONE:
			break;
		}

	}

	return SPDK_POLLER_BUSY;
}

static void set_nonblocking(int fd) {
        int fdflags = fcntl(fd, F_GETFL);

        fdflags |= O_NONBLOCK;
        fcntl(fd, F_SETFL, fdflags);
}


int longhorn_sync_client(const char *addr, uint16_t port, uint64_t blob_id, struct spdk_lvol_store *lvs) {
	struct sockaddr_in sockaddr = {'\0'};
	int sockfd;
	struct longhorn_sync_client_context *ctx;

	inet_aton(addr, &sockaddr.sin_addr);
        sockaddr.sin_port = htons(port);
        sockaddr.sin_family = AF_INET;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	if (sockfd <= 0) {
		return -errno;
	}
	
	if (connect(sockfd, (struct sockaddr *) &sockaddr, sizeof(sockaddr)) < 0) {
		return -errno;
	}

	ctx = calloc(1, sizeof(struct longhorn_sync_client_context));
	ctx->fd = sockfd;
	ctx->state = NAME;
	ctx->remaining = sizeof(ctx->name);
	ctx->current = ctx->name;
	ctx->lvs = lvs;


	set_nonblocking(ctx->fd);

	ctx->poller = SPDK_POLLER_REGISTER(longhorn_sync_client_poll, ctx, 4000);

	write(sockfd, &blob_id, sizeof (uint64_t));

}
