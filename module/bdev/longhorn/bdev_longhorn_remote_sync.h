#ifndef SPDK_BDEV_LONGHORN_REMOTE_SYNC__H
#define SPDK_BDEV_LONGHORN_REMOTE_SYNC__H

int longhorn_remote_sync_server(const char *addr, uint16_t port, struct spdk_lvol_store *lvs);

#endif /* SPDK_BDEV_LONGHORN_REMOTE_SYNC__H */

