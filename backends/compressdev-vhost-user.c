#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "hw/virtio/vhost-user.h"
#include "standard-headers/linux/virtio_comp.h"
#include "system/compressdev-vhost.h"
#include "chardev/char-fe.h"
#include "system/compressdev-vhost-user.h"
#include "qom/object.h"


/**
 * @TYPE_COMPRESSDEV_BACKEND_VHOST_USER:
 * name of backend that uses vhost user server
 */
#define TYPE_COMPRESSDEV_BACKEND_VHOST_USER "compressdev-vhost-user"

OBJECT_DECLARE_SIMPLE_TYPE(CompressDevBackendVhostUser, COMPRESSDEV_BACKEND_VHOST_USER)


struct CompressDevBackendVhostUser {
    CompressDevBackend parent_obj;

    VhostUserState vhost_user;
    CharBackend chr;
    char *chr_name;
    bool opened;
    CompressDevBackendVhost *vhost_compress[MAX_COMPRESS_QUEUE_NUM];
};

static int
compressdev_vhost_user_running(
             CompressDevBackendVhost *compress)
{
    return compress ? 1 : 0;
}

CompressDevBackendVhost *
compressdev_vhost_user_get_vhost(
                         CompressDevBackendClient *cc,
                         CompressDevBackend *b,
                         uint16_t queue)
{
    CompressDevBackendVhostUser *s =
                      COMPRESSDEV_BACKEND_VHOST_USER(b);
    assert(cc->type == QCOMPRESSDEV_BACKEND_TYPE_VHOST_USER);
    assert(queue < MAX_COMPRESS_QUEUE_NUM);

    return s->vhost_compress[queue];
}

static void compressdev_vhost_user_stop(int queues,
                          CompressDevBackendVhostUser *s)
{
    size_t i;

    for (i = 0; i < queues; i++) {
        if (!compressdev_vhost_user_running(s->vhost_compress[i])) {
            continue;
        }

        compressdev_vhost_cleanup(s->vhost_compress[i]);
        s->vhost_compress[i] = NULL;
    }
}

static int
compressdev_vhost_user_start(int queues,
                         CompressDevBackendVhostUser *s)
{
    CompressDevBackendVhostOptions options;
    CompressDevBackend *b = COMPRESSDEV_BACKEND(s);
    int max_queues;
    size_t i;

    for (i = 0; i < queues; i++) {
        if (compressdev_vhost_user_running(s->vhost_compress[i])) {
            continue;
        }

        options.opaque = &s->vhost_user;
        options.backend_type = VHOST_BACKEND_TYPE_USER;
        options.cc = b->conf.peers.ccs[i];
        s->vhost_compress[i] = compressdev_vhost_init(&options);
        if (!s->vhost_compress[i]) {
            error_report("failed to init vhost_compress for queue %zu", i);
            goto err;
        }

        if (i == 0) {
            max_queues =
              compressdev_vhost_get_max_queues(s->vhost_compress[i]);
            if (queues > max_queues) {
                error_report("you are asking more queues than supported: %d",
                             max_queues);
                goto err;
            }
        }
    }

    return 0;

err:
    compressdev_vhost_user_stop(i + 1, s);
    return -1;
}

static Chardev *
compressdev_vhost_claim_chardev(CompressDevBackendVhostUser *s,
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

static void compressdev_vhost_user_event(void *opaque, QEMUChrEvent event)
{
    CompressDevBackendVhostUser *s = opaque;
    CompressDevBackend *b = COMPRESSDEV_BACKEND(s);
    int queues = b->conf.peers.queues;

    assert(queues < MAX_COMPRESS_QUEUE_NUM);

    switch (event) {
    case CHR_EVENT_OPENED:
        if (compressdev_vhost_user_start(queues, s) < 0) {
            exit(1);
        }
        b->ready = true;
        break;
    case CHR_EVENT_CLOSED:
        b->ready = false;
        compressdev_vhost_user_stop(queues, s);
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        /* Ignore */
        break;
    }
}

static void compressdev_vhost_user_init(
             CompressDevBackend *backend, Error **errp)
{
    int queues = backend->conf.peers.queues;
    size_t i;
    Error *local_err = NULL;
    Chardev *chr;
    CompressDevBackendClient *cc;
    CompressDevBackendVhostUser *s =
                      COMPRESSDEV_BACKEND_VHOST_USER(backend);

    chr = compressdev_vhost_claim_chardev(s, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    s->opened = true;

    for (i = 0; i < queues; i++) {
        cc = compressdev_backend_new_client();
        cc->info_str = g_strdup_printf("compressdev-vhost-user%zu to %s ",
                                       i, chr->label);
        cc->queue_index = i;
        cc->type = QCOMPRESSDEV_BACKEND_TYPE_VHOST_USER;

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
                     compressdev_vhost_user_event, NULL, s, NULL, true);

    backend->conf.compress_services =
                         1u << QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATEFUL |
                         1u << QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATELESS;
    backend->conf.comp_algo = 1u << VIRTIO_COMP_ALGO_DEFLATE;
    // backend->conf.hash_algo = 1u << VIRTIO_COMPRESS_HASH_SHA1;

    backend->conf.max_size = UINT64_MAX;
}

static int64_t compressdev_vhost_user_compress_create_session(
           CompressDevBackend *backend,
           CompressDevBackendSessionInfo *sess_info,
           uint32_t queue_index, Error **errp)
{
    CompressDevBackendClient *cc =
                   backend->conf.peers.ccs[queue_index];
    CompressDevBackendVhost *vhost_compress;
    uint64_t session_id = 0;
    int ret;

    vhost_compress = compressdev_vhost_user_get_vhost(cc, backend, queue_index);
    if (vhost_compress) {
        struct vhost_dev *dev = &(vhost_compress->dev);
        ret = dev->vhost_ops->vhost_compress_create_session(dev,
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

static int compressdev_vhost_user_create_session(
           CompressDevBackend *backend,
           CompressDevBackendSessionInfo *sess_info,
           uint32_t queue_index,
           CompressDevCompletionFunc cb,
           void *opaque)
{
    uint32_t op_code = sess_info->op_code;
    int64_t ret;
    Error *local_error = NULL;
    int status;

    switch (op_code) {
    case VIRTIO_COMP_STATEFUL_CREATE_SESSION:
    case VIRTIO_COMP_STATELESS_CREATE_SESSION:
        ret = compressdev_vhost_user_compress_create_session(backend, sess_info,
                   queue_index, &local_error);
        break;

    default:
        error_report("Unsupported opcode :%" PRIu32 "", sess_info->op_code);
        return -VIRTIO_COMP_NOTSUPP;
    }

    if (local_error) {
        error_report_err(local_error);
    }
    if (ret < 0) {
        status = -VIRTIO_COMP_ERR;
    } else {
        sess_info->session_id = ret;
        status = VIRTIO_COMP_OK;
    }
    if (cb) {
        cb(opaque, status);
    }
    return 0;
}

static int compressdev_vhost_user_close_session(
           CompressDevBackend *backend,
           uint64_t session_id,
           uint32_t queue_index,
           CompressDevCompletionFunc cb,
           void *opaque)
{
    CompressDevBackendClient *cc =
                  backend->conf.peers.ccs[queue_index];
    CompressDevBackendVhost *vhost_compress;
    int ret = -1, status;

    vhost_compress = compressdev_vhost_user_get_vhost(cc, backend, queue_index);
    if (vhost_compress) {
        struct vhost_dev *dev = &(vhost_compress->dev);
        ret = dev->vhost_ops->vhost_compress_close_session(dev,
                                                         session_id);
        if (ret < 0) {
            status = -VIRTIO_COMP_ERR;
        } else {
            status = VIRTIO_COMP_OK;
        }
    } else {
        status = -VIRTIO_COMP_NOTSUPP;
    }
    if (cb) {
        cb(opaque, status);
    }
    return 0;
}

static void compressdev_vhost_user_cleanup(
             CompressDevBackend *backend,
             Error **errp)
{
    CompressDevBackendVhostUser *s =
                      COMPRESSDEV_BACKEND_VHOST_USER(backend);
    size_t i;
    int queues = backend->conf.peers.queues;
    CompressDevBackendClient *cc;

    compressdev_vhost_user_stop(queues, s);

    for (i = 0; i < queues; i++) {
        cc = backend->conf.peers.ccs[i];
        if (cc) {
            compressdev_backend_free_client(cc);
            backend->conf.peers.ccs[i] = NULL;
        }
    }

    vhost_user_cleanup(&s->vhost_user);
}

static void compressdev_vhost_user_set_chardev(Object *obj,
                                    const char *value, Error **errp)
{
    CompressDevBackendVhostUser *s =
                      COMPRESSDEV_BACKEND_VHOST_USER(obj);

    if (s->opened) {
        error_setg(errp, "Property 'chardev' can no longer be set");
    } else {
        g_free(s->chr_name);
        s->chr_name = g_strdup(value);
    }
}

static char *
compressdev_vhost_user_get_chardev(Object *obj, Error **errp)
{
    CompressDevBackendVhostUser *s =
                      COMPRESSDEV_BACKEND_VHOST_USER(obj);
    Chardev *chr = qemu_chr_fe_get_driver(&s->chr);

    if (chr && chr->label) {
        return g_strdup(chr->label);
    }

    return NULL;
}

static void compressdev_vhost_user_finalize(Object *obj)
{
    CompressDevBackendVhostUser *s =
                      COMPRESSDEV_BACKEND_VHOST_USER(obj);

    qemu_chr_fe_deinit(&s->chr, false);

    g_free(s->chr_name);
}

static void
compressdev_vhost_user_class_init(ObjectClass *oc, const void *data)
{
    CompressDevBackendClass *bc = COMPRESSDEV_BACKEND_CLASS(oc);

    bc->init = compressdev_vhost_user_init;
    bc->cleanup = compressdev_vhost_user_cleanup;
    bc->create_session = compressdev_vhost_user_create_session;
    bc->close_session = compressdev_vhost_user_close_session;
    bc->do_op = NULL;

    object_class_property_add_str(oc, "chardev",
                                  compressdev_vhost_user_get_chardev,
                                  compressdev_vhost_user_set_chardev);

}

static const TypeInfo compressdev_vhost_user_info = {
    .name = TYPE_COMPRESSDEV_BACKEND_VHOST_USER,
    .parent = TYPE_COMPRESSDEV_BACKEND,
    .class_init = compressdev_vhost_user_class_init,
    .instance_finalize = compressdev_vhost_user_finalize,
    .instance_size = sizeof(CompressDevBackendVhostUser),
};

static void
compressdev_vhost_user_register_types(void)
{
    type_register_static(&compressdev_vhost_user_info);
}

type_init(compressdev_vhost_user_register_types);
