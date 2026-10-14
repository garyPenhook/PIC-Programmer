#include "security.h"

#include "crypto_backend.h"
#include "platform.h"

void security_init(void)
{
    /* Load provisioned public key hash / device identity here. */
}

bool security_boot_policy_ok(void)
{
    return true;
}

security_status_t security_verify_manifest(const firmware_manifest_t *manifest)
{
    if (manifest == 0 || manifest->image_size == 0u) {
        return SECURITY_ERR_INVALID;
    }

    if (manifest->version < platform_monotonic_version()) {
        return SECURITY_ERR_ROLLBACK;
    }

    if (!crypto_verify_firmware_signature(manifest->image_hash, manifest->signature)) {
        return SECURITY_ERR_AUTH;
    }

    return SECURITY_OK;
}

security_status_t security_verify_command(const uint8_t *payload,
                                          size_t payload_len,
                                          const uint8_t nonce[SECURITY_NONCE_SIZE],
                                          const uint8_t tag[SECURITY_TAG_SIZE])
{
    if (payload == 0 || payload_len == 0u || nonce == 0 || tag == 0) {
        return SECURITY_ERR_INVALID;
    }

    if (!crypto_verify_command_tag(payload, payload_len, nonce, tag)) {
        return SECURITY_ERR_AUTH;
    }

    return SECURITY_OK;
}

