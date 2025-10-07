#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-comp.h"
#include "hw/qdev-properties.h"
#include "standard-headers/linux/virtio_ids.h"
#include "system/compressdev-vhost.h"

#define VIRTIO_COMP_VM_VERSION 1

typedef struct VirtIOCompressSessionReq {
    VirtIODevice *vdev;
    VirtQueue *vq;
    VirtQueueElement *elem;
    CompressDevBackendSessionInfo info;
    CompressDevCompletionFunc cb;
} VirtIOCompressSessionReq;

static void virtio_comp_free_create_session_req(VirtIOCompressSessionReq *sreq)
{
    switch (sreq->info.op_code) {
    case VIRTIO_COMP_STATEFUL_CREATE_SESSION:
    case VIRTIO_COMP_STATELESS_CREATE_SESSION:
    case VIRTIO_COMP_STATEFUL_DESTROY_SESSION:
    case VIRTIO_COMP_STATELESS_DESTROY_SESSION:
        break;

    default:
        error_report("Unknown opcode: %u", sreq->info.op_code);
    }
    g_free(sreq);
}

/*
 * Transfer virtqueue index to compress queue index.
 * The control virtqueue is after the data virtqueues
 * so the input value doesn't need to be adjusted
 */
static inline int virtio_comp_vq2q(int queue_index)
{
    return queue_index;
}

static int
virtio_comp_stateless_create_session(VirtIOCompress *vcompress,
               struct virtio_comp_stateless_create_session_req *sess_req,
               uint32_t queue_id,
               uint32_t opcode,
               struct iovec *iov, unsigned int out_num,
               VirtIOCompressSessionReq *sreq)
{
    // VirtIODevice *vdev = VIRTIO_DEVICE(vcompress);
    CompressDevBackendStatelessSessionInfo *stateless_info = &sreq->info.u.stateless_sess_info;
    int queue_index;

    sreq->info.op_code = opcode;
    stateless_info->comp_algo = ldl_le_p(&sess_req->req.para.algo);
    stateless_info->direction = ldl_le_p(&sess_req->req.para.op);

    switch (stateless_info->comp_algo) {
    case VIRTIO_COMP_ALGO_DEFLATE:
        stateless_info->u.deflate.huffman =
            ldl_le_p(&sess_req->req.para.u.deflate.huffman);
        break;

    /* TODO LZS&LZ4 handling */

    default:
        return -VIRTIO_COMP_ERR;
    }

    queue_index = virtio_comp_vq2q(queue_id);
    return compressdev_backend_create_session(vcompress->compressdev, &sreq->info,
                                            queue_index, sreq->cb, sreq);
}

static int
virtio_comp_stateful_create_session(VirtIOCompress *vcompress,
               struct virtio_comp_stateful_create_session_req *sess_req,
               uint32_t queue_id, uint32_t opcode,
               struct iovec *iov, unsigned int out_num,
               VirtIOCompressSessionReq *sreq)
{
    // VirtIODevice *vdev = VIRTIO_DEVICE(vcompress);
    CompressDevBackendStatefulSessionInfo *stateful_info = &sreq->info.u.stateful_sess_info;
    int queue_index;

    sreq->info.op_code = opcode;
    stateful_info->comp_algo = ldl_le_p(&sess_req->req.para.algo);
    stateful_info->direction = ldl_le_p(&sess_req->req.para.op);

    switch (stateful_info->comp_algo) {
    case VIRTIO_COMP_ALGO_DEFLATE:
        stateful_info->u.deflate.huffman =
            ldl_le_p(&sess_req->req.para.u.deflate.huffman);
        break;

    /* TODO LZS&LZ4 handling */

    default:
        return -VIRTIO_COMP_ERR;
    }

    queue_index = virtio_comp_vq2q(queue_id);
    return compressdev_backend_create_session(vcompress->compressdev, &sreq->info,
                                            queue_index, sreq->cb, sreq);
}

static int
virtio_comp_handle_close_session(VirtIOCompress *vcompress,
         struct virtio_comp_destroy_session_req *close_sess_req,
         uint32_t queue_id,
         VirtIOCompressSessionReq *sreq)
{
    uint64_t session_id;

    session_id = ldq_le_p(&close_sess_req->session_id);
    DPRINTF("close session, id=%" PRIu64 "\n", session_id);

    return compressdev_backend_close_session(
                vcompress->compressdev, session_id, queue_id, sreq->cb, sreq);
}

static void virtio_comp_create_session_completion(void *opaque, int ret)
{
    VirtIOCompressSessionReq *sreq = (VirtIOCompressSessionReq *)opaque;
    VirtQueue *vq = sreq->vq;
    VirtQueueElement *elem = sreq->elem;
    VirtIODevice *vdev = sreq->vdev;
    struct virtio_comp_session_input input;
    struct iovec *in_iov = elem->in_sg;
    unsigned in_num = elem->in_num;
    size_t s;

    memset(&input, 0, sizeof(input));
    /* Serious errors, need to reset virtio compress device */
    if (ret == -EFAULT) {
        virtqueue_detach_element(vq, elem, 0);
        goto out;
    } else if (ret == -VIRTIO_COMP_NOTSUPP) {
        stl_le_p(&input.status, VIRTIO_COMP_NOTSUPP);
    } else if (ret == -VIRTIO_COMP_KEY_REJECTED) {
        stl_le_p(&input.status, VIRTIO_COMP_KEY_REJECTED);
    } else if (ret != VIRTIO_COMP_OK) {
        stl_le_p(&input.status, VIRTIO_COMP_ERR);
    } else {
        /* Set the session id */
        stq_le_p(&input.session_id, sreq->info.session_id);
        stl_le_p(&input.status, VIRTIO_COMP_OK);
    }

    s = iov_from_buf(in_iov, in_num, 0, &input, sizeof(input));
    if (unlikely(s != sizeof(input))) {
        virtio_error(vdev, "virtio-comp input incorrect");
        virtqueue_detach_element(vq, elem, 0);
        goto out;
    }
    virtqueue_push(vq, elem, sizeof(input));
    virtio_notify(vdev, vq);

out:
    g_free(elem);
    virtio_comp_free_create_session_req(sreq);
}

static void virtio_comp_destroy_session_completion(void *opaque, int ret)
{
    VirtIOCompressSessionReq *sreq = (VirtIOCompressSessionReq *)opaque;
    VirtQueue *vq = sreq->vq;
    VirtQueueElement *elem = sreq->elem;
    VirtIODevice *vdev = sreq->vdev;
    struct iovec *in_iov = elem->in_sg;
    unsigned in_num = elem->in_num;
    uint8_t status;
    size_t s;

    if (ret < 0) {
        status = VIRTIO_COMP_ERR;
    } else {
        status = VIRTIO_COMP_OK;
    }
    s = iov_from_buf(in_iov, in_num, 0, &status, sizeof(status));
    if (unlikely(s != sizeof(status))) {
        virtio_error(vdev, "virtio-comp status incorrect");
        virtqueue_detach_element(vq, elem, 0);
        goto out;
    }
    virtqueue_push(vq, elem, sizeof(status));
    virtio_notify(vdev, vq);

out:
    g_free(elem);
    g_free(sreq);
}

static void virtio_comp_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);
    struct virtio_comp_op_ctrl_req ctrl;
    VirtQueueElement *elem;
    VirtIOCompressSessionReq *sreq;
    unsigned out_num;
    unsigned in_num;
    uint32_t queue_id;
    uint32_t opcode;
    struct virtio_comp_session_input input;
    size_t s;
    int ret;
    struct iovec *out_iov;
    struct iovec *in_iov;

    for (;;) {
        g_autofree struct iovec *out_iov_copy = NULL;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }
        if (elem->out_num < 1 || elem->in_num < 1) {
            virtio_error(vdev, "virtio-comp ctrl missing headers");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            break;
        }

        out_num = elem->out_num;
        out_iov_copy = g_memdup2(elem->out_sg, sizeof(out_iov[0]) * out_num);
        out_iov = out_iov_copy;

        in_num = elem->in_num;
        in_iov = elem->in_sg;

        if (unlikely(iov_to_buf(out_iov, out_num, 0, &ctrl, sizeof(ctrl))
                    != sizeof(ctrl))) {
            virtio_error(vdev, "virtio-comp request ctrl_hdr too short");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            break;
        }
        iov_discard_front(&out_iov, &out_num, sizeof(ctrl));

        opcode = ldl_le_p(&ctrl.header.opcode);
        queue_id = ldl_le_p(&ctrl.header.queue_id);

        sreq = g_new0(VirtIOCompressSessionReq, 1);
        sreq->vdev = vdev;
        sreq->vq = vq;
        sreq->elem = elem;

        switch (opcode) {
        case VIRTIO_COMP_STATEFUL_CREATE_SESSION:
            sreq->cb = virtio_comp_create_session_completion;
            ret = virtio_comp_stateful_create_session(vcompress,
                            &ctrl.u.stateful_create_session,
                            queue_id, opcode,
                            out_iov, out_num,
                            sreq);
            if (ret < 0) {
                virtio_comp_create_session_completion(sreq, ret);
            }
            break;

        case VIRTIO_COMP_STATELESS_CREATE_SESSION:
            sreq->cb = virtio_comp_create_session_completion;
            ret = virtio_comp_stateless_create_session(vcompress,
                             &ctrl.u.stateless_create_session,
                             queue_id, opcode,
                             out_iov, out_num,
                             sreq);
            if (ret < 0) {
                virtio_comp_create_session_completion(sreq, ret);
            }
            break;

        case VIRTIO_COMP_STATEFUL_DESTROY_SESSION:
        case VIRTIO_COMP_STATELESS_DESTROY_SESSION:
            sreq->cb = virtio_comp_destroy_session_completion;
            ret = virtio_comp_handle_close_session(vcompress,
                   &ctrl.u.destroy_session, queue_id,
                   sreq);
            if (ret < 0) {
                virtio_comp_destroy_session_completion(sreq, ret);
            }
            break;

        default:
            memset(&input, 0, sizeof(input));
            error_report("virtio-comp unsupported ctrl opcode: %d", opcode);
            stl_le_p(&input.status, VIRTIO_COMP_NOTSUPP);
            s = iov_from_buf(in_iov, in_num, 0, &input, sizeof(input));
            if (unlikely(s != sizeof(input))) {
                virtio_error(vdev, "virtio-comp input incorrect");
                virtqueue_detach_element(vq, elem, 0);
            } else {
                virtqueue_push(vq, elem, sizeof(input));
                virtio_notify(vdev, vq);
            }
            g_free(sreq);
            g_free(elem);

            break;
        } /* end switch case */

    } /* end for loop */
}

static void virtio_comp_init_request(VirtIOCompress *vcompress, VirtQueue *vq,
                                VirtIOCompressReq *req)
{
    req->vcompress = vcompress;
    req->vq = vq;
    req->in = NULL;
    req->in_iov = NULL;
    req->in_num = 0;
    req->in_len = 0;
    req->flags = QCOMPRESSDEV_BACKEND_SERVICE_TYPE__MAX;
    memset(&req->op_info, 0x00, sizeof(req->op_info));
}

static void virtio_comp_free_request(VirtIOCompressReq *req)
{
    if (!req) {
        return;
    }

    if (req->flags == QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATEFUL) {
        size_t max_len;
        CompressDevBackendStatefulOpInfo *op_info = req->op_info.u.stateful_op_info;

        if (op_info) {
            max_len = op_info->src_len + op_info->dst_len;
                    //   + op_info->digest_result_len;

            /* Zeroize and free request data structure */
            memset(op_info, 0, sizeof(*op_info) + max_len);
            g_free(op_info);
        }
    } else if (req->flags == QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATELESS) {
        CompressDevBackendStatelessOpInfo *op_info = req->op_info.u.stateless_op_info;
        if (op_info) {
            g_free(op_info->src);
            g_free(op_info->dst);
            memset(op_info, 0, sizeof(*op_info));
            g_free(op_info);
        }
    }

    g_free(req->in_iov);
    g_free(req);
}

static void
virtio_comp_stateful_input_data_helper(VirtIODevice *vdev,
                VirtIOCompressReq *req,
                uint32_t status,
                CompressDevBackendStatefulOpInfo *stateful_op_info)
{
    size_t s, len;
    struct iovec *in_iov = req->in_iov;

    if (status != VIRTIO_COMP_OK) {
        return;
    }

    len = stateful_op_info->src_len;
    /* Save the cipher result */
    s = iov_from_buf(in_iov, req->in_num, 0, stateful_op_info->dst, len);
    if (s != len) {
        virtio_error(vdev, "virtio-comp dest data incorrect");
        return;
    }

    iov_discard_front(&in_iov, &req->in_num, len);
}

static void
virtio_comp_stateless_input_data_helper(VirtIODevice *vdev,
        VirtIOCompressReq *req, int32_t status,
        CompressDevBackendStatelessOpInfo *stateless_op_info)
{
    size_t s, len;
    struct iovec *in_iov = req->in_iov;

    if (status != VIRTIO_COMP_OK) {
        return;
    }

    len = stateless_op_info->dst_len;
    if (!len) {
        return;
    }

    s = iov_from_buf(in_iov, req->in_num, 0, stateless_op_info->dst, len);
    if (s != len) {
        virtio_error(vdev, "virtio-comp asym dest data incorrect");
        return;
    }

    iov_discard_front(&in_iov, &req->in_num, len);

    /* For akcipher, dst_len may be changed after operation */
    req->in_len = sizeof(struct virtio_comp_inhdr) + stateless_op_info->dst_len;
}

static void virtio_comp_req_complete(void *opaque, int ret)
{
    VirtIOCompressReq *req = (VirtIOCompressReq *)opaque;
    VirtIOCompress *vcompress = req->vcompress;
    VirtIODevice *vdev = VIRTIO_DEVICE(vcompress);
    uint8_t status = -ret;

    if (req->flags == QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATEFUL) {
        virtio_comp_stateful_input_data_helper(vdev, req, status,
                                            req->op_info.u.stateful_op_info);
    } else if (req->flags == QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATELESS) {
        virtio_comp_stateless_input_data_helper(vdev, req, status,
                                             req->op_info.u.stateless_op_info);
    }
    stb_p(&req->in->status, status);
    virtqueue_push(req->vq, &req->elem, req->in_len);
    virtio_notify(vdev, req->vq);
    virtio_comp_free_request(req);
}

static VirtIOCompressReq *
virtio_comp_get_request(VirtIOCompress *s, VirtQueue *vq)
{
    VirtIOCompressReq *req = virtqueue_pop(vq, sizeof(VirtIOCompressReq));

    if (req) {
        virtio_comp_init_request(s, vq, req);
    }
    return req;
}

static int
virtio_comp_handle_stateless_req(VirtIOCompress *vcompress,
               struct virtio_comp_stateless_data_req *req,
               CompressDevBackendOpInfo *op_info,
               struct iovec *iov, unsigned int out_num)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vcompress);
    CompressDevBackendStatelessOpInfo *stateless_op_info;
    uint32_t src_len;
    uint32_t dst_len;
    uint32_t len;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;

    stateless_op_info = g_new0(CompressDevBackendStatelessOpInfo, 1);
    src_len = ldl_le_p(&req->para.src_data_len);
    dst_len = ldl_le_p(&req->para.dst_data_len);

    if (src_len > 0) {
        src = g_malloc0(src_len);
        len = iov_to_buf(iov, out_num, 0, src, src_len);
        if (unlikely(len != src_len)) {
            virtio_error(vdev, "virtio-comp asym src data incorrect"
                         "expected %u, actual %u", src_len, len);
            goto err;
        }

        iov_discard_front(&iov, &out_num, src_len);
    }

    if (dst_len > 0) {
        dst = g_malloc0(dst_len);
    }

    stateless_op_info->src_len = src_len;
    stateless_op_info->dst_len = dst_len;
    stateless_op_info->src = src;
    stateless_op_info->dst = dst;
    op_info->u.stateless_op_info = stateless_op_info;

    return 0;

 err:
    g_free(stateless_op_info);
    g_free(src);
    g_free(dst);

    return -EFAULT;
}

static int
virtio_comp_handle_stateful_req(VirtIOCompress *vcompress,
               struct virtio_comp_stateful_data_req *req,
               CompressDevBackendOpInfo *op_info,
               struct iovec *iov, unsigned int out_num)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vcompress);
    CompressDevBackendStatefulOpInfo *stateful_op_info;
    uint32_t src_len;
    uint32_t dst_len;
    uint32_t len;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;

    stateful_op_info = g_new0(CompressDevBackendStatefulOpInfo, 1);
    src_len = ldl_le_p(&req->para.src_data_len);
    dst_len = ldl_le_p(&req->para.dst_data_len);

    if (src_len > 0) {
        src = g_malloc0(src_len);
        len = iov_to_buf(iov, out_num, 0, src, src_len);
        if (unlikely(len != src_len)) {
            virtio_error(vdev, "virtio-comp asym src data incorrect"
                         "expected %u, actual %u", src_len, len);
            goto err;
        }

        iov_discard_front(&iov, &out_num, src_len);
    }

    if (dst_len > 0) {
        dst = g_malloc0(dst_len);
    }

    stateful_op_info->src_len = src_len;
    stateful_op_info->dst_len = dst_len;
    stateful_op_info->src = src;
    stateful_op_info->dst = dst;
    op_info->u.stateful_op_info = stateful_op_info;

    return 0;

 err:
    g_free(stateful_op_info);
    g_free(src);
    g_free(dst);

    return -EFAULT;
}

static int
virtio_comp_handle_request(VirtIOCompressReq *request)
{
    VirtIOCompress *vcompress = request->vcompress;
    VirtIODevice *vdev = VIRTIO_DEVICE(vcompress);
    VirtQueueElement *elem = &request->elem;
    int queue_index = virtio_comp_vq2q(virtio_get_queue_index(request->vq));
    struct virtio_comp_op_data_req req;
    int ret;
    g_autofree struct iovec *in_iov_copy = NULL;
    g_autofree struct iovec *out_iov_copy = NULL;
    struct iovec *in_iov;
    struct iovec *out_iov;
    unsigned in_num;
    unsigned out_num;
    uint32_t opcode;
    CompressDevBackendOpInfo *op_info = &request->op_info;

    if (elem->out_num < 1 || elem->in_num < 1) {
        virtio_error(vdev, "virtio-comp dataq missing headers");
        return -1;
    }

    out_num = elem->out_num;
    out_iov_copy = g_memdup2(elem->out_sg, sizeof(out_iov[0]) * out_num);
    out_iov = out_iov_copy;

    in_num = elem->in_num;
    in_iov_copy = g_memdup2(elem->in_sg, sizeof(in_iov[0]) * in_num);
    in_iov = in_iov_copy;

    if (unlikely(iov_to_buf(out_iov, out_num, 0, &req, sizeof(req))
                != sizeof(req))) {
        virtio_error(vdev, "virtio-comp request outhdr too short");
        return -1;
    }
    iov_discard_front(&out_iov, &out_num, sizeof(req));

    if (in_iov[in_num - 1].iov_len <
            sizeof(struct virtio_comp_inhdr)) {
        virtio_error(vdev, "virtio-comp request inhdr too short");
        return -1;
    }
    /* We always touch the last byte, so just see how big in_iov is. */
    request->in_len = iov_size(in_iov, in_num);
    request->in = (void *)in_iov[in_num - 1].iov_base
              + in_iov[in_num - 1].iov_len
              - sizeof(struct virtio_comp_inhdr);
    iov_discard_back(in_iov, &in_num, sizeof(struct virtio_comp_inhdr));

    /*
     * The length of operation result, including dest_data
     * and digest_result if exists.
     */
    request->in_num = in_num;
    request->in_iov = in_iov;
    /* now, we free the in_iov_copy inside virtio_comp_free_request */
    in_iov_copy = NULL;

    opcode = ldl_le_p(&req.header.opcode);
    op_info->session_id = ldq_le_p(&req.header.session_id);
    op_info->op_code = opcode;
    op_info->queue_index = queue_index;
    op_info->cb = virtio_comp_req_complete;
    op_info->opaque = request;

    switch (opcode) {
    case VIRTIO_COMP_STATEFUL_COMPRESS:
    case VIRTIO_COMP_STATEFUL_DECOMPRESS:
        op_info->algtype = request->flags = QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATEFUL;
        ret = virtio_comp_handle_stateful_req(vcompress,
                         &req.u.stateful_req, op_info,
                         out_iov, out_num);
        goto check_result;

    case VIRTIO_COMP_STATELESS_COMPRESS:
    case VIRTIO_COMP_STATELESS_DECOMPRESS:
        op_info->algtype = request->flags = QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATELESS;
        ret = virtio_comp_handle_stateless_req(vcompress,
                         &req.u.stateless_req, op_info,
                         out_iov, out_num);

check_result:
        /* Serious errors, need to reset virtio compress device */
        if (ret == -EFAULT) {
            return -1;
        } else if (ret == -VIRTIO_COMP_NOTSUPP) {
            virtio_comp_req_complete(request, -VIRTIO_COMP_NOTSUPP);
        } else {
            ret = compressdev_backend_compress_operation(vcompress->compressdev,
                                    op_info);
            if (ret < 0) {
                virtio_comp_req_complete(request, ret);
            }
        }
        break;

    default:
        error_report("virtio-comp unsupported dataq opcode: %u",
                     opcode);
        virtio_comp_req_complete(request, -VIRTIO_COMP_NOTSUPP);
    }

    return 0;
}

static void virtio_comp_handle_dataq(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);
    VirtIOCompressReq *req;

    while ((req = virtio_comp_get_request(vcompress, vq))) {
        if (virtio_comp_handle_request(req) < 0) {
            virtqueue_detach_element(req->vq, &req->elem, 0);
            virtio_comp_free_request(req);
            break;
        }
    }
}

static void virtio_comp_dataq_bh(void *opaque)
{
    VirtIOCompressQueue *q = opaque;
    VirtIOCompress *vcompress = q->vcompress;
    VirtIODevice *vdev = VIRTIO_DEVICE(vcompress);

    /* This happens when device was stopped but BH wasn't. */
    if (!vdev->vm_running) {
        return;
    }

    /* Just in case the driver is not ready on more */
    if (unlikely(!(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK))) {
        return;
    }

    for (;;) {
        virtio_comp_handle_dataq(vdev, q->dataq);
        virtio_queue_set_notification(q->dataq, 1);

        /* Are we done or did the guest add more buffers? */
        if (virtio_queue_empty(q->dataq)) {
            break;
        }

        virtio_queue_set_notification(q->dataq, 0);
    }
}

static void
virtio_comp_handle_dataq_bh(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);
    VirtIOCompressQueue *q =
         &vcompress->vqs[virtio_comp_vq2q(virtio_get_queue_index(vq))];

    /* This happens when device was stopped but VCPU wasn't. */
    if (!vdev->vm_running) {
        return;
    }
    virtio_queue_set_notification(vq, 0);
    qemu_bh_schedule(q->dataq_bh);
}

static uint64_t virtio_comp_get_features(VirtIODevice *vdev,
                                           uint64_t features,
                                           Error **errp)
{
    return features;
}

static void virtio_comp_reset(VirtIODevice *vdev)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);
    /* multiqueue is disabled by default */
    vcompress->curr_queues = 1;
    if (!compressdev_backend_is_ready(vcompress->compressdev)) {
        vcompress->status &= ~VIRTIO_COMP_S_HW_READY;
    } else {
        vcompress->status |= VIRTIO_COMP_S_HW_READY;
    }
}

static uint32_t virtio_comp_init_services(uint32_t qservices)
{
    uint32_t vservices = 0;

    if (qservices & (1 << QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATEFUL)) {
        vservices |= (1 << VIRTIO_COMP_SERVICE_STATEFUL);
    }
    if (qservices & (1 << QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATELESS)) {
        vservices |= (1 << VIRTIO_COMP_SERVICE_STATELESS);
    }

    return vservices;
}

static void virtio_comp_init_config(VirtIODevice *vdev)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);

    vcompress->conf.compress_services = virtio_comp_init_services(
                     vcompress->conf.compressdev->conf.compress_services);
    vcompress->conf.comp_algo =
                     vcompress->conf.compressdev->conf.comp_algo;
    vcompress->conf.hash_algo = vcompress->conf.compressdev->conf.hash_algo;
    vcompress->conf.max_size = vcompress->conf.compressdev->conf.max_size;
}

static void virtio_comp_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOCompress *vcompress = VIRTIO_COMP(dev);
    int i;

    vcompress->compressdev = vcompress->conf.compressdev;
    if (vcompress->compressdev == NULL) {
        error_setg(errp, "'compressdev' parameter expects a valid object");
        return;
    } else if (compressdev_backend_is_used(vcompress->compressdev)) {
        error_setg(errp, "can't use already used compressdev backend: %s",
                   object_get_canonical_path_component(OBJECT(vcompress->conf.compressdev)));
        return;
    }

    vcompress->max_queues = MAX(vcompress->compressdev->conf.peers.queues, 1);
    if (vcompress->max_queues + 1 > VIRTIO_QUEUE_MAX) {
        error_setg(errp, "Invalid number of queues (= %" PRIu32 "), "
                   "must be a positive integer less than %d.",
                   vcompress->max_queues, VIRTIO_QUEUE_MAX);
        return;
    }

    virtio_init(vdev, VIRTIO_ID_COMP, vcompress->config_size);
    vcompress->curr_queues = 1;
    vcompress->vqs = g_new0(VirtIOCompressQueue, vcompress->max_queues);
    for (i = 0; i < vcompress->max_queues; i++) {
        vcompress->vqs[i].dataq =
                 virtio_add_queue(vdev, 1024, virtio_comp_handle_dataq_bh);
        vcompress->vqs[i].dataq_bh =
                 virtio_bh_new_guarded(dev, virtio_comp_dataq_bh,
                                       &vcompress->vqs[i]);
        vcompress->vqs[i].vcompress = vcompress;
    }

    vcompress->ctrl_vq = virtio_add_queue(vdev, 1024, virtio_comp_handle_ctrl);
    if (!compressdev_backend_is_ready(vcompress->compressdev)) {
        vcompress->status &= ~VIRTIO_COMP_S_HW_READY;
    } else {
        vcompress->status |= VIRTIO_COMP_S_HW_READY;
    }

    virtio_comp_init_config(vdev);
    compressdev_backend_set_used(vcompress->compressdev, true);
}

static void virtio_comp_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOCompress *vcompress = VIRTIO_COMP(dev);
    VirtIOCompressQueue *q;
    int i, max_queues;

    max_queues = vcompress->multiqueue ? vcompress->max_queues : 1;
    for (i = 0; i < max_queues; i++) {
        virtio_delete_queue(vcompress->vqs[i].dataq);
        q = &vcompress->vqs[i];
        qemu_bh_delete(q->dataq_bh);
    }

    g_free(vcompress->vqs);
    virtio_delete_queue(vcompress->ctrl_vq);

    virtio_cleanup(vdev);
    compressdev_backend_set_used(vcompress->compressdev, false);
}

static const VMStateDescription vmstate_virtio_compress = {
    .name = "virtio-comp",
    .unmigratable = 1,
    .minimum_version_id = VIRTIO_COMP_VM_VERSION,
    .version_id = VIRTIO_COMP_VM_VERSION,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static const Property virtio_comp_properties[] = {
    DEFINE_PROP_LINK("compressdev", VirtIOCompress, conf.compressdev,
                     TYPE_COMPRESSDEV_BACKEND, CompressDevBackend *),
};

static void virtio_comp_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOCompress *c = VIRTIO_COMP(vdev);
    struct virtio_comp_config compress_cfg = {};

    /*
     * Virtio-compress device conforms to VIRTIO 1.0 which is always LE,
     * so we can use LE accessors directly.
     */
    stl_le_p(&compress_cfg.status, c->status);
    stl_le_p(&compress_cfg.max_dataqueues, c->max_queues);
    stl_le_p(&compress_cfg.compress_services, c->conf.compress_services);
    stl_le_p(&compress_cfg.comp_algo, c->conf.comp_algo);
    stl_le_p(&compress_cfg.hash_algo, c->conf.hash_algo);
    stq_le_p(&compress_cfg.max_size, c->conf.max_size);

    memcpy(config, &compress_cfg, c->config_size);
}

static bool virtio_comp_started(VirtIOCompress *c, uint8_t status)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(c);
    return (status & VIRTIO_CONFIG_S_DRIVER_OK) &&
        (c->status & VIRTIO_COMP_S_HW_READY) && vdev->vm_running;
}

static void virtio_comp_vhost_status(VirtIOCompress *c, uint8_t status)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(c);
    int queues = c->multiqueue ? c->max_queues : 1;
    CompressDevBackend *b = c->compressdev;
    CompressDevBackendClient *cc = b->conf.peers.ccs[0];

    if (!compressdev_get_vhost(cc, b, 0)) {
        return;
    }

    if ((virtio_comp_started(c, status)) == !!c->vhost_started) {
        return;
    }

    if (!c->vhost_started) {
        int r;

        c->vhost_started = 1;
        r = compressdev_vhost_start(vdev, queues);
        if (r < 0) {
            error_report("unable to start vhost compress: %d: "
                         "falling back on userspace virtio", -r);
            c->vhost_started = 0;
        }
    } else {
        compressdev_vhost_stop(vdev, queues);
        c->vhost_started = 0;
    }
}

static int virtio_comp_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);

    virtio_comp_vhost_status(vcompress, status);
    return 0;
}

static void virtio_comp_guest_notifier_mask(VirtIODevice *vdev, int idx,
                                           bool mask)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);
    int queue = virtio_comp_vq2q(idx);

    assert(vcompress->vhost_started);

    /*
     * Add the check for configure interrupt, Use VIRTIO_CONFIG_IRQ_IDX -1
     * as the macro of configure interrupt's IDX, If this driver does not
     * support, the function will return
     */

    if (idx == VIRTIO_CONFIG_IRQ_IDX) {
        return;
    }
    compressdev_vhost_virtqueue_mask(vdev, queue, idx, mask);
}

static bool virtio_comp_guest_notifier_pending(VirtIODevice *vdev, int idx)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);
    int queue = virtio_comp_vq2q(idx);

    assert(vcompress->vhost_started);

    /*
     * Add the check for configure interrupt, Use VIRTIO_CONFIG_IRQ_IDX -1
     * as the macro of configure interrupt's IDX, If this driver does not
     * support, the function will return
     */

    if (idx == VIRTIO_CONFIG_IRQ_IDX) {
        return false;
    }
    return compressdev_vhost_virtqueue_pending(vdev, queue, idx);
}

static struct vhost_dev *virtio_comp_get_vhost(VirtIODevice *vdev)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(vdev);
    CompressDevBackend *b;
    CompressDevBackendClient *cc;
    CompressDevBackendVhost *vhost_compress;

    b = vcompress->compressdev;
    if (!b) {
        return NULL;
    }

    cc = b->conf.peers.ccs[0];
    vhost_compress = compressdev_get_vhost(cc, b, 0);
    if (!vhost_compress) {
        return NULL;
    }

    return &vhost_compress->dev;
}

static void virtio_comp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_comp_properties);
    dc->vmsd = &vmstate_virtio_compress;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_comp_device_realize;
    vdc->unrealize = virtio_comp_device_unrealize;
    vdc->get_config = virtio_comp_get_config;
    vdc->get_features = virtio_comp_get_features;
    vdc->reset = virtio_comp_reset;
    vdc->set_status = virtio_comp_set_status;
    vdc->guest_notifier_mask = virtio_comp_guest_notifier_mask;
    vdc->guest_notifier_pending = virtio_comp_guest_notifier_pending;
    vdc->get_vhost = virtio_comp_get_vhost;
}

static void virtio_comp_instance_init(Object *obj)
{
    VirtIOCompress *vcompress = VIRTIO_COMP(obj);

    /*
     * The default config_size is sizeof(struct virtio_comp_config).
     * Can be overridden with virtio_comp_set_config_size.
     */
    vcompress->config_size = sizeof(struct virtio_comp_config);
}

static const TypeInfo virtio_comp_info = {
    .name = TYPE_VIRTIO_COMP,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOCompress),
    .instance_init = virtio_comp_instance_init,
    .class_init = virtio_comp_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_comp_info);
}

type_init(virtio_register_types)
