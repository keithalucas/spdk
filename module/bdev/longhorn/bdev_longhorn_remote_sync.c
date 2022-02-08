#include <sys/select.h>
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/jsonrpc.h"
#include "spdk/thread.h"
#include "spdk_internal/lvolstore.h"
#include "../lvol/vbdev_lvol.h"
#include "lib/blob/blobstore.h"
#include "bdev_longhorn_lvol.h"


#ifndef MAX
#define MAX(a,b)        ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#endif


struct longhorn_server_connection_entry {
	int fd;

	int busy;
	struct spdk_lvol_store *lvs;


	TAILQ_ENTRY(longhorn_server_connection_entry) entries;
	
};

struct longhorn_server_entry {
	struct sockaddr_in addr;
	int fd;
	struct spdk_lvol_store *lvs;

	TAILQ_HEAD(, longhorn_server_connection_entry) connections;

	TAILQ_ENTRY(longhorn_server_entry) entries;
};

static TAILQ_HEAD(, longhorn_server_entry) sync_servers = TAILQ_HEAD_INITIALIZER(sync_servers);

static int sync_connection_readable(struct longhorn_server_connection_entry *entry) {
	uint64_t blob_id;


	printf("fd readable\n");
	if (!entry->busy) {
		read(entry->fd, &blob_id, sizeof(blob_id));

		printf("%lx %lu\n", blob_id, blob_id);

	
		entry->busy = 1;

		longhorn_lvol_transmit(entry->fd, blob_id, entry->lvs->blobstore, &entry->busy);

	}
	
	return 0;
}
		
static void set_nonblocking(int fd) {
	int fdflags = fcntl(fd, F_GETFL);

	fdflags |= O_NONBLOCK;
	fcntl(fd, F_SETFL, fdflags);
}



struct longhorn_server_connection_entry *longhorn_new_connection(int fd, struct spdk_lvol_store *lvs) {
	struct longhorn_server_connection_entry *entry;
	struct sockaddr_in remote_addr = {'\0'};
	socklen_t addrlen = sizeof(remote_addr);
	int remote_fd;

	remote_fd = accept(fd, (struct sockaddr *)&remote_addr, &addrlen);

	set_nonblocking(remote_fd);

	if (remote_fd > 0) {
		entry = calloc(1, sizeof(struct longhorn_server_connection_entry));
		entry->fd = remote_fd;
		entry->lvs = lvs;

		return entry;
	}

	return NULL;
}


static int longhorn_sync_poll(void *arg) {
	fd_set rdset;
	fd_set wrset;
	fd_set errset;
	struct longhorn_server_entry *entry;
	struct longhorn_server_connection_entry *connection;
	struct longhorn_server_connection_entry *next_connection;
	struct longhorn_server_connection_entry *new_connection;
	int max_fd = 0;
	struct timeval timeout = {0, 0};

	FD_ZERO(&rdset);
	FD_ZERO(&errset);
	FD_ZERO(&wrset);

	TAILQ_FOREACH(entry, &sync_servers, entries) {
		max_fd = MAX(max_fd, entry->fd);

		FD_SET(entry->fd, &rdset);
		FD_SET(entry->fd, &errset);
		TAILQ_FOREACH(connection, &entry->connections, entries) {
			max_fd = MAX(max_fd, connection->fd);

			FD_SET(connection->fd, &rdset);
			FD_SET(connection->fd, &errset);
			FD_SET(connection->fd, &wrset);
		}
	}

	if (select(max_fd + 1, &rdset, NULL, &errset, &timeout) > 0) {
		TAILQ_FOREACH(entry, &sync_servers, entries) {
			if (FD_ISSET(entry->fd, &rdset)) {
				new_connection = longhorn_new_connection(entry->fd, entry->lvs);
				
				if (new_connection != NULL) {
					TAILQ_INSERT_TAIL(&entry->connections, new_connection, entries);
				}

			}

			connection = TAILQ_FIRST(&entry->connections);

			while(connection != NULL) {
				next_connection = TAILQ_NEXT(connection, entries);

				if (FD_ISSET(connection->fd, &rdset)) {
					sync_connection_readable(connection);
				}

				connection = next_connection;
			}
		}

	}

	return SPDK_POLLER_BUSY;


}

static struct spdk_poller *poller = NULL;

int longhorn_remote_sync_server(const char *addr, uint16_t port, struct spdk_lvol_store *lvs) {
	struct longhorn_server_entry *entry;

        entry = calloc(1, sizeof(struct longhorn_server_entry));

        inet_aton(addr, &entry->addr.sin_addr);
        entry->addr.sin_port = htons(port);
        entry->addr.sin_family = AF_INET;

	/* TODO check return values */
	entry->fd = socket(AF_INET, SOCK_STREAM, 0);

	bind(entry->fd, (struct sockaddr *) &entry->addr, sizeof(struct sockaddr_in));

	listen(entry->fd, 10);

	set_nonblocking(entry->fd);

	entry->lvs = lvs;

	TAILQ_INIT(&entry->connections);
	TAILQ_INSERT_TAIL(&sync_servers, entry, entries);

	if (poller == NULL) {
		poller = SPDK_POLLER_REGISTER(longhorn_sync_poll, NULL, 4000);
	}

	return 0;
}





