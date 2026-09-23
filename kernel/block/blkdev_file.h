#ifndef NEOOS_BLKDEV_FILE_H
#define NEOOS_BLKDEV_FILE_H

#include "fs/file.h"

// file_ops for an opened /dev/<block device>; f->priv is the blockdev.
extern const struct file_ops blkdev_file_ops;

#endif
