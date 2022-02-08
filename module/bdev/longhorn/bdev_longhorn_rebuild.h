#ifndef BDEV_LONGHORN_REBUILD_H
#define BDEV_LONGHORN_REBUILD_H

#include "lib/blob/blobstore.h"
#include "spdk/queue.h"

struct lvs_name {
	spdk_blob_id id;

};

struct longhorn_blob_info {
	char *name;
	uint64_t num_clusters;
	uint64_t allocated_clusters;
	uint32_t *table;
};

void longhorn_get_blob_info(struct spdk_blob_store *bs, uint64_t blob_id, void (*callback)(struct longhorn_blob_info *info, void *cb_arg), void *cb_arg);

int bdev_longhorn_lookup_name(const char *name, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);
int bdev_longhorn_import(const char *name, const char *lvs, const char *file);
int bdev_longhorn_link(const char *child, const char *parent);
void bdev_longhorn_get_children_remote(const char *address,
                                       uint16_t port, 
                                       const char *name);
void bdev_longhorn_rebuilt_remote(const char *address,
                                  uint16_t port, 
                                  const char *name,
				  char *remote_prefix,
				  struct spdk_lvol_store *lvs);
#endif /* BDEV_LONGHORN_REBUILD_H */
