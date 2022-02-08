#ifndef SPDK__BDEV_LONGHORN_SYNC_H
#define SPDK__BDEV_LONGHORN_SYNC_H
void longhorn_snapshot_bdev_sync(const char *snapshot_bdev_name,
                                 const char *name,
                                 struct spdk_lvol_store *lvs,
                                 uint64_t num_clusters,
                                 uint64_t allocated_clusters,
                                 uint32_t cluster_size,
                                 uint32_t io_unit_size,
                                 uint32_t *table);

#endif /* SPDK__BDEV_LONGHORN_SYNC_H */
