/*
 * Virtio compress Support
 *
 * Copyright (c) 2016 HUAWEI TECHNOLOGIES CO., LTD.
 *
 * Authors:
 *    Gonglei <arei.gonglei@huawei.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#ifndef QEMU_VIRTIO_COMP_H
#define QEMU_VIRTIO_COMP_H

#include "standard-headers/linux/virtio_comp.h"
#include "hw/virtio/virtio.h"
#include "system/iothread.h"
#include "system/compressdev.h"
#include "qom/object.h"


#define DEBUG_VIRTIO_COMP 0

#define DPRINTF(fmt, ...) \
do { \
    if (DEBUG_VIRTIO_COMP) { \
        fprintf(stderr, "virtio_compress: " fmt, ##__VA_ARGS__); \
    } \
} while (0)


#define TYPE_VIRTIO_COMP "virtio-comp-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOCompress, VIRTIO_COMP)
#define VIRTIO_COMP_GET_PARENT_CLASS(obj) \
        OBJECT_GET_PARENT_CLASS(obj, TYPE_VIRTIO_COMP)


typedef struct VirtIOCompressConf {
    CompressDevBackend *compressdev;

    /* Supported service mask */
    uint32_t compress_services;

    /* Detailed algorithms mask */
    uint32_t comp_algo;
    uint32_t hash_algo;

    uint64_t max_size;
} VirtIOCompressConf;

struct VirtIOCompress;

typedef struct VirtIOCompressReq {
    VirtQueueElement elem;
    /* flags of operation, such as type of algorithm */
    uint32_t flags;
    struct virtio_comp_inhdr *in;
    struct iovec *in_iov; /* Head address of dest iovec */
    unsigned int in_num; /* Number of dest iovec */
    size_t in_len;
    VirtQueue *vq;
    struct VirtIOCompress *vcompress;
    CompressDevBackendOpInfo op_info;
} VirtIOCompressReq;

typedef struct VirtIOCompressQueue {
    VirtQueue *dataq;
    QEMUBH *dataq_bh;
    struct VirtIOCompress *vcompress;
} VirtIOCompressQueue;

struct VirtIOCompress {
    VirtIODevice parent_obj;

    VirtQueue *ctrl_vq;
    VirtIOCompressQueue *vqs;
    VirtIOCompressConf conf;
    CompressDevBackend *compressdev;

    uint32_t max_queues;
    uint32_t status;

    int multiqueue;
    uint32_t curr_queues;
    size_t config_size;
    uint8_t vhost_started;
};

#endif /* QEMU_VIRTIO_COMP_H */
