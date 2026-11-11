#include "pake.h"
#include <openssl/crypto.h>
#include <openssl/curve25519.h>
#include <openssl/digest.h>
#include <openssl/hkdf.h>

BS_PAKE *bs_pake_start(int client, const uint8_t pin[8], uint8_t message[BS_PAKE_MESSAGE]) {
    static const uint8_t mac[] = "BrightSync macOS pairing v1";
    static const uint8_t windows[] = "BrightSync Windows pairing v1";
    SPAKE2_CTX *context = SPAKE2_CTX_new(client ? spake2_role_alice : spake2_role_bob,
        client ? mac : windows, client ? sizeof(mac) - 1 : sizeof(windows) - 1,
        client ? windows : mac, client ? sizeof(windows) - 1 : sizeof(mac) - 1);
    size_t size = 0;
    if (context && SPAKE2_generate_msg(context, message, &size, BS_PAKE_MESSAGE, pin, 8) &&
        size == BS_PAKE_MESSAGE) return context;
    SPAKE2_CTX_free(context);
    return NULL;
}

void bs_pake_free(BS_PAKE *context) { SPAKE2_CTX_free(context); }

int bs_pake_finish(BS_PAKE *context, const uint8_t peer[BS_PAKE_MESSAGE],
                   const uint8_t nonce[BS_PAKE_NONCE], uint8_t result[BS_PAKE_RESULT]) {
    static const uint8_t info[] = "BrightSync v1 sync key, client proof, server proof";
    uint8_t shared[SPAKE2_MAX_KEY_SIZE];
    size_t size = 0;
    int success = SPAKE2_process_msg(context, shared, &size, sizeof(shared), peer, BS_PAKE_MESSAGE) &&
        HKDF(result, BS_PAKE_RESULT, EVP_sha256(), shared, size, nonce, BS_PAKE_NONCE, info, sizeof(info) - 1);
    OPENSSL_cleanse(shared, sizeof(shared));
    return success;
}

void bs_pake_clear(void *bytes, size_t size) { OPENSSL_cleanse(bytes, size); }

int bs_pake_equal(const uint8_t left[32], const uint8_t right[32]) {
    return CRYPTO_memcmp(left, right, 32) == 0;
}
