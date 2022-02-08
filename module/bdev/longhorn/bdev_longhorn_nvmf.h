#ifndef _BDEV_LONGHORN_NVMF_H_
#define _BDEV_LONGHORN_NVMF_H_

#include "spdk/nvmf.h"


#define VOLUME_FORMAT "nqn.2021-12.io.longhorn.volume:%s"
#define REPLICA_FORMAT "nqn.2021-12.io.longhorn.replica:%s/%s"
#define SNAPSHOT_FORMAT "nqn.2021-12.io.longhorn.snapshot:%s"

void longhorn_nvmf_create_transport(spdk_nvmf_tgt_add_transport_done_fn cb_fn,
                                    void *cb_arg);
void longhorn_nvmf_create_subsystem(const char *nqn);

typedef void (*longhorn_publish_nvmf_cb)(void *arg);
void longhorn_publish_nvmf(const char *bdev, const char *nqn, const char *addr, uint16_t port, longhorn_publish_nvmf_cb cb_fn, void *cb_arg); 

typedef void (*longhorn_set_external_addr_cb)(const char *addr, void *arg);

void longhorn_set_external_addr(const char *addr,
                                longhorn_set_external_addr_cb cb_fn,
                                void *cb_arg);

typedef void (*longhorn_attach_nvmf_cb)(const char **bdev_names, size_t bdev_cnt, int status, void *arg);
void longhorn_attach_nvmf(const char *bdev_name_prefix, const char *nqn, const char *addr, uint16_t port, longhorn_attach_nvmf_cb cb_fn, void *cb_arg);

#endif /* _BDEV_LONGHORN_NVMF_H_ */

