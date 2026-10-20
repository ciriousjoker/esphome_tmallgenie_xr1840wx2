// Host-only crypto provider. Production links mesh_pairing_mbedtls.c instead.
#include "mesh_pairing_crypto.h"
#include <openssl/evp.h>
#include <string.h>
bool mesh_crypto_aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int n = 0; uint8_t buffer[32];
  bool ok = c && EVP_EncryptInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL) &&
      EVP_CIPHER_CTX_set_padding(c, 0) && EVP_EncryptUpdate(c, buffer, &n, in, 16);
  if (ok) memcpy(out, buffer, 16);
  EVP_CIPHER_CTX_free(c); return ok;
}
bool mesh_crypto_sha256(const uint8_t *data, size_t length, uint8_t output[32]) {
  unsigned n; return EVP_Digest(data, length, output, &n, EVP_sha256(), NULL) && n == 32;
}
bool mesh_crypto_ccm(bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *input, size_t length, uint8_t *output, size_t mic) {
  if (!encrypt && length < mic) return false;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int n; bool ok = false;
  if (encrypt) {
    ok = c && EVP_EncryptInit_ex(c, EVP_aes_128_ccm(), NULL, NULL, NULL) &&
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_IVLEN, 13, NULL) &&
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_TAG, mic, NULL) &&
        EVP_EncryptInit_ex(c, NULL, NULL, key, nonce) &&
        EVP_EncryptUpdate(c, NULL, &n, NULL, length) &&
        EVP_EncryptUpdate(c, output, &n, input, length) &&
        EVP_EncryptFinal_ex(c, output + n, &n) &&
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_GET_TAG, mic, output + length);
  } else {
    size_t plain = length - mic;
    ok = c && EVP_DecryptInit_ex(c, EVP_aes_128_ccm(), NULL, NULL, NULL) &&
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_IVLEN, 13, NULL) &&
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_CCM_SET_TAG, mic, (void *) (input + plain)) &&
        EVP_DecryptInit_ex(c, NULL, NULL, key, nonce) &&
        EVP_DecryptUpdate(c, NULL, &n, NULL, plain) &&
        EVP_DecryptUpdate(c, output, &n, input, plain);
  }
  EVP_CIPHER_CTX_free(c); return ok;
}
