/*
 * QEMU Cryptodev backend for QEMU cipher APIs
 *
 * Copyright (c) 2016 HUAWEI TECHNOLOGIES CO., LTD.
 *
 * Authors:
 *    Gonglei <arei.gonglei@huawei.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "hw/virtio/vhost-user.h"
#include "standard-headers/linux/virtio_crypto.h"
#include "system/cryptodev-vhost.h"
#include "chardev/char-fe.h"
#include "system/cryptodev-vhost-user.h"
#include "qom/object.h"


#ifndef VHOST_USER_VERSION
# define VHOST_USER_VERSION 0x1
#endif
#ifndef VHOST_USER_NEED_REPLY_MASK
# define VHOST_USER_NEED_REPLY_MASK (1u << 3)
#endif

#define VHOST_USER_CRYPTO_FREEZE      50
#define VHOST_USER_CRYPTO_SAVE_STATE  51
#define VHOST_USER_CRYPTO_LOAD_STATE  52
#define VHOST_USER_CRYPTO_THAW        53
/**
 * @TYPE_CRYPTODEV_BACKEND_VHOST_USER:
 * name of backend that uses vhost user server
 */
#define TYPE_CRYPTODEV_BACKEND_VHOST_USER "cryptodev-vhost-user"

OBJECT_DECLARE_SIMPLE_TYPE(CryptoDevBackendVhostUser, CRYPTODEV_BACKEND_VHOST_USER)


struct CryptoDevBackendVhostUser {
    CryptoDevBackend parent_obj;

    VhostUserState vhost_user;
    CharBackend chr;
    char *chr_name;
    bool opened;
    CryptoDevBackendVhost *vhost_crypto[MAX_CRYPTO_QUEUE_NUM];

        /* --- migration cache --- */
    uint8_t  *mig_blob;
    uint32_t  mig_blob_len;
    uint64_t  mig_epoch;   /* 可选：从 TLV 解析出的世代/计数 */
};

#ifndef VC_SNAP_MAGIC
/* ---- Minimal snapshot header (frontend-local) ---- */
typedef struct QEMU_PACKED VC_SnapHdr {
    uint32_t magic;       /* 'VCRY' -> 0x59524356 LE */
    uint16_t version;     /* = 1 */
    uint16_t hdr_len;     /* bytes, including this header */
    uint32_t payload_len; /* TLV region length */
    uint32_t crc32;       /* CRC over TLV region (optional verify) */
} VC_SnapHdr;

#define VC_SNAP_MAGIC 0x59524356u
#endif

typedef struct QEMU_PACKED VuHdr {
    uint32_t request;
    uint32_t flags;
    uint32_t size;
} VuHdr;


/* 无 payload / 携带 1 个 FD */
static int vuc_send_with_fd_chr(CharBackend *chr, uint32_t req, int fd)
{
    VuHdr hdr = {
        .request = req,
        .flags   = VHOST_USER_VERSION | VHOST_USER_NEED_REPLY_MASK,
        .size    = 0,
    };
    int fds[1] = { fd };
    qemu_chr_fe_set_msgfds(chr, fds, 1);                         // 把 FD 附在下一次 write
    int ret = qemu_chr_fe_write_all(chr, (const uint8_t *)&hdr, sizeof(hdr));
    return (ret == sizeof(hdr)) ? 0 : -EIO;
}
/* 改名为带前缀版本 */
static int vuc_read_full(int fd, void *buf, size_t count) {
    uint8_t *p = buf;
    size_t done = 0;
    while (done < count) {
        ssize_t n = read(fd, p + done, count - done);
        if (n == 0) return -EPIPE;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        done += n;
    }
    return 0;
}

static int vuc_write_full(int fd, const void *buf, size_t count) {
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < count) {
        ssize_t n = write(fd, p + done, count - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        done += n;
    }
    return 0;
}

static int cryptodev_vhost_user_pre_save(CryptoDevBackend *backend,
                                         uint8_t **blob, uint32_t *len,
                                         uint64_t *epoch)
{
    CryptoDevBackendVhostUser *s = CRYPTODEV_BACKEND_VHOST_USER(backend);
    int sv[2] = { -1, -1 };
    int r = -1;

    *blob = NULL;
    *len  = 0;
    *epoch = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        return -errno;
    }

    /* 1) 通过 chardev 发送 SAVE_STATE 请求，并附带一个 FD 给后端 */
    r = vuc_send_with_fd_chr(&s->chr, VHOST_USER_CRYPTO_SAVE_STATE, sv[1]);
    if (r < 0) {
        goto out;
    }

    /* 2) 从 sv[0] 读取“快照头” */
    VC_SnapHdr hdr;
    r = vuc_read_full(sv[0], &hdr, sizeof(hdr));
    if (r) {                           /* r!=0 表示失败 */
        r = -EIO;
        goto out;
    }

    uint32_t paylen = le32_to_cpu(hdr.payload_len);
    if (!paylen || paylen > (64u << 20)) {   /* 防御：最大 64MB */
        r = -EINVAL;
        goto out;
    }

    /* 3) 读取 payload */
    uint8_t *buf = g_malloc(paylen);
    r = vuc_read_full(sv[0], buf, paylen);
    if (r) {
        g_free(buf);
        r = -EIO;
        goto out;
    }

    /* 4) （可选）CRC 校验：先跳过，链路打通后再加
       // #include "qemu/crc32c.h"
       // if (crc32c(0, buf, paylen) != le32_to_cpu(hdr.crc32)) {
       //     g_free(buf); r = -EBADMSG; goto out;
       // }
    */

    /* 5) 交付给 VMState */
    *blob = buf;
    *len  = paylen;
    r = 0;

out:
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    return r;
}
static int cryptodev_vhost_user_post_load(CryptoDevBackend *backend,
                                          const uint8_t *blob, uint32_t len,
                                          uint64_t epoch)
{
    CryptoDevBackendVhostUser *s = CRYPTODEV_BACKEND_VHOST_USER(backend);
    int sv[2] = { -1, -1 };
    int r = -1;

    if (!blob || !len) {
        return -EINVAL;
    }
    /* 防御：限制最大输入，避免后端被异常 len 撞死（可按需调整上限） */
    if (len > (64u << 20)) {  /* 64MB */
        return -EINVAL;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        return -errno;
    }

    /* 1) 通过 chardev 发送 LOAD_STATE，并附带 sv[1] 给后端 */
    r = vuc_send_with_fd_chr(&s->chr, VHOST_USER_CRYPTO_LOAD_STATE, sv[1]);
    /* 不再需要写端，先关掉，避免泄漏 */
    close(sv[1]);
    sv[1] = -1;
    if (r < 0) {
        goto out;
    }

    /* 2) 把 VMState 的 blob 发送给后端（vuc_write_full 返回 0 表成功） */
    r = vuc_write_full(sv[0], blob, len);
    if (r) {                  /* 非 0 说明写失败/被对端中断 */
        r = -EIO;
        goto out;
    }

    r = 0;
out:
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    return r;
}
static int
cryptodev_vhost_user_running(
             CryptoDevBackendVhost *crypto)
{
    return crypto ? 1 : 0;
}

CryptoDevBackendVhost *
cryptodev_vhost_user_get_vhost(
                         CryptoDevBackendClient *cc,
                         CryptoDevBackend *b,
                         uint16_t queue)
{
    CryptoDevBackendVhostUser *s =
                      CRYPTODEV_BACKEND_VHOST_USER(b);
    assert(cc->type == QCRYPTODEV_BACKEND_TYPE_VHOST_USER);
    assert(queue < MAX_CRYPTO_QUEUE_NUM);

    return s->vhost_crypto[queue];
}

static void cryptodev_vhost_user_stop(int queues,
                          CryptoDevBackendVhostUser *s)
{
    size_t i;

    for (i = 0; i < queues; i++) {
        if (!cryptodev_vhost_user_running(s->vhost_crypto[i])) {
            continue;
        }

        cryptodev_vhost_cleanup(s->vhost_crypto[i]);
        s->vhost_crypto[i] = NULL;
    }
}

static int
cryptodev_vhost_user_start(int queues,
                         CryptoDevBackendVhostUser *s)
{
    CryptoDevBackendVhostOptions options;
    CryptoDevBackend *b = CRYPTODEV_BACKEND(s);
    int max_queues;
    size_t i;

    for (i = 0; i < queues; i++) {
        if (cryptodev_vhost_user_running(s->vhost_crypto[i])) {
            continue;
        }

        options.opaque = &s->vhost_user;
        options.backend_type = VHOST_BACKEND_TYPE_USER;
        options.cc = b->conf.peers.ccs[i];
        s->vhost_crypto[i] = cryptodev_vhost_init(&options);
        if (!s->vhost_crypto[i]) {
            error_report("failed to init vhost_crypto for queue %zu", i);
            goto err;
        }

        if (i == 0) {
            max_queues =
              cryptodev_vhost_get_max_queues(s->vhost_crypto[i]);
            if (queues > max_queues) {
                error_report("you are asking more queues than supported: %d",
                             max_queues);
                goto err;
            }
        }
    }

    return 0;

err:
    cryptodev_vhost_user_stop(i + 1, s);
    return -1;
}

static Chardev *
cryptodev_vhost_claim_chardev(CryptoDevBackendVhostUser *s,
                                    Error **errp)
{
    Chardev *chr;

    if (s->chr_name == NULL) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE,
                   "chardev", "a valid character device");
        return NULL;
    }

    chr = qemu_chr_find(s->chr_name);
    if (chr == NULL) {
        error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                  "Device '%s' not found", s->chr_name);
        return NULL;
    }

    return chr;
}

static void cryptodev_vhost_user_event(void *opaque, QEMUChrEvent event)
{
    CryptoDevBackendVhostUser *s = opaque;
    CryptoDevBackend *b = CRYPTODEV_BACKEND(s);
    int queues = b->conf.peers.queues;

    assert(queues < MAX_CRYPTO_QUEUE_NUM);

    switch (event) {
    case CHR_EVENT_OPENED:
        if (cryptodev_vhost_user_start(queues, s) < 0) {
            exit(1);
        }
        b->ready = true;
        break;
    case CHR_EVENT_CLOSED:
        b->ready = false;
        cryptodev_vhost_user_stop(queues, s);
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        /* Ignore */
        break;
    }
}

static void cryptodev_vhost_user_init(
             CryptoDevBackend *backend, Error **errp)
{
    int queues = backend->conf.peers.queues;
    size_t i;
    Error *local_err = NULL;
    Chardev *chr;
    CryptoDevBackendClient *cc;
    CryptoDevBackendVhostUser *s =
                      CRYPTODEV_BACKEND_VHOST_USER(backend);

    chr = cryptodev_vhost_claim_chardev(s, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    s->opened = true;

    for (i = 0; i < queues; i++) {
        cc = cryptodev_backend_new_client();
        cc->info_str = g_strdup_printf("cryptodev-vhost-user%zu to %s ",
                                       i, chr->label);
        cc->queue_index = i;
        cc->type = QCRYPTODEV_BACKEND_TYPE_VHOST_USER;

        backend->conf.peers.ccs[i] = cc;

        if (i == 0) {
            if (!qemu_chr_fe_init(&s->chr, chr, errp)) {
                return;
            }
        }
    }

    if (!vhost_user_init(&s->vhost_user, &s->chr, errp)) {
        return;
    }

    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL,
                     cryptodev_vhost_user_event, NULL, s, NULL, true);

    backend->conf.crypto_services =
                         1u << QCRYPTODEV_BACKEND_SERVICE_TYPE_CIPHER |
                         1u << QCRYPTODEV_BACKEND_SERVICE_TYPE_HASH |
                         1u << QCRYPTODEV_BACKEND_SERVICE_TYPE_MAC;
    backend->conf.cipher_algo_l = 1u << VIRTIO_CRYPTO_CIPHER_AES_CBC;
    backend->conf.hash_algo = 1u << VIRTIO_CRYPTO_HASH_SHA1;

    backend->conf.max_size = UINT64_MAX;
    backend->conf.max_cipher_key_len = VHOST_USER_MAX_CIPHER_KEY_LEN;
    backend->conf.max_auth_key_len = VHOST_USER_MAX_AUTH_KEY_LEN;
}

static int64_t cryptodev_vhost_user_crypto_create_session(
           CryptoDevBackend *backend,
           CryptoDevBackendSessionInfo *sess_info,
           uint32_t queue_index, Error **errp)
{
    CryptoDevBackendClient *cc =
                   backend->conf.peers.ccs[queue_index];
    CryptoDevBackendVhost *vhost_crypto;
    uint64_t session_id = 0;
    int ret;

    vhost_crypto = cryptodev_vhost_user_get_vhost(cc, backend, queue_index);
    if (vhost_crypto) {
        struct vhost_dev *dev = &(vhost_crypto->dev);
        ret = dev->vhost_ops->vhost_crypto_create_session(dev,
                                                          sess_info,
                                                          &session_id);
        if (ret < 0) {
            return -1;
        } else {
            return session_id;
        }
    }
    return -1;
}

static int cryptodev_vhost_user_create_session(
           CryptoDevBackend *backend,
           CryptoDevBackendSessionInfo *sess_info,
           uint32_t queue_index,
           CryptoDevCompletionFunc cb,
           void *opaque)
{
    uint32_t op_code = sess_info->op_code;
    int64_t ret;
    Error *local_error = NULL;
    int status;

    switch (op_code) {
    case VIRTIO_CRYPTO_CIPHER_CREATE_SESSION:
    case VIRTIO_CRYPTO_AKCIPHER_CREATE_SESSION:
    case VIRTIO_CRYPTO_HASH_CREATE_SESSION:
    case VIRTIO_CRYPTO_MAC_CREATE_SESSION:
    case VIRTIO_CRYPTO_AEAD_CREATE_SESSION:
        ret = cryptodev_vhost_user_crypto_create_session(backend, sess_info,
                   queue_index, &local_error);
        break;

    default:
        error_report("Unsupported opcode :%" PRIu32 "", sess_info->op_code);
        return -VIRTIO_CRYPTO_NOTSUPP;
    }

    if (local_error) {
        error_report_err(local_error);
    }
    if (ret < 0) {
        status = -VIRTIO_CRYPTO_ERR;
    } else {
        sess_info->session_id = ret;
        status = VIRTIO_CRYPTO_OK;
    }
    if (cb) {
        cb(opaque, status);
    }
    return 0;
}

static int cryptodev_vhost_user_close_session(
           CryptoDevBackend *backend,
           uint64_t session_id,
           uint32_t queue_index,
           CryptoDevCompletionFunc cb,
           void *opaque)
{
    CryptoDevBackendClient *cc =
                  backend->conf.peers.ccs[queue_index];
    CryptoDevBackendVhost *vhost_crypto;
    int ret = -1, status;

    vhost_crypto = cryptodev_vhost_user_get_vhost(cc, backend, queue_index);
    if (vhost_crypto) {
        struct vhost_dev *dev = &(vhost_crypto->dev);
        ret = dev->vhost_ops->vhost_crypto_close_session(dev,
                                                         session_id);
        if (ret < 0) {
            status = -VIRTIO_CRYPTO_ERR;
        } else {
            status = VIRTIO_CRYPTO_OK;
        }
    } else {
        status = -VIRTIO_CRYPTO_NOTSUPP;
    }
    if (cb) {
        cb(opaque, status);
    }
    return 0;
}

static void cryptodev_vhost_user_cleanup(
             CryptoDevBackend *backend,
             Error **errp)
{
    CryptoDevBackendVhostUser *s =
                      CRYPTODEV_BACKEND_VHOST_USER(backend);
    size_t i;
    int queues = backend->conf.peers.queues;
    CryptoDevBackendClient *cc;

    cryptodev_vhost_user_stop(queues, s);

    for (i = 0; i < queues; i++) {
        cc = backend->conf.peers.ccs[i];
        if (cc) {
            cryptodev_backend_free_client(cc);
            backend->conf.peers.ccs[i] = NULL;
        }
    }

    vhost_user_cleanup(&s->vhost_user);
}

static void cryptodev_vhost_user_set_chardev(Object *obj,
                                    const char *value, Error **errp)
{
    CryptoDevBackendVhostUser *s =
                      CRYPTODEV_BACKEND_VHOST_USER(obj);

    if (s->opened) {
        error_setg(errp, "Property 'chardev' can no longer be set");
    } else {
        g_free(s->chr_name);
        s->chr_name = g_strdup(value);
    }
}

static char *
cryptodev_vhost_user_get_chardev(Object *obj, Error **errp)
{
    CryptoDevBackendVhostUser *s =
                      CRYPTODEV_BACKEND_VHOST_USER(obj);
    Chardev *chr = qemu_chr_fe_get_driver(&s->chr);

    if (chr && chr->label) {
        return g_strdup(chr->label);
    }

    return NULL;
}

static void cryptodev_vhost_user_finalize(Object *obj)
{
    CryptoDevBackendVhostUser *s =
                      CRYPTODEV_BACKEND_VHOST_USER(obj);

    qemu_chr_fe_deinit(&s->chr, false);

    g_free(s->chr_name);
    g_free(s->mig_blob); 
}

static void
cryptodev_vhost_user_class_init(ObjectClass *oc, const void *data)
{
    CryptoDevBackendClass *bc = CRYPTODEV_BACKEND_CLASS(oc);

    bc->init = cryptodev_vhost_user_init;
    bc->cleanup = cryptodev_vhost_user_cleanup;
    bc->create_session = cryptodev_vhost_user_create_session;
    bc->close_session = cryptodev_vhost_user_close_session;
    bc->pre_save = cryptodev_vhost_user_pre_save;   /* <— 新增 */
    bc->post_load = cryptodev_vhost_user_post_load; /* <— 新增 */
    bc->do_op = NULL;

    object_class_property_add_str(oc, "chardev",
                                  cryptodev_vhost_user_get_chardev,
                                  cryptodev_vhost_user_set_chardev);

}

static const TypeInfo cryptodev_vhost_user_info = {
    .name = TYPE_CRYPTODEV_BACKEND_VHOST_USER,
    .parent = TYPE_CRYPTODEV_BACKEND,
    .class_init = cryptodev_vhost_user_class_init,
    .instance_finalize = cryptodev_vhost_user_finalize,
    .instance_size = sizeof(CryptoDevBackendVhostUser),
};

static void
cryptodev_vhost_user_register_types(void)
{
    type_register_static(&cryptodev_vhost_user_info);
}

type_init(cryptodev_vhost_user_register_types);
