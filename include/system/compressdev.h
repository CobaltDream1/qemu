#ifndef COMPRESSDEV_H
#define COMPRESSDEV_H

#include "qemu/queue.h"
#include "qemu/throttle.h"
#include "qom/object.h"
#include "qapi/qapi-types-compressdev.h"

/**
 * CompressDevBackend:
 *
 * The CompressDevBackend object is an interface
 * for different compressdev backends, which provides compress
 * operation wrapper.
 *
 */

#define TYPE_COMPRESSDEV_BACKEND "compressdev-backend"

OBJECT_DECLARE_TYPE(CompressDevBackend, CompressDevBackendClass,
                    COMPRESSDEV_BACKEND)


#define MAX_COMPRESS_QUEUE_NUM  64

typedef struct CompressDevBackendConf CompressDevBackendConf;
typedef struct CompressDevBackendPeers CompressDevBackendPeers;
typedef struct CompressDevBackendClient CompressDevBackendClient;

typedef struct CompressDevBackendDeflateParam {
    uint32_t huffman;
} CompressDevBackendDeflateParam;

/**
 * CompressDevBackendStatelessSessionInfo:
 *
 * @compress_alg: algorithm type of compress
 * @hash_alg: algorithm type of HASH/MAC
 * @hash_result_len: byte length of HASH operation result
 * @op_type: operation type (refer to virtio_compress.h)
 * @direction: compression or direction for CIPHER
 * @hash_mode: HASH mode for HASH operation (refer to virtio_compress.h)
 */
typedef struct CompressDevBackendStatelessSessionInfo {
    /* corresponding with virtio compress spec */
    uint32_t comp_algo;
    uint8_t direction;
    int level;
    uint8_t window_size;
    uint32_t chksum;
    uint32_t hash_algo;

    union {
        CompressDevBackendDeflateParam deflate;
    } u;
} CompressDevBackendStatelessSessionInfo;

/**
 * CompressDevBackendStatefulSessionInfo:
 */

typedef struct CompressDevBackendStatefulSessionInfo {
    /* corresponding with virtio compress spec */
    uint32_t comp_algo;
    uint8_t direction;
    int level;
    uint8_t window_size;
    uint32_t chksum;
    uint32_t hash_algo;

    union {
        CompressDevBackendDeflateParam deflate;
    } u;
} CompressDevBackendStatefulSessionInfo;

typedef struct CompressDevBackendSessionInfo {
    uint32_t op_code;
    uint32_t dir;
    union {
        CompressDevBackendStatelessSessionInfo stateless_sess_info;
        CompressDevBackendStatefulSessionInfo stateful_sess_info;
    } u;
    uint64_t session_id;
} CompressDevBackendSessionInfo;

/**
 * CompressDevBackendStatelessOpInfo:
 *
 * @src_len: byte length of source data
 * @dst_len: byte length of destination data
 * @digest_result_len: byte length of hash digest result
 * @op_type: operation type (refer to virtio_compress.h)
 * @src: point to the source data
 * @dst: point to the destination data
 * @aad_data: point to the additional authenticated data
 * @digest_result: point to the digest result data
 * @data[0]: point to the extensional memory by one memory allocation
 *
 */
typedef struct CompressDevBackendStatelessOpInfo {
    uint32_t src_len;
    uint32_t dst_len;
    uint32_t digest_result_len;
    uint8_t direction;
    uint8_t *src;
    uint8_t *dst;
    uint8_t *digest_result;
    uint8_t data[];
} CompressDevBackendStatelessOpInfo;


/**
 * CompressDevBackendStatefulOpInfo:
 *
 * @src_len: byte length of source data
 * @dst_len: byte length of destination data
 * @src: point to the source data
 * @dst: point to the destination data
 *
 */
typedef struct CompressDevBackendStatefulOpInfo {
    uint32_t src_len;
    uint32_t dst_len;
    uint8_t *src;
    uint8_t *dst;
} CompressDevBackendStatefulOpInfo;

typedef void (*CompressDevCompletionFunc) (void *opaque, int ret);

typedef struct CompressDevBackendOpInfo {
    QCompressdevBackendAlgoType algtype;
    uint32_t op_code;
    uint32_t queue_index;
    CompressDevCompletionFunc cb;
    void *opaque; /* argument for cb */
    uint64_t session_id;
    union {
        CompressDevBackendStatelessOpInfo *stateless_op_info;
        CompressDevBackendStatefulOpInfo *stateful_op_info;
    } u;
    QTAILQ_ENTRY(CompressDevBackendOpInfo) next;
} CompressDevBackendOpInfo;

struct CompressDevBackendClass {
    ObjectClass parent_class;

    void (*init)(CompressDevBackend *backend, Error **errp);
    void (*cleanup)(CompressDevBackend *backend, Error **errp);

    int (*create_session)(CompressDevBackend *backend,
                          CompressDevBackendSessionInfo *sess_info,
                          uint32_t queue_index,
                          CompressDevCompletionFunc cb,
                          void *opaque);

    int (*close_session)(CompressDevBackend *backend,
                         uint64_t session_id,
                         uint32_t queue_index,
                         CompressDevCompletionFunc cb,
                         void *opaque);

    int (*do_op)(CompressDevBackend *backend,
                 CompressDevBackendOpInfo *op_info);
};

struct CompressDevBackendClient {
    QCompressdevBackendType type;
    char *info_str;
    unsigned int queue_index;
    int vring_enable;
    QTAILQ_ENTRY(CompressDevBackendClient) next;
};

struct CompressDevBackendPeers {
    CompressDevBackendClient *ccs[MAX_COMPRESS_QUEUE_NUM];
    uint32_t queues;
};

struct CompressDevBackendConf {
    CompressDevBackendPeers peers;

    /* Supported service mask */
    uint32_t compress_services;

    /* Detailed algorithms mask */
    uint32_t comp_algo;
    uint32_t hash_algo;
    /* Maximum size of each compress request's content */
    uint64_t max_size;
};

typedef struct CompressdevBackendStatelessStat {
    int64_t compress_ops;
    int64_t decompress_ops;
    int64_t compress_bytes;
    int64_t decompress_bytes;
} CompressdevBackendStatelessStat;

typedef struct CompressdevBackendStatefulStat {
    int64_t compress_ops;
    int64_t decompress_ops;
    int64_t compress_bytes;
    int64_t decompress_bytes;
} CompressdevBackendStatefulStat;

struct CompressDevBackend {
    Object parent_obj;

    bool ready;
    /* Tag the compressdev backend is used by virtio-comp or not */
    bool is_used;
    CompressDevBackendConf conf;
    CompressdevBackendStatelessStat *stateless_stat;
    CompressdevBackendStatefulStat *stateful_stat;

    ThrottleState ts;
    ThrottleTimers tt;
    ThrottleConfig tc;
    QTAILQ_HEAD(, CompressDevBackendOpInfo) opinfos;
};

#define CompressdevStatelessStatInc(be, op, bytes) do { \
   be->stateless_stat->op##_bytes += (bytes); \
   be->stateless_stat->op##_ops += 1; \
} while (/*CONSTCOND*/0)

#define CompressdevStatelessStatIncCompress(be, bytes) \
            CompressdevStatelessStatInc(be, compress, bytes)

#define CompressdevStatelessStatIncDecompress(be, bytes) \
            CompressdevStatelessStatInc(be, decompress, bytes)

#define CompressdevStatefulStatInc(be, op, bytes) do { \
    be->stateful_stat->op##_bytes += (bytes); \
    be->stateful_stat->op##_ops += 1; \
} while (/*CONSTCOND*/0)

#define CompressdevStatefulStatIncCompress(be, bytes) \
            CompressdevStatefulStatInc(be, compress, bytes)

#define CompressdevStatefulStatIncDecompress(be, bytes) \
            CompressdevStatefulStatInc(be, decompress, bytes)

/**
 * compressdev_backend_new_client:
 *
 * Creates a new compressdev backend client object.
 *
 * The returned object must be released with
 * compressdev_backend_free_client() when no
 * longer required
 *
 * Returns: a new compressdev backend client object
 */
CompressDevBackendClient *compressdev_backend_new_client(void);

/**
 * compressdev_backend_free_client:
 * @cc: the compressdev backend client object
 *
 * Release the memory associated with @cc that
 * was previously allocated by compressdev_backend_new_client()
 */
void compressdev_backend_free_client(
                  CompressDevBackendClient *cc);

/**
 * compressdev_backend_cleanup:
 * @backend: the compressdev backend object
 * @errp: pointer to a NULL-initialized error object
 *
 * Clean the resource associated with @backend that realizaed
 * by the specific backend's init() callback
 */
void compressdev_backend_cleanup(
           CompressDevBackend *backend,
           Error **errp);

/**
 * compressdev_backend_create_session:
 * @backend: the compressdev backend object
 * @sess_info: parameters needed by session creating
 * @queue_index: queue index of compressdev backend client
 * @errp: pointer to a NULL-initialized error object
 * @cb: callback when session create is compeleted
 * @opaque: parameter passed to callback
 *
 * Create a session for compress algorithms
 *
 * Returns: 0 for success and cb will be called when creation is completed,
 * negative value for error, and cb will not be called.
 */
int compressdev_backend_create_session(
           CompressDevBackend *backend,
           CompressDevBackendSessionInfo *sess_info,
           uint32_t queue_index,
           CompressDevCompletionFunc cb,
           void *opaque);

/**
 * compressdev_backend_close_session:
 * @backend: the compressdev backend object
 * @session_id: the session id
 * @queue_index: queue index of compressdev backend client
 * @errp: pointer to a NULL-initialized error object
 * @cb: callback when session create is compeleted
 * @opaque: parameter passed to callback
 *
 * Close a session for which was previously
 * created by compressdev_backend_create_session()
 *
 * Returns: 0 for success and cb will be called when creation is completed,
 * negative value for error, and cb will not be called.
 */
int compressdev_backend_close_session(
           CompressDevBackend *backend,
           uint64_t session_id,
           uint32_t queue_index,
           CompressDevCompletionFunc cb,
           void *opaque);

/**
 * compressdev_backend_compress_operation:
 * @backend: the compressdev backend object
 * @op_info: pointer to a CompressDevBackendOpInfo object
 *
 * Do compress operation, such as compression, decompression, signature and
 * verification
 *
 * Returns: 0 for success and cb will be called when creation is completed,
 * negative value for error, and cb will not be called.
 */
int compressdev_backend_compress_operation(
                 CompressDevBackend *backend,
                 CompressDevBackendOpInfo *op_info);

/**
 * compressdev_backend_set_used:
 * @backend: the compressdev backend object
 * @used: true or false
 *
 * Set the compressdev backend is used by virtio-comp or not
 */
void compressdev_backend_set_used(CompressDevBackend *backend, bool used);

/**
 * compressdev_backend_is_used:
 * @backend: the compressdev backend object
 *
 * Return the status that the compressdev backend is used
 * by virtio-comp or not
 *
 * Returns: true on used, or false on not used
 */
bool compressdev_backend_is_used(CompressDevBackend *backend);

/**
 * compressdev_backend_set_ready:
 * @backend: the compressdev backend object
 * @ready: true or false
 *
 * Set the compressdev backend is ready or not, which is called
 * by the children of the compressdev banckend interface.
 */
void compressdev_backend_set_ready(CompressDevBackend *backend, bool ready);

/**
 * compressdev_backend_is_ready:
 * @backend: the compressdev backend object
 *
 * Return the status that the compressdev backend is ready or not
 *
 * Returns: true on ready, or false on not ready
 */
bool compressdev_backend_is_ready(CompressDevBackend *backend);

#endif /* COMPRESSDEV_H */
