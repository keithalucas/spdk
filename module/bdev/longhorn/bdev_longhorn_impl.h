#ifndef SPDK__BDEV_LONGHORN_IMPL_H
#define SPDK__BDEV_LONGHORN_IMPL_H

int longhorn_start(struct longhorn_bdev *longhorn_bdev);

void
longhorn_submit_rw_request(struct longhorn_bdev_io *raid_io);

void
longhorn_submit_null_payload_request(struct longhorn_bdev_io *raid_io);


#endif /* SPDK__BDEV_LONGHORN_IMPL_H */
