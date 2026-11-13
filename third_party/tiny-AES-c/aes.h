#ifndef _AES_H_
#define _AES_H_

#include <stdint.h>
#include <stddef.h>

// AES-128 のみを使用する設定
#define AES128 1

#define AES_BLOCKLEN 16 // ブロック長は AES 共通で 16 バイト

#define AES_KEYLEN 16       // AES-128: 鍵長 16 バイト
#define AES_keyExpSize 176  // 拡張鍵サイズ

#ifdef __cplusplus
extern "C" {
#endif

struct AES_ctx {
    uint8_t RoundKey[AES_keyExpSize];
    uint8_t Iv[AES_BLOCKLEN];
};

void AES_init_ctx(struct AES_ctx* ctx, const uint8_t* key);
void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);
void AES_ctx_set_iv(struct AES_ctx* ctx, const uint8_t* iv);

// CBC モード（buf の長さは 16 の倍数であること）
void AES_CBC_encrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);
void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

#ifdef __cplusplus
}
#endif

#endif // _AES_H_
