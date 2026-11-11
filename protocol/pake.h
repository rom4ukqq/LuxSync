#ifndef BRIGHTSYNC_PAKE_H
#define BRIGHTSYNC_PAKE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spake2_ctx_st BS_PAKE;
#define BS_PAKE_MESSAGE 32
#define BS_PAKE_NONCE 16
#define BS_PAKE_RESULT 96

/* Both roles accept the same eight ASCII PIN characters. */
BS_PAKE *bs_pake_start(int client, const uint8_t pin[8], uint8_t message[BS_PAKE_MESSAGE]);
void bs_pake_free(BS_PAKE *context);

/* One call per context. Result: 32-byte sync key, client proof, server proof. */
int bs_pake_finish(BS_PAKE *context, const uint8_t peer[BS_PAKE_MESSAGE],
                   const uint8_t nonce[BS_PAKE_NONCE], uint8_t result[BS_PAKE_RESULT]);
void bs_pake_clear(void *bytes, size_t size);
int bs_pake_equal(const uint8_t left[32], const uint8_t right[32]);

#ifdef __cplusplus
}
#endif
#endif
