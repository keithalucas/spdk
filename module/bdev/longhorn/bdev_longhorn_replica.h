#ifndef SPDK__BDEV_LONGHORN_REPLICA_H
#define SPDK__BDEV_LONGHORN_REPLICA_H

struct bdev_longhorn_replica {
};

struct bdev_longhorn_replica_info {
	char *name;
	uint64_t id;
};

typedef (*longhorn_replica_create_cb) (struct spdk_lvol_store *lvs, const char *name, const char *nqn, void *arg);

void bdev_longhorn_replica_create(struct spdk_lvol_store *lvs,
                                  const char *name,
                                  uint64_t size,
                                  const char *addr,
                                  uint16_t port,
                                  longhorn_replica_create_cb cb_fn,
                                  void *cb_arg);


#endif /* SPDK__BDEV_LONGHORN_REPLICA_H */
