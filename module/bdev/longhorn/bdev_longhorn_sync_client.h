#ifndef SPDK_BDEV_LONGHORN_REBUILD_RPC__H
#define SPDK_BDEV_LONGHORN_REBUILD_RPC__H

int longhorn_sync_client(const char *addr, uint16_t port, uint64_t blob_id, struct spdk_lvol_store *lvs);

#endif /* SPDK_BDEV_LONGHORN_REBUILD_RPC__H */

