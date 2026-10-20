#include "mesh_pairing_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void unhex(const char *str, uint8_t *out) {
  for (size_t i = 0; str[i * 2]; i++) { unsigned v; assert(sscanf(str + i * 2, "%2x", &v) == 1); out[i] = v; }
}
int main(void) {
  // Official Bluetooth Mesh example #6, also published by BlueZ test-mesh-crypto.c.
  uint8_t net[16], dev[16], access[20], expected[29], packets[3][29], sizes[3];
  unhex("7dd7364cd842ad18c17c2b820c84c3d6", net);
  unhex("9d6dd0e96eb25dc19a40ed9914f8f03f", dev);
  unhex("0056341263964771734fbd76e3b40519d1d94a48", access);
  mesh_pairing_network_t network;
  assert(mesh_pairing_network_init(&network, net, 0x12345678));
  assert(network.nid == 0x68);
  assert(mesh_pairing_access_encode(&network, dev, 3, 0x1201, 0x3129ab, 4, access, 20, packets, sizes) == 2);
  unhex("68cab5c5348a230afba8c63d4e686364979deaf4fd40961145939cda0e", expected);
  assert(sizes[0] == 29 && memcmp(packets[0], expected, 29) == 0);
  unhex("681615b5dd4a846cae0c032bf0746f44f1b8cc8ce5edc57e55beed49c0", expected);
  assert(sizes[1] == 29 && memcmp(packets[1], expected, 29) == 0);
  mesh_pairing_packet_t decoded;
  assert(mesh_pairing_network_decode(&network, packets[0], sizes[0], &decoded));
  assert(decoded.source == 3 && decoded.destination == 0x1201 && decoded.sequence == 0x3129ab);
  packets[0][20] ^= 1;
  assert(!mesh_pairing_network_decode(&network, packets[0], sizes[0], &decoded));
  uint8_t message[] = {0x80, 0x03, 0, 0, 0, 0}, output[16]; size_t length = sizeof(output);
  assert(mesh_pairing_access_encode(&network, dev, 0x1201, 3, 7, 4, message, sizeof(message), packets, sizes) == 1);
  assert(mesh_pairing_network_decode(&network, packets[0], sizes[0], &decoded));
  assert(mesh_pairing_access_decode(&network, &decoded, dev, output, &length));
  assert(length == sizeof(message) && memcmp(output, message, length) == 0);
  dev[0] ^= 1; length = sizeof(output);
  assert(!mesh_pairing_access_decode(&network, &decoded, dev, output, &length));
  for (size_t n = 0; n < 14; n++) assert(!mesh_pairing_network_decode(&network, packets[0], n, &decoded));
  uint8_t confirmation[16], device_key[16], mask[22], expected_key[16], expected_mask[22];
  unhex("000102030405060708090a0b0c0d0e0f", confirmation);
  // Independently generated with Python hashlib, explicitly lowercase ASCII.
  unhex("ac9a92210e6b18b49f0a5df179847733", expected_key);
  unhex("97867141b778406afc776b4fb817764bff6562cc1a89", expected_mask);
  assert(mesh_pairing_session(confirmation, device_key, mask));
  assert(memcmp(device_key, expected_key, 16) == 0 && memcmp(mask, expected_mask, 22) == 0);
  puts("PASS: official Mesh #6 vector, authenticated decoding, tamper rejection and session derivation");
}
