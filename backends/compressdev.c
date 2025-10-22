#include "qemu/osdep.h"
#include "system/compressdev.h"
#include "system/stats.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-compressdev.h"
#include "qapi/qapi-types-stats.h"
#include "qapi/visitor.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qom/object_interfaces.h"
#include "hw/virtio/virtio-comp.h"

#define STATEFUL_COMPRESS_OPS_STR "stateful-compress-ops"
#define STATEFUL_DECOMPRESS_OPS_STR "stateful-decompress-ops"
#define STATEFUL_COMPRESS_BYTES_STR "stateful-compress-bytes"
#define STATEFUL_DECOMPRESS_BYTES_STR "stateful-decompress-bytes"

#define STATELESS_COMPRESS_OPS_STR "stateless-compress-ops"
#define STATELESS_DECOMPRESS_OPS_STR "stateless-decompress-ops"
#define STATELESS_COMPRESS_BYTES_STR "stateless-compress-bytes"
#define STATELESS_DECOMPRESS_BYTES_STR "stateless-decompress-bytes"

typedef struct StatsArgs {
    union StatsResultsType {
        StatsResultList **stats;
        StatsSchemaList **schema;
    } result;
    strList *names;
    Error **errp;
} StatsArgs;

static QTAILQ_HEAD(, CompressDevBackendClient) compress_clients;

static int qmp_query_compressdev_foreach(Object *obj, void *data)
{
    CompressDevBackend *backend;
    QCompressdevInfoList **infolist = data;
    uint32_t services, i;

    if (!object_dynamic_cast(obj, TYPE_COMPRESSDEV_BACKEND)) {
        return 0;
    }

    QCompressdevInfo *info = g_new0(QCompressdevInfo, 1);
    info->id = g_strdup(object_get_canonical_path_component(obj));

    backend = COMPRESSDEV_BACKEND(obj);
    services = backend->conf.compress_services;
    for (i = 0; i < QCOMPRESSDEV_BACKEND_SERVICE_TYPE__MAX; i++) {
        if (services & (1 << i)) {
            QAPI_LIST_PREPEND(info->service, i);
        }
    }

    for (i = 0; i < backend->conf.peers.queues; i++) {
        CompressDevBackendClient *cc = backend->conf.peers.ccs[i];
        QCompressdevBackendClient *client = g_new0(QCompressdevBackendClient, 1);

        client->queue = cc->queue_index;
        client->type = cc->type;
        QAPI_LIST_PREPEND(info->client, client);
    }

    QAPI_LIST_PREPEND(*infolist, info);

    return 0;
}

QCompressdevInfoList *qmp_query_compressdev(Error **errp)
{
    QCompressdevInfoList *list = NULL;
    Object *objs = object_get_container("objects");

    object_child_foreach(objs, qmp_query_compressdev_foreach, &list);

    return list;
}

CompressDevBackendClient *compressdev_backend_new_client(void)
{
    CompressDevBackendClient *cc;

    cc = g_new0(CompressDevBackendClient, 1);
    QTAILQ_INSERT_TAIL(&compress_clients, cc, next);

    return cc;
}

void compressdev_backend_free_client(
                  CompressDevBackendClient *cc)
{
    QTAILQ_REMOVE(&compress_clients, cc, next);
    g_free(cc->info_str);
    g_free(cc);
}

void compressdev_backend_cleanup(
             CompressDevBackend *backend,
             Error **errp)
{
    CompressDevBackendClass *bc =
                  COMPRESSDEV_BACKEND_GET_CLASS(backend);

    if (bc->cleanup) {
        bc->cleanup(backend, errp);
    }

    g_free(backend->stateful_stat);
    g_free(backend->stateless_stat);
}

int compressdev_backend_create_session(
           CompressDevBackend *backend,
           CompressDevBackendSessionInfo *sess_info,
           uint32_t queue_index,
           CompressDevCompletionFunc cb,
           void *opaque)
{
    CompressDevBackendClass *bc =
                      COMPRESSDEV_BACKEND_GET_CLASS(backend);

    if (bc->create_session) {
        return bc->create_session(backend, sess_info, queue_index, cb, opaque);
    }
    return -VIRTIO_COMP_NOTSUPP;
}

int compressdev_backend_close_session(
           CompressDevBackend *backend,
           uint64_t session_id,
           uint32_t queue_index,
           CompressDevCompletionFunc cb,
           void *opaque)
{
    CompressDevBackendClass *bc =
                      COMPRESSDEV_BACKEND_GET_CLASS(backend);

    if (bc->close_session) {
        return bc->close_session(backend, session_id, queue_index, cb, opaque);
    }
    return -VIRTIO_COMP_NOTSUPP;
}

static int compressdev_backend_operation(
                 CompressDevBackend *backend,
                 CompressDevBackendOpInfo *op_info)
{
    CompressDevBackendClass *bc =
                      COMPRESSDEV_BACKEND_GET_CLASS(backend);

    if (bc->do_op) {
        return bc->do_op(backend, op_info);
    }
    return -VIRTIO_COMP_NOTSUPP;
}

static int compressdev_backend_account(CompressDevBackend *backend,
                 CompressDevBackendOpInfo *op_info)
{
    enum QCompressdevBackendAlgoType algtype = op_info->algtype;
    int len;

    if (algtype == QCOMPRESSDEV_BACKEND_ALGO_TYPE_STATELESS) {
        CompressDevBackendStatelessOpInfo *stateless_op_info = op_info->u.stateless_op_info;
        len = stateless_op_info->src_len;

        // if (unlikely(!backend->stateless_stat)) {
        //     return -VIRTIO_COMP_NOTSUPP;
        // }
        switch (op_info->op_code) {
        case VIRTIO_COMP_STATELESS_COMPRESS:
            CompressdevStatelessStatIncCompress(backend, len);
            break;
        case VIRTIO_COMP_STATELESS_DECOMPRESS:
            CompressdevStatelessStatIncDecompress(backend, len);
            break;
        }
    } else if (algtype == QCOMPRESSDEV_BACKEND_ALGO_TYPE_STATEFUL) {
        CompressDevBackendStatefulOpInfo *stateful_op_info = op_info->u.stateful_op_info;
        len = stateful_op_info->src_len;

        if (unlikely(!backend->stateful_stat)) {
            error_report("compressdev: Unexpected sym operation");
            return -VIRTIO_COMP_NOTSUPP;
        }
        switch (op_info->op_code) {
        case VIRTIO_COMP_STATEFUL_COMPRESS:
            CompressdevStatefulStatIncCompress(backend, len);
            break;
        case VIRTIO_COMP_STATEFUL_DECOMPRESS:
            CompressdevStatefulStatIncDecompress(backend, len);
            break;
        default:
            return -VIRTIO_COMP_NOTSUPP;
        }
    } else {
        error_report("Unsupported compressdev alg type: %" PRIu32 "", algtype);
        return -VIRTIO_COMP_NOTSUPP;
    }

    return len;
}

static void compressdev_backend_throttle_timer_cb(void *opaque)
{
    CompressDevBackend *backend = (CompressDevBackend *)opaque;
    CompressDevBackendOpInfo *op_info, *tmpop;
    int ret;

    QTAILQ_FOREACH_SAFE(op_info, &backend->opinfos, next, tmpop) {
        QTAILQ_REMOVE(&backend->opinfos, op_info, next);
        ret = compressdev_backend_account(backend, op_info);
        if (ret < 0) {
            op_info->cb(op_info->opaque, ret);
            continue;
        }

        throttle_account(&backend->ts, THROTTLE_WRITE, ret);
        compressdev_backend_operation(backend, op_info);
        if (throttle_enabled(&backend->tc) &&
            throttle_schedule_timer(&backend->ts, &backend->tt,
                                    THROTTLE_WRITE)) {
            break;
        }
    }
}

int compressdev_backend_compress_operation(
                 CompressDevBackend *backend,
                 CompressDevBackendOpInfo *op_info)
{
    int ret;

    if (!throttle_enabled(&backend->tc)) {
        goto do_account;
    }

    if (throttle_schedule_timer(&backend->ts, &backend->tt, THROTTLE_WRITE) ||
        !QTAILQ_EMPTY(&backend->opinfos)) {
        QTAILQ_INSERT_TAIL(&backend->opinfos, op_info, next);
        return 0;
    }

do_account:
    ret = compressdev_backend_account(backend, op_info);
    if (ret < 0) {
        return ret;
    }

    throttle_account(&backend->ts, THROTTLE_WRITE, ret);

    return compressdev_backend_operation(backend, op_info);
}

static void
compressdev_backend_get_queues(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);
    uint32_t value = backend->conf.peers.queues;

    visit_type_uint32(v, name, &value, errp);
}

static void
compressdev_backend_set_queues(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value) {
        error_setg(errp, "Property '%s.%s' doesn't take value '%" PRIu32 "'",
                   object_get_typename(obj), name, value);
        return;
    }
    backend->conf.peers.queues = value;
}

static void compressdev_backend_set_throttle(CompressDevBackend *backend, int field,
                                           uint64_t value, Error **errp)
{
    uint64_t orig = backend->tc.buckets[field].avg;
    bool enabled = throttle_enabled(&backend->tc);

    if (orig == value) {
        return;
    }

    backend->tc.buckets[field].avg = value;
    if (!throttle_enabled(&backend->tc)) {
        throttle_timers_destroy(&backend->tt);
        compressdev_backend_throttle_timer_cb(backend); /* drain opinfos */
        return;
    }

    if (!throttle_is_valid(&backend->tc, errp)) {
        backend->tc.buckets[field].avg = orig; /* revert change */
        return;
    }

    if (!enabled) {
        throttle_init(&backend->ts);
        throttle_timers_init(&backend->tt, qemu_get_aio_context(),
                             QEMU_CLOCK_REALTIME, NULL,
                             compressdev_backend_throttle_timer_cb, backend);
    }

    throttle_config(&backend->ts, QEMU_CLOCK_REALTIME, &backend->tc);
}

static void compressdev_backend_get_bps(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);
    uint64_t value = backend->tc.buckets[THROTTLE_BPS_TOTAL].avg;

    visit_type_uint64(v, name, &value, errp);
}

static void compressdev_backend_set_bps(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    compressdev_backend_set_throttle(backend, THROTTLE_BPS_TOTAL, value, errp);
}

static void compressdev_backend_get_ops(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);
    uint64_t value = backend->tc.buckets[THROTTLE_OPS_TOTAL].avg;

    visit_type_uint64(v, name, &value, errp);
}

static void compressdev_backend_set_ops(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    compressdev_backend_set_throttle(backend, THROTTLE_OPS_TOTAL, value, errp);
}

static void
compressdev_backend_complete(UserCreatable *uc, Error **errp)
{
    ERRP_GUARD();
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(uc);
    CompressDevBackendClass *bc = COMPRESSDEV_BACKEND_GET_CLASS(uc);
    uint32_t services;
    uint64_t value;

    QTAILQ_INIT(&backend->opinfos);
    value = backend->tc.buckets[THROTTLE_OPS_TOTAL].avg;
    compressdev_backend_set_throttle(backend, THROTTLE_OPS_TOTAL, value, errp);
    if (*errp) {
        return;
    }
    value = backend->tc.buckets[THROTTLE_BPS_TOTAL].avg;
    compressdev_backend_set_throttle(backend, THROTTLE_BPS_TOTAL, value, errp);
    if (*errp) {
        return;
    }

    if (bc->init) {
        bc->init(backend, errp);
        if (*errp) {
            return;
        }
    }

    services = backend->conf.compress_services;
    if (services & (1 << QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATEFUL)) {
        backend->stateful_stat = g_new0(CompressdevBackendStatefulStat, 1);
    }

    if (services & (1 << QCOMPRESSDEV_BACKEND_SERVICE_TYPE_STATELESS)) {
        backend->stateless_stat = g_new0(CompressdevBackendStatelessStat, 1);
    }
}

void compressdev_backend_set_used(CompressDevBackend *backend, bool used)
{
    backend->is_used = used;
}

bool compressdev_backend_is_used(CompressDevBackend *backend)
{
    return backend->is_used;
}

void compressdev_backend_set_ready(CompressDevBackend *backend, bool ready)
{
    backend->ready = ready;
}

bool compressdev_backend_is_ready(CompressDevBackend *backend)
{
    return backend->ready;
}

static bool
compressdev_backend_can_be_deleted(UserCreatable *uc)
{
    return !compressdev_backend_is_used(COMPRESSDEV_BACKEND(uc));
}

static void compressdev_backend_instance_init(Object *obj)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);

    /* Initialize devices' queues property to 1 */
    object_property_set_int(obj, "queues", 1, NULL);

    throttle_config_init(&backend->tc);
}

static void compressdev_backend_finalize(Object *obj)
{
    CompressDevBackend *backend = COMPRESSDEV_BACKEND(obj);

    compressdev_backend_cleanup(backend, NULL);
    if (throttle_enabled(&backend->tc)) {
        throttle_timers_destroy(&backend->tt);
    }
}

static StatsList *compressdev_backend_stats_add(const char *name, int64_t *val,
                                              StatsList *stats_list)
{
    Stats *stats = g_new0(Stats, 1);

    stats->name = g_strdup(name);
    stats->value = g_new0(StatsValue, 1);
    stats->value->type = QTYPE_QNUM;
    stats->value->u.scalar = *val;

    QAPI_LIST_PREPEND(stats_list, stats);
    return stats_list;
}

static int compressdev_backend_stats_query(Object *obj, void *data)
{
    StatsArgs *stats_args = data;
    StatsResultList **stats_results = stats_args->result.stats;
    StatsList *stats_list = NULL;
    StatsResult *entry;
    CompressDevBackend *backend;
    CompressdevBackendStatefulStat *stateful_stat;
    CompressdevBackendStatelessStat *stateless_stat;

    if (!object_dynamic_cast(obj, TYPE_COMPRESSDEV_BACKEND)) {
        return 0;
    }

    backend = COMPRESSDEV_BACKEND(obj);
    stateful_stat = backend->stateful_stat;
    if (stateful_stat) {
        stats_list = compressdev_backend_stats_add(STATEFUL_COMPRESS_OPS_STR,
                         &stateful_stat->compress_ops, stats_list);
        stats_list = compressdev_backend_stats_add(STATEFUL_DECOMPRESS_OPS_STR,
                         &stateful_stat->decompress_ops, stats_list);
        stats_list = compressdev_backend_stats_add(STATEFUL_COMPRESS_BYTES_STR,
                         &stateful_stat->compress_bytes, stats_list);
        stats_list = compressdev_backend_stats_add(STATEFUL_DECOMPRESS_BYTES_STR,
                         &stateful_stat->decompress_bytes, stats_list);
    }

    stateless_stat = backend->stateless_stat;
    if (stateless_stat) {
        stats_list = compressdev_backend_stats_add(STATELESS_COMPRESS_OPS_STR,
                         &stateless_stat->compress_ops, stats_list);
        stats_list = compressdev_backend_stats_add(STATELESS_DECOMPRESS_OPS_STR,
                         &stateless_stat->decompress_ops, stats_list);
        stats_list = compressdev_backend_stats_add(STATELESS_COMPRESS_BYTES_STR,
                         &stateless_stat->compress_bytes, stats_list);
        stats_list = compressdev_backend_stats_add(STATELESS_DECOMPRESS_BYTES_STR,
                         &stateless_stat->decompress_bytes, stats_list);
    }

    entry = g_new0(StatsResult, 1);
    entry->provider = STATS_PROVIDER_COMPRESSDEV;
    entry->qom_path = object_get_canonical_path(obj);
    entry->stats = stats_list;
    QAPI_LIST_PREPEND(*stats_results, entry);

    return 0;
}

static void compressdev_backend_stats_cb(StatsResultList **result,
                                       StatsTarget target,
                                       strList *names, strList *targets,
                                       Error **errp)
{
    switch (target) {
    case STATS_TARGET_COMPRESSDEV:
    {
        Object *objs = object_get_container("objects");
        StatsArgs stats_args;
        stats_args.result.stats = result;
        stats_args.names = names;
        stats_args.errp = errp;

        object_child_foreach(objs, compressdev_backend_stats_query, &stats_args);
        break;
    }
    default:
        break;
    }
}

static StatsSchemaValueList *compressdev_backend_schemas_add(const char *name,
                                 StatsSchemaValueList *list)
{
    StatsSchemaValueList *schema_entry = g_new0(StatsSchemaValueList, 1);

    schema_entry->value = g_new0(StatsSchemaValue, 1);
    schema_entry->value->type = STATS_TYPE_CUMULATIVE;
    schema_entry->value->name = g_strdup(name);
    schema_entry->next = list;

    return schema_entry;
}

static void compressdev_backend_schemas_cb(StatsSchemaList **result,
                                         Error **errp)
{
    StatsSchemaValueList *stats_list = NULL;
    const char *stateful_stats[] = { STATEFUL_COMPRESS_OPS_STR, STATEFUL_DECOMPRESS_OPS_STR,
                                STATEFUL_COMPRESS_BYTES_STR, STATEFUL_DECOMPRESS_BYTES_STR };
    const char *stateless_stats[] = { STATELESS_COMPRESS_OPS_STR, STATELESS_DECOMPRESS_OPS_STR,
                                 STATELESS_COMPRESS_BYTES_STR, STATELESS_DECOMPRESS_BYTES_STR };

    for (int i = 0; i < ARRAY_SIZE(stateful_stats); i++) {
        stats_list = compressdev_backend_schemas_add(stateful_stats[i], stats_list);
    }

    for (int i = 0; i < ARRAY_SIZE(stateless_stats); i++) {
        stats_list = compressdev_backend_schemas_add(stateless_stats[i], stats_list);
    }

    add_stats_schema(result, STATS_PROVIDER_COMPRESSDEV, STATS_TARGET_COMPRESSDEV,
                     stats_list);
}

static void
compressdev_backend_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = compressdev_backend_complete;
    ucc->can_be_deleted = compressdev_backend_can_be_deleted;

    QTAILQ_INIT(&compress_clients);
    object_class_property_add(oc, "queues", "uint32",
                              compressdev_backend_get_queues,
                              compressdev_backend_set_queues,
                              NULL, NULL);
    object_class_property_add(oc, "throttle-bps", "uint64",
                              compressdev_backend_get_bps,
                              compressdev_backend_set_bps,
                              NULL, NULL);
    object_class_property_add(oc, "throttle-ops", "uint64",
                              compressdev_backend_get_ops,
                              compressdev_backend_set_ops,
                              NULL, NULL);

    add_stats_callbacks(STATS_PROVIDER_COMPRESSDEV, compressdev_backend_stats_cb,
                        compressdev_backend_schemas_cb);
}

static const TypeInfo compressdev_backend_info = {
    .name = TYPE_COMPRESSDEV_BACKEND,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CompressDevBackend),
    .instance_init = compressdev_backend_instance_init,
    .instance_finalize = compressdev_backend_finalize,
    .class_size = sizeof(CompressDevBackendClass),
    .class_init = compressdev_backend_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void
compressdev_backend_register_types(void)
{
    type_register_static(&compressdev_backend_info);
}

type_init(compressdev_backend_register_types);
