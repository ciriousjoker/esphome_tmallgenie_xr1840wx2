#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t nid;
  uint8_t encryption[16];
  uint8_t privacy[16];
  uint32_t iv_index;
} mesh_pairing_network_t;

typedef struct {
  uint16_t source, destination;
  uint32_t sequence;
  bool control;
  uint8_t lower[18];
  size_t lower_length;
} mesh_pairing_packet_t;

bool mesh_pairing_network_init(mesh_pairing_network_t *network, const uint8_t key[16], uint32_t iv);
bool mesh_pairing_session(const uint8_t confirmation[16], uint8_t device_key[16], uint8_t session_mask[22]);
size_t mesh_pairing_access_encode(const mesh_pairing_network_t *network, const uint8_t key[16],
                                 uint16_t source, uint16_t destination, uint32_t sequence, uint8_t ttl,
                                 const uint8_t *access, size_t length, uint8_t packets[3][29], uint8_t sizes[3]);
bool mesh_pairing_network_decode(const mesh_pairing_network_t *network, const uint8_t *data, size_t length,
                                 mesh_pairing_packet_t *packet);
bool mesh_pairing_access_decode(const mesh_pairing_network_t *network, const mesh_pairing_packet_t *packet,
                                const uint8_t key[16], uint8_t *access, size_t *length);

// Crypto provider implemented with ESP-IDF's mbedTLS; host tests use OpenSSL.
bool mesh_crypto_aes(const uint8_t key[16], const uint8_t input[16], uint8_t output[16]);
bool mesh_crypto_sha256(const uint8_t *data, size_t length, uint8_t output[32]);
bool mesh_crypto_ccm(bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *input, size_t length, uint8_t *output, size_t mic_length);
