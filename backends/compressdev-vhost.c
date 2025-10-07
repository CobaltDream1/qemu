#include "qemu/osdep.h"
#include "hw/virtio/virtio-bus.h"
#include "system/compressdev-vhost.h"

#ifdef CONFIG_VHOST_COMP
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/virtio/virtio-comp.h"
#include "system/compressdev-vhost-user.h"

uint64_t
compressdev_vhost_get_max_queues(
                        CompressDevBackendVhost *compress)
{
    return compress->dev.max_queues;
}

void compressdev_vhost_cleanup(CompressDevBackendVhost *compress)
{
    vhost_dev_cleanup(&compress->dev);
    g_free(compress);
}

struct CompressDevBackendVhost *
compressdev_vhost_init(
             CompressDevBackendVhostOptions *options)
{
    int r;
    CompressDevBackendVhost *compress;
    Error *local_err = NULL;

    compress = g_new0(CompressDevBackendVhost, 1);
    compress->dev.max_queues = 1;
    compress->dev.nvqs = 1;
    compress->dev.vqs = compress->vqs;

    compress->cc = options->cc;

    compress->dev.protocol_features = 0;
    compress->backend = -1;

    /* vhost-user needs vq_index to initiate a specific queue pair */
    compress->dev.vq_index = compress->cc->queue_index * compress->dev.nvqs;

    r = vhost_dev_init(&compress->dev, options->opaque, options->backend_type, 0,
                       &local_err);
    if (r < 0) {
        error_report_err(local_err);
        goto fail;
    }

    return compress;
fail:
    g_free(compress);
    return NULL;
}

static int
compressdev_vhost_start_one(CompressDevBackendVhost *compress,
                                  VirtIODevice *dev)
{
    int r;

    compress->dev.nvqs = 1;
    compress->dev.vqs = compress->vqs;

    r = vhost_dev_enable_notifiers(&compress->dev, dev);
    if (r < 0) {
        goto fail_notifiers;
    }

    r = vhost_dev_start(&compress->dev, dev, false);
    if (r < 0) {
        goto fail_start;
    }

    return 0;

fail_start:
    vhost_dev_disable_notifiers(&compress->dev, dev);
fail_notifiers:
    return r;
}

static void
compressdev_vhost_stop_one(CompressDevBackendVhost *compress,
                                 VirtIODevice *dev)
{
    vhost_dev_stop(&compress->dev, dev, false);
    vhost_dev_disable_notifiers(&compress->dev, dev);
}

CompressDevBackendVhost *
compressdev_get_vhost(CompressDevBackendClient *cc,
                            CompressDevBackend *b,
                            uint16_t queue)
{
    CompressDevBackendVhost *vhost_compress = NULL;

    if (!cc) {
        return NULL;
    }

    switch (cc->type) {
#if defined(CONFIG_VHOST_USER) && defined(CONFIG_LINUX)
    case QCOMPRESSDEV_BACKEND_TYPE_VHOST_USER:
        vhost_compress = compressdev_vhost_user_get_vhost(cc, b, queue);
        break;
#endif
    default:
        break;
    }

    return vhost_compress;
}

static void
compressdev_vhost_set_vq_index(CompressDevBackendVhost *compress,
                                     int vq_index)
{
    compress->dev.vq_index = vq_index;
}

static int
vhost_set_vring_enable(CompressDevBackendClient *cc,
                            CompressDevBackend *b,
                            uint16_t queue, int enable)
{
    CompressDevBackendVhost *compress =
                       compressdev_get_vhost(cc, b, queue);
    const VhostOps *vhost_ops;

    cc->vring_enable = enable;

    if (!compress) {
        return 0;
    }

    vhost_ops = compress->dev.vhost_ops;
    if (vhost_ops->vhost_set_vring_enable) {
        return vhost_ops->vhost_set_vring_enable(&compress->dev, enable);
    }

    return 0;
}

int compressdev_vhost_start(VirtIODevice *dev, int total_queues)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(dev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(dev)));
    VirtioBusState *vbus = VIRTIO_BUS(qbus);
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(vbus);
    int r, e;
    int i;
    CompressDevBackend *b = vcompress->compressdev;
    CompressDevBackendVhost *vhost_compress;
    CompressDevBackendClient *cc;

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return -ENOSYS;
    }

    for (i = 0; i < total_queues; i++) {
        cc = b->conf.peers.ccs[i];

        vhost_compress = compressdev_get_vhost(cc, b, i);
        compressdev_vhost_set_vq_index(vhost_compress, i);

        /* Suppress the masking guest notifiers on vhost user
         * because vhost user doesn't interrupt masking/unmasking
         * properly.
         */
        if (cc->type == QCOMPRESSDEV_BACKEND_TYPE_VHOST_USER) {
            dev->use_guest_notifier_mask = false;
        }
     }

    r = k->set_guest_notifiers(qbus->parent, total_queues, true);
    if (r < 0) {
        error_report("error binding guest notifier: %d", -r);
        goto err;
    }

    for (i = 0; i < total_queues; i++) {
        cc = b->conf.peers.ccs[i];

        vhost_compress = compressdev_get_vhost(cc, b, i);
        r = compressdev_vhost_start_one(vhost_compress, dev);

        if (r < 0) {
            goto err_start;
        }

        if (cc->vring_enable) {
            /* restore vring enable state */
            r = vhost_set_vring_enable(cc, b, i, cc->vring_enable);

            if (r < 0) {
                goto err_start;
            }
        }
    }

    return 0;

err_start:
    while (--i >= 0) {
        cc = b->conf.peers.ccs[i];
        vhost_compress = compressdev_get_vhost(cc, b, i);
        compressdev_vhost_stop_one(vhost_compress, dev);
    }
    e = k->set_guest_notifiers(qbus->parent, total_queues, false);
    if (e < 0) {
        error_report("vhost guest notifier cleanup failed: %d", e);
    }
err:
    return r;
}

void compressdev_vhost_stop(VirtIODevice *dev, int total_queues)
{
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(dev)));
    VirtioBusState *vbus = VIRTIO_BUS(qbus);
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(vbus);
    VirtIOCompress *vcompress = VIRTIO_COMP(dev);
    CompressDevBackend *b = vcompress->compressdev;
    CompressDevBackendVhost *vhost_compress;
    CompressDevBackendClient *cc;
    size_t i;
    int r;

    for (i = 0; i < total_queues; i++) {
        cc = b->conf.peers.ccs[i];

        vhost_compress = compressdev_get_vhost(cc, b, i);
        compressdev_vhost_stop_one(vhost_compress, dev);
    }

    r = k->set_guest_notifiers(qbus->parent, total_queues, false);
    if (r < 0) {
        error_report("vhost guest notifier cleanup failed: %d", r);
    }
    assert(r >= 0);
}

void compressdev_vhost_virtqueue_mask(VirtIODevice *dev,
                                           int queue,
                                           int idx, bool mask)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(dev);
    CompressDevBackend *b = vcompress->compressdev;
    CompressDevBackendVhost *vhost_compress;
    CompressDevBackendClient *cc;

    assert(queue < MAX_COMPRESS_QUEUE_NUM);

    cc = b->conf.peers.ccs[queue];
    vhost_compress = compressdev_get_vhost(cc, b, queue);

    vhost_virtqueue_mask(&vhost_compress->dev, dev, idx, mask);
}

bool compressdev_vhost_virtqueue_pending(VirtIODevice *dev,
                                              int queue, int idx)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(dev);
    CompressDevBackend *b = vcompress->compressdev;
    CompressDevBackendVhost *vhost_compress;
    CompressDevBackendClient *cc;

    assert(queue < MAX_COMPRESS_QUEUE_NUM);

    cc = b->conf.peers.ccs[queue];
    vhost_compress = compressdev_get_vhost(cc, b, queue);

    return vhost_virtqueue_pending(&vhost_compress->dev, idx);
}

#else
uint64_t
compressdev_vhost_get_max_queues(CompressDevBackendVhost *compress)
{
    return 0;
}

void compressdev_vhost_cleanup(CompressDevBackendVhost *compress)
{
}

struct CompressDevBackendVhost *
compressdev_vhost_init(CompressDevBackendVhostOptions *options)
{
    return NULL;
}

CompressDevBackendVhost *
compressdev_get_vhost(CompressDevBackendClient *cc,
                    CompressDevBackend *b,
                    uint16_t queue)
{
    return NULL;
}

int compressdev_vhost_start(VirtIODevice *dev, int total_queues)
{
    return -1;
}

void compressdev_vhost_stop(VirtIODevice *dev, int total_queues)
{
}

void compressdev_vhost_virtqueue_mask(VirtIODevice *dev,
                                    int queue,
                                    int idx, bool mask)
{
}

bool compressdev_vhost_virtqueue_pending(VirtIODevice *dev,
                                       int queue, int idx)
{
    return false;
}
#endif
