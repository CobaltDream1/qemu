#ifndef COMPRESSDEV_VHOST_USER_H
#define COMPRESSDEV_VHOST_USER_H

#include "system/compressdev-vhost.h"

#define VHOST_USER_MAX_AUTH_KEY_LEN    512
#define VHOST_USER_MAX_CIPHER_KEY_LEN  64


/**
 * compressdev_vhost_user_get_vhost:
 * @cc: the client object for each queue
 * @b: the compressdev backend common vhost object
 * @queue: the queue index
 *
 * Gets a new compressdev backend common vhost object based on
 * @b and @queue
 *
 * Returns: the compressdev backend common vhost object
 */
CompressDevBackendVhost *
compressdev_vhost_user_get_vhost(
                         CompressDevBackendClient *cc,
                         CompressDevBackend *b,
                         uint16_t queue);

#endif /* COMPRESSDEV_VHOST_USER_H */
