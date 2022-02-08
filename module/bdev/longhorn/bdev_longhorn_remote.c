#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/jsonrpc.h"
#include "spdk/thread.h"
#include "bdev_longhorn_remote.h"

struct tcp_client_handler_entry {
	char *command;
	int32_t id;

	void *arg;

	json_remote_response_handler_fn fn;

	TAILQ_ENTRY(tcp_client_handler_entry) entries;
};
	

struct tcp_client_entry {
	const char *addr;
	struct spdk_jsonrpc_client *client;

	TAILQ_HEAD(, tcp_client_handler_entry) handlers;

	TAILQ_ENTRY(tcp_client_entry) entries;
};

static TAILQ_HEAD(, tcp_client_entry) tcp_clients = TAILQ_HEAD_INITIALIZER(tcp_clients);
static struct spdk_poller *poller = NULL;

static void handler_free(struct tcp_client_handler_entry *handler) {
	if (handler) {
		free(handler->command);
		free(handler);
	}

}


static void remote_client_handle(struct tcp_client_entry *entry)
{
	struct spdk_jsonrpc_client_response *response = NULL;
	int32_t id = 0;
	struct tcp_client_handler_entry *handler = NULL;
	struct tcp_client_handler_entry *next = NULL;

	response = spdk_jsonrpc_client_get_response(entry->client);

	if (spdk_json_number_to_int32(response->id, &id) != 0) {
		printf("Unable to decode TCP client message.\n");

		spdk_jsonrpc_client_free_response(response);

		return;
	}

	handler = TAILQ_FIRST(&entry->handlers);

	while (handler != NULL) {
		next = TAILQ_NEXT(handler, entries);

		if (handler->id == id) {
			(*(handler->fn))(entry->addr, handler->command, id, 
				       response->result, response->error,
				       handler->arg);

			TAILQ_REMOVE(&entry->handlers, handler, entries);
			handler_free(handler);

			break;
		}

		handler = next;
	}

	spdk_jsonrpc_client_free_response(response);
}


static int remote_client_poll(void *arg)
{
	struct tcp_client_entry *entry = NULL;
	struct tcp_client_entry *next = NULL;
	int error = 0;

	entry = TAILQ_FIRST(&tcp_clients);

	while (entry != NULL) {
		next = TAILQ_NEXT(entry, entries);

		error = spdk_jsonrpc_client_poll(entry->client, 0);

		if (error > 0) {
			remote_client_handle(entry);
		} else if (error == -EIO) {
		}

		entry = next;
	}

	return SPDK_POLLER_BUSY;
}

static struct tcp_client_entry *
json_client_lookup(const char *addr) {
	struct tcp_client_entry *entry = NULL;

	TAILQ_FOREACH(entry, &tcp_clients, entries) {
		if (strcmp(addr, entry->addr) == 0) {
			return entry;
		}
	}
	
	return NULL;
}
	
int json_remote_client(const char *addr) 
{
	struct spdk_jsonrpc_client *client = NULL;
	struct tcp_client_entry *entry = json_client_lookup(addr);

	if (entry == NULL) {
		client = spdk_jsonrpc_client_connect(addr, AF_INET);

		if (client != NULL) {
			entry = calloc(1, sizeof(struct tcp_client_entry));

			entry->addr = strdup(addr);
			entry->client = client;
			TAILQ_INIT(&entry->handlers);

			TAILQ_INSERT_TAIL(&tcp_clients, entry, entries);

			if (poller == NULL) {
				poller = SPDK_POLLER_REGISTER(remote_client_poll,
							      NULL, 4000);
			}
		}
	}
	
	return 0;
}

int json_remote_client_send_command(const char *addr, 
				    const char *command,
				    int32_t id,
				    struct spdk_jsonrpc_client_request *request,
				    json_remote_response_handler_fn fn,
				    void *arg) {

	struct tcp_client_entry *entry = json_client_lookup(addr);
	struct tcp_client_handler_entry *handler;

	if (entry) {
		handler = calloc(1, sizeof(struct tcp_client_handler_entry));
		handler->command = strdup(command); 
		handler->id = id;
		handler->fn = fn;
		handler->arg = arg;

		TAILQ_INSERT_TAIL(&entry->handlers, handler, entries);
		spdk_jsonrpc_client_send_request(entry->client, request);
	}
	
	return 0;
}
