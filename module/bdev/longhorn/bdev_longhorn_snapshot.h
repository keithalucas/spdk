#ifndef _BDEV_LONGHORN_SNAPSHOT__H_
#define _BDEV_LONGHORN_SNAPSHOT__H_
#include "spdk/lvol.h"


void bdev_longhorn_snapshot_remote(const char *addr,
                                   const char *name,
                                   const char *lvs,
                                   const char *snapshot,
                                   spdk_lvol_op_complete cb_fn,
                                   void *cb_arg);

struct block {
	uint64_t block;
	TAILQ_ENTRY(block) next;
};

struct block_diff {
	TAILQ_HEAD(, block) blocks;

	uint64_t blocksize;
	
	uint64_t num_diff;
};


typedef void (*compare_bdev_cb)(int status, struct block_diff *diff, void *arg);

void bdev_longhorn_compare(const char *bdev_name1,
                           const char *bdev_name2,
                           uint64_t blocksize,
                           compare_bdev_cb cb_fn,
                           void *cb_arg);
int
longhorn_volume_snapshot(const char *name, const char *snapshot_name);

#endif /* _BDEV_LONGHORN_SNAPSHOT__H_ */
