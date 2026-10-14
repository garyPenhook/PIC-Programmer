#ifndef CRYPTO_BACKEND_H
#define CRYPTO_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool crypto_verify_firmware_signature(const uint8_t hash[32],
                                      const uint8_t signature[64]);
bool crypto_verify_command_tag(const uint8_t *payload,
                               size_t payload_len,
                               const uint8_t nonce[16],
                               const uint8_t tag[32]);

#endif

