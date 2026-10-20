#include "mesh_pairing_crypto.h"
#include "mbedtls/aes.h"
#include "mbedtls/ccm.h"
#include "mbedtls/sha256.h"

bool mesh_crypto_aes(const uint8_t key[16], const uint8_t input[16], uint8_t output[16]) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int err = mbedtls_aes_setkey_enc(&aes, key, 128);
  if (!err) err = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output);
  mbedtls_aes_free(&aes);
  return err == 0;
}
bool mesh_crypto_sha256(const uint8_t *data, size_t length, uint8_t output[32]) {
  return mbedtls_sha256(data, length, output, 0) == 0;
}
bool mesh_crypto_ccm(bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *input, size_t length, uint8_t *output, size_t mic_length) {
  if (!encrypt && length < mic_length) return false;
  mbedtls_ccm_context ccm;
  mbedtls_ccm_init(&ccm);
  int err = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (!err && encrypt)
    err = mbedtls_ccm_encrypt_and_tag(&ccm, length, nonce, 13, NULL, 0, input, output, output + length, mic_length);
  if (!err && !encrypt)
    err = mbedtls_ccm_auth_decrypt(&ccm, length - mic_length, nonce, 13, NULL, 0, input, output,
                                 input + length - mic_length, mic_length);
  mbedtls_ccm_free(&ccm);
  return err == 0;
}
