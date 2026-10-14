#include "crypto_backend.h"

bool crypto_verify_firmware_signature(const uint8_t hash[32],
                                      const uint8_t signature[64])
{
    (void)hash;
    (void)signature;

    /*
     * Replace with reviewed implementation:
     * - ECDSA P-256 / Ed25519 for firmware signatures.
     * - Public key or hash pinned in protected flash.
     */
    return false;
}

bool crypto_verify_command_tag(const uint8_t *payload,
                               size_t payload_len,
                               const uint8_t nonce[16],
                               const uint8_t tag[32])
{
    (void)payload;
    (void)payload_len;
    (void)nonce;
    (void)tag;

    /*
     * Replace with HMAC-SHA256 or AEAD tag check.
     * Must include nonce/replay window in authenticated data.
     */
    return false;
}

