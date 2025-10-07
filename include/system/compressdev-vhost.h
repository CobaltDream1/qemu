#ifndef COMPRESSDEV_VHOST_H
#define COMPRESSDEV_VHOST_H

#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-backend.h"
#include "chardev/char.h"

#include "system/compressdev.h"


typedef struct CompressDevBackendVhostOptions {
    VhostBackendType backend_type;
    void *opaque;
    int total_queues;
    CompressDevBackendClient *cc;
} CompressDevBackendVhostOptions;

typedef struct CompressDevBackendVhost {
    struct vhost_dev dev;
    struct vhost_virtqueue vqs[1];
    int backend;
    CompressDevBackendClient *cc;
} CompressDevBackendVhost;

/**
 * compressdev_vhost_get_max_queues:
 * @compress: the compressdev backend common vhost object
 *
 * Get the maximum queue number of @compress.
 *
 *
 * Returns: the maximum queue number
 */
uint64_t
compressdev_vhost_get_max_queues(
                        CompressDevBackendVhost *compress);


/**
 * compressdev_vhost_init:
 * @options: the common vhost object's option
 *
 * Creates a new compressdev backend common vhost object
 *
 ** The returned object must be released with
 * compressdev_vhost_cleanup() when no
 * longer required
 *
 * Returns: the compressdev backend common vhost object
 */
struct CompressDevBackendVhost *
compressdev_vhost_init(
             CompressDevBackendVhostOptions *options);

/**
 * compressdev_vhost_cleanup:
 * @compress: the compressdev backend common vhost object
 *
 * Clean the resource associated with @compress that realizaed
 * by compressdev_vhost_init()
 *
 */
void compressdev_vhost_cleanup(
                        CompressDevBackendVhost *compress);

/**
 * compressdev_get_vhost:
 * @cc: the client object for each queue
 * @b: the compressdev backend common vhost object
 * @queue: the compressdev backend queue index
 *
 * Gets a new compressdev backend common vhost object based on
 * @b and @queue
 *
 * Returns: the compressdev backend common vhost object
 */
CompressDevBackendVhost *
compressdev_get_vhost(CompressDevBackendClient *cc,
                            CompressDevBackend *b,
                            uint16_t queue);
/**
 * compressdev_vhost_start:
 * @dev: the virtio compress object
 * @total_queues: the total count of queue
 *
 * Starts the vhost compress logic
 *
 * Returns: 0 for success, negative for errors
 */
int compressdev_vhost_start(VirtIODevice *dev, int total_queues);

/**
 * compressdev_vhost_stop:
 * @dev: the virtio compress object
 * @total_queues: the total count of queue
 *
 * Stops the vhost compress logic
 *
 */
void compressdev_vhost_stop(VirtIODevice *dev, int total_queues);

/**
 * compressdev_vhost_virtqueue_mask:
 * @dev: the virtio compress object
 * @queue: the compressdev backend queue index
 * @idx: the virtqueue index
 * @mask: mask or not (true or false)
 *
 * Mask/unmask events for @idx virtqueue on @dev device
 *
 */
void compressdev_vhost_virtqueue_mask(VirtIODevice *dev,
                                           int queue,
                                           int idx, bool mask);

/**
 * compressdev_vhost_virtqueue_pending:
 * @dev: the virtio compress object
 * @queue: the compressdev backend queue index
 * @idx: the virtqueue index
 *
 * Test and clear event pending status for @idx virtqueue on @dev device.
 * Should be called after unmask to avoid losing events.
 *
 * Returns: true for success, false for errors
 */
bool compressdev_vhost_virtqueue_pending(VirtIODevice *dev,
                                              int queue, int idx);

#endif /* COMPRESSDEV_VHOST_H */
