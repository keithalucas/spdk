#ifndef SPDK_BDEV_LONGHORN_REMOTE__H
#define SPDK_BDEV_LONGHORN_REMOTE__H

typedef void (*json_remote_response_handler_fn)(const char *addr, 
					   const char *command,
					   int32_t id,
					   struct spdk_json_val *result,
					   struct spdk_json_val *error,
					   void *arg);

int json_remote_client(const char *addr);
int json_remote_client_send_command(const char *addr,
				    const char *command,
                                    int32_t id,
                                    struct spdk_jsonrpc_client_request *request,
                                    json_remote_response_handler_fn fn,
                                    void *arg); 



#endif /* SPDK_BDEV_LONGHORN_REMOTE__H */
