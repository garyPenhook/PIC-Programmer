#ifndef SECURITY_H
#define SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_NONCE_SIZE 16u
#define SECURITY_TAG_SIZE 32u

typedef enum {
    SECURITY_OK = 0,
    SECURITY_ERR_INVALID = -1,
    SECURITY_ERR_AUTH = -2,
    SECURITY_ERR_ROLLBACK = -3,
} security_status_t;

typedef struct {
    uint32_t version;
    uint32_t image_size;
    uint8_t image_hash[32];
    uint8_t signature[64];
} firmware_manifest_t;

void security_init(void);
bool security_boot_policy_ok(void);
security_status_t security_verify_manifest(const firmware_manifest_t *manifest);
security_status_t security_verify_command(const uint8_t *payload,
                                          size_t payload_len,
                                          const uint8_t nonce[SECURITY_NONCE_SIZE],
                                          const uint8_t tag[SECURITY_TAG_SIZE]);

#endif

