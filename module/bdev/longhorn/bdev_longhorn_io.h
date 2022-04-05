#ifndef _BDEV_LONGHORN_IO__H
#define _BDEV_LONGHORN_IO__H

void
longhorn_pause_queue_playback(struct longhorn_bdev_io_channel *longhorn_ch);

void
longhorn_bdev_submit_request(struct spdk_io_channel *ch, 
			     struct spdk_bdev_io *bdev_io);
int
longhorn_start(struct longhorn_bdev *bdev);



#endif /* _BDEV_LONGHORN_IO__H */
