#ifndef SPDK_BDEV_LONGHORN_LVOL__H
#define SPDK_BDEV_LONGHORN_LVOL__H

struct longhorn_lvol_header {
	uint64_t num_clusters;
	uint64_t allocated_clusters;
	uint32_t cluster_size;
	uint32_t io_unit_size;
};

struct longhorn_transmit_context;

struct spdk_lvol_store *
longhorn_get_lvol_store_by_name(const char *name);

uint64_t longhorn_get_allocated_clusters(struct spdk_blob *blob);
void longhorn_export_allocated_clusters(struct spdk_blob *blob, uint32_t *table);

//struct longhorn_transmit_context *longhorn_transmit_context_create
void longhorn_lvol_transmit(int fd, uint64_t blob_id, struct spdk_blob_store *bs, bool *busy);
#endif /* SPDK_BDEV_LONGHORN_LVOL__H */

