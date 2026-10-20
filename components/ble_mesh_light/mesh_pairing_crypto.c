#include "mesh_pairing_crypto.h"
#include <string.h>

static void be16(uint8_t *p, uint16_t n) { p[0] = n >> 8; p[1] = n; }
static void be24(uint8_t *p, uint32_t n) { p[0] = n >> 16; p[1] = n >> 8; p[2] = n; }
static void be32(uint8_t *p, uint32_t n) { p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n; }

static void double_block(uint8_t block[16]) {
  uint8_t carry = 0, high = block[0] >> 7;
  for (int i = 15; i >= 0; i--) { uint8_t next = block[i] >> 7; block[i] = (block[i] << 1) | carry; carry = next; }
  if (high) block[15] ^= 0x87;
}

static bool cmac(const uint8_t key[16], const uint8_t *data, size_t length, uint8_t output[16]) {
  uint8_t subkey[16] = {0}, block[16] = {0};
  if (!mesh_crypto_aes(key, subkey, subkey)) return false;
  double_block(subkey);
  size_t blocks = length ? (length + 15) / 16 : 1;
  for (size_t n = 0; n + 1 < blocks; n++) {
    for (size_t i = 0; i < 16; i++) block[i] ^= data[n * 16 + i];
    if (!mesh_crypto_aes(key, block, block)) return false;
  }
  size_t remaining = length - (blocks - 1) * 16;
  if (remaining != 16) { double_block(subkey); block[remaining] ^= 0x80; }
  for (size_t i = 0; i < 16; i++) block[i] ^= subkey[i] ^ (i < remaining ? data[(blocks - 1) * 16 + i] : 0);
  return mesh_crypto_aes(key, block, output);
}

bool mesh_pairing_network_init(mesh_pairing_network_t *network, const uint8_t key[16], uint32_t iv) {
  uint8_t zero[16] = {0}, salt[16], t[16], t1[16], input[18];
  if (!cmac(zero, (const uint8_t *) "smk2", 4, salt) || !cmac(salt, key, 16, t)) return false;
  input[0] = 0; input[1] = 1;
  if (!cmac(t, input, 2, t1)) return false;
  network->nid = t1[15] & 0x7f;
  memcpy(input, t1, 16); input[16] = 0; input[17] = 2;
  if (!cmac(t, input, 18, network->encryption)) return false;
  memcpy(input, network->encryption, 16); input[17] = 3;
  if (!cmac(t, input, 18, network->privacy)) return false;
  network->iv_index = iv;
  return true;
}

bool mesh_pairing_session(const uint8_t confirmation[16], uint8_t device_key[16], uint8_t session_mask[22]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t input[42], digest[32];
  for (size_t i = 0; i < 16; i++) { input[i * 2] = hex[confirmation[i] >> 4]; input[i * 2 + 1] = hex[confirmation[i] & 15]; }
  memcpy(input + 32, "DeviceKey", 9);
  if (!mesh_crypto_sha256(input, 41, digest)) return false;
  memcpy(device_key, digest, 16);
  memcpy(input + 32, "SessionKey", 10);
  if (!mesh_crypto_sha256(input, 42, digest)) return false;
  memcpy(session_mask, digest, 22);
  memset(digest, 0, sizeof(digest));
  return true;
}

static size_t network_encode(const mesh_pairing_network_t *network, uint16_t source, uint16_t destination,
                             uint32_t sequence, uint8_t ttl, const uint8_t *lower, size_t length, uint8_t *packet) {
  if (length > 16 || sequence > 0xffffff) return 0;
  uint8_t header[6] = {ttl & 0x7f}, nonce[13] = {0}, plain[18], privacy[16] = {0}, mask[16];
  be24(header + 1, sequence); be16(header + 4, source);
  memcpy(nonce + 1, header, 6); be32(nonce + 9, network->iv_index);
  be16(plain, destination); memcpy(plain + 2, lower, length);
  packet[0] = ((network->iv_index & 1) << 7) | network->nid;
  if (!mesh_crypto_ccm(true, network->encryption, nonce, plain, length + 2, packet + 7, 4)) return 0;
  be32(privacy + 5, network->iv_index); memcpy(privacy + 9, packet + 7, 7);
  if (!mesh_crypto_aes(network->privacy, privacy, mask)) return 0;
  for (size_t i = 0; i < 6; i++) packet[1 + i] = header[i] ^ mask[i];
  return 7 + length + 2 + 4;
}

size_t mesh_pairing_access_encode(const mesh_pairing_network_t *network, const uint8_t key[16],
                                 uint16_t source, uint16_t destination, uint32_t sequence, uint8_t ttl,
                                 const uint8_t *access, size_t length, uint8_t packets[3][29], uint8_t sizes[3]) {
  if (!length || length > 28 || sequence > 0xfffffd) return 0;
  uint8_t nonce[13] = {2, 0}, upper[32], lower[16];
  be24(nonce + 2, sequence); be16(nonce + 5, source); be16(nonce + 7, destination); be32(nonce + 9, network->iv_index);
  if (!mesh_crypto_ccm(true, key, nonce, access, length, upper, 4)) return 0;
  size_t upper_length = length + 4;
  if (upper_length <= 15) {
    lower[0] = 0; memcpy(lower + 1, upper, upper_length);
    sizes[0] = network_encode(network, source, destination, sequence, ttl, lower, upper_length + 1, packets[0]);
    return sizes[0] ? 1 : 0;
  }
  size_t count = (upper_length + 11) / 12;
  for (size_t i = 0; i < count; i++) {
    size_t part = upper_length - i * 12; if (part > 12) part = 12;
    lower[0] = 0x80; be24(lower + 1, ((sequence & 0x1fff) << 10) | (i << 5) | (count - 1));
    memcpy(lower + 4, upper + i * 12, part);
    sizes[i] = network_encode(network, source, destination, sequence + i, ttl, lower, part + 4, packets[i]);
    if (!sizes[i]) return 0;
  }
  return count;
}

bool mesh_pairing_network_decode(const mesh_pairing_network_t *network, const uint8_t *data, size_t length,
                                 mesh_pairing_packet_t *packet) {
  if (length < 14 || length > 29 || data[0] != (((network->iv_index & 1) << 7) | network->nid)) return false;
  uint8_t privacy[16] = {0}, mask[16], header[6], nonce[13] = {0}, plain[22];
  be32(privacy + 5, network->iv_index); memcpy(privacy + 9, data + 7, 7);
  if (!mesh_crypto_aes(network->privacy, privacy, mask)) return false;
  for (size_t i = 0; i < 6; i++) header[i] = data[1 + i] ^ mask[i];
  bool control = (header[0] & 0x80) != 0;
  size_t mic = control ? 8 : 4;
  if (length < 10 + mic) return false;
  memcpy(nonce + 1, header, 6); be32(nonce + 9, network->iv_index);
  if (!mesh_crypto_ccm(false, network->encryption, nonce, data + 7, length - 7, plain, mic)) return false;
  packet->control = control;
  packet->sequence = ((uint32_t) header[1] << 16) | ((uint32_t) header[2] << 8) | header[3];
  packet->source = ((uint16_t) header[4] << 8) | header[5];
  packet->destination = ((uint16_t) plain[0] << 8) | plain[1];
  packet->lower_length = length - 9 - mic;
  memcpy(packet->lower, plain + 2, packet->lower_length);
  return true;
}

bool mesh_pairing_access_decode(const mesh_pairing_network_t *network, const mesh_pairing_packet_t *packet,
                                const uint8_t key[16], uint8_t *access, size_t *length) {
  if (packet->control || packet->lower_length < 6 || (packet->lower[0] & 0x80)) return false;
  bool application = (packet->lower[0] & 0x40) != 0;
  uint8_t nonce[13] = {application ? 1 : 2, 0};
  size_t plain_length = packet->lower_length - 5;
  if (*length < plain_length) return false;
  be24(nonce + 2, packet->sequence); be16(nonce + 5, packet->source); be16(nonce + 7, packet->destination);
  be32(nonce + 9, network->iv_index);
  if (!mesh_crypto_ccm(false, key, nonce, packet->lower + 1, packet->lower_length - 1, access, 4)) return false;
  *length = plain_length;
  return true;
}
