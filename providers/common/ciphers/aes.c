/*
 * Copyright 2019 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <string.h>
#include <openssl/crypto.h>
#include <openssl/core_numbers.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include "internal/cryptlib.h"
#include "internal/provider_algs.h"
#include "ciphers_locl.h"

static void PROV_AES_KEY_generic_init(PROV_AES_KEY *ctx,
                                      const unsigned char *iv,
                                      int enc)
{
    if (iv != NULL)
        memcpy(ctx->iv, iv, AES_BLOCK_SIZE);
    ctx->enc = enc;
}

static int aes_einit(void *vctx, const unsigned char *key,
                           const unsigned char *iv)
{
    PROV_AES_KEY *ctx = (PROV_AES_KEY *)vctx;

    PROV_AES_KEY_generic_init(ctx, iv, 1);
    if (key != NULL)
        return ctx->ciph->init(ctx, key, ctx->keylen);

    return 1;
}

static int aes_dinit(void *vctx, const unsigned char *key,
                     const unsigned char *iv)
{
    PROV_AES_KEY *ctx = (PROV_AES_KEY *)vctx;

    PROV_AES_KEY_generic_init(ctx, iv, 0);
    if (key != NULL)
        return ctx->ciph->init(ctx, key, ctx->keylen);

    return 1;
}

static int aes_update(void *vctx, unsigned char *out, size_t *outl,
                      const unsigned char *in, size_t inl)
{
    PROV_AES_KEY *ctx = (PROV_AES_KEY *)vctx;
    size_t nextblocks = fillblock(ctx->buf, &ctx->bufsz, AES_BLOCK_SIZE, &in,
                                  &inl);
    size_t outlint = 0;

    /*
     * If we're decrypting and we end an update on a block boundary we hold
     * the last block back in case this is the last update call and the last
     * block is padded.
     */
    if (ctx->bufsz == AES_BLOCK_SIZE
            && (ctx->enc || inl > 0 || !ctx->pad)) {
        if (!ctx->ciph->cipher(ctx, out, ctx->buf, AES_BLOCK_SIZE))
            return 0;
        ctx->bufsz = 0;
        outlint = AES_BLOCK_SIZE;
        out += AES_BLOCK_SIZE;
    }
    if (nextblocks > 0) {
        if (!ctx->enc && ctx->pad && nextblocks == inl) {
            if (!ossl_assert(inl >= AES_BLOCK_SIZE))
                return 0;
            nextblocks -= AES_BLOCK_SIZE;
        }
        if (!ctx->ciph->cipher(ctx, out, in, nextblocks))
            return 0;
        in += nextblocks;
        inl -= nextblocks;
        outlint += nextblocks;
    }
    if (!trailingdata(ctx->buf, &ctx->bufsz, AES_BLOCK_SIZE, &in, &inl))
        return 0;

    *outl = outlint;
    return inl == 0;
}

static int aes_final(void *vctx, unsigned char *out, size_t *outl)
{
    PROV_AES_KEY *ctx = (PROV_AES_KEY *)vctx;

    if (ctx->enc) {
        if (ctx->pad) {
            padblock(ctx->buf, &ctx->bufsz, AES_BLOCK_SIZE);
        } else if (ctx->bufsz == 0) {
            *outl = 0;
            return 1;
        } else if (ctx->bufsz != AES_BLOCK_SIZE) {
            /* TODO(3.0): What is the correct error code here? */
            return 0;
        }

        if (!ctx->ciph->cipher(ctx, out, ctx->buf, AES_BLOCK_SIZE))
            return 0;
        ctx->bufsz = 0;
        *outl = AES_BLOCK_SIZE;
        return 1;
    }

    /* Decrypting */
    /* TODO(3.0): What's the correct error here */
    if (ctx->bufsz != AES_BLOCK_SIZE) {
        if (ctx->bufsz == 0 && !ctx->pad) {
            *outl = 0;
            return 1;
        }
        return 0;
    }

    if (!ctx->ciph->cipher(ctx, ctx->buf, ctx->buf, AES_BLOCK_SIZE))
        return 0;

    /* TODO(3.0): What is the correct error here */
    if (ctx->pad && !unpadblock(ctx->buf, &ctx->bufsz, AES_BLOCK_SIZE))
        return 0;

    memcpy(out, ctx->buf, ctx->bufsz);
    *outl = ctx->bufsz;
    ctx->bufsz = 0;
    return 1;
}

static void *aes_256_ecb_newctx(void)
{
    PROV_AES_KEY *ctx = OPENSSL_zalloc(sizeof(*ctx));

    ctx->pad = 1;
    ctx->keylen = 256 / 8;
    ctx->ciph = PROV_AES_CIPHER_ecb();
    ctx->mode = EVP_CIPH_ECB_MODE;
    return ctx;
}

static void *aes_192_ecb_newctx(void)
{
    PROV_AES_KEY *ctx = OPENSSL_zalloc(sizeof(*ctx));

    ctx->pad = 1;
    ctx->keylen = 192 / 8;
    ctx->ciph = PROV_AES_CIPHER_ecb();
    ctx->mode = EVP_CIPH_ECB_MODE;
    return ctx;
}

static void *aes_128_ecb_newctx(void)
{
    PROV_AES_KEY *ctx = OPENSSL_zalloc(sizeof(*ctx));

    ctx->pad = 1;
    ctx->keylen = 128 / 8;
    ctx->ciph = PROV_AES_CIPHER_ecb();
    ctx->mode = EVP_CIPH_ECB_MODE;
    return ctx;
}

static void aes_freectx(void *vctx)
{
    PROV_AES_KEY *ctx = (PROV_AES_KEY *)vctx;

    OPENSSL_clear_free(ctx,  sizeof(*ctx));
}

static void *aes_dupctx(void *ctx)
{
    PROV_AES_KEY *in = (PROV_AES_KEY *)ctx;
    PROV_AES_KEY *ret = OPENSSL_malloc(sizeof(*ret));

    *ret = *in;

    return ret;
}

static size_t key_length_256(void)
{
    return 256 / 8;
}

static size_t key_length_192(void)
{
    return 192 / 8;
}

static size_t key_length_128(void)
{
    return 128 / 8;
}

static int aes_get_params(void *vctx, const OSSL_PARAM params[])
{
    PROV_AES_KEY *ctx = (PROV_AES_KEY *)vctx;
    const OSSL_PARAM *p;

    p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_PADDING);
    if (p != NULL && !OSSL_PARAM_set_uint(p, ctx->pad))
        return 0;

    return 1;
}

static int aes_set_params(void *vctx, const OSSL_PARAM params[])
{
    PROV_AES_KEY *ctx = (PROV_AES_KEY *)vctx;
    const OSSL_PARAM *p;

    p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_PADDING);
    if (p != NULL) {
        int pad;

        if (!OSSL_PARAM_get_int(p, &pad))
            return 0;
        ctx->pad = pad ? 1 : 0;
    }
    return 1;
}

const OSSL_DISPATCH aes256ecb_functions[] = {
    { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void))aes_256_ecb_newctx },
    { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void))aes_einit },
    { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void))aes_dinit },
    { OSSL_FUNC_CIPHER_UPDATE, (void (*)(void))aes_update },
    { OSSL_FUNC_CIPHER_FINAL, (void (*)(void))aes_final },
    { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void))aes_freectx },
    { OSSL_FUNC_CIPHER_DUPCTX, (void (*)(void))aes_dupctx },
    { OSSL_FUNC_CIPHER_KEY_LENGTH, (void (*)(void))key_length_256 },
    { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void))aes_get_params },
    { OSSL_FUNC_CIPHER_SET_PARAMS, (void (*)(void))aes_set_params },
    { 0, NULL }
};

const OSSL_DISPATCH aes192ecb_functions[] = {
    { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void))aes_192_ecb_newctx },
    { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void))aes_einit },
    { OSSL_FUNC_CIPHER_ENCRYPT_UPDATE, (void (*)(void))aes_update },
    { OSSL_FUNC_CIPHER_ENCRYPT_FINAL, (void (*)(void))aes_efinal },
    { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void))aes_dinit },
    { OSSL_FUNC_CIPHER_DECRYPT_UPDATE, (void (*)(void))aes_update },
    { OSSL_FUNC_CIPHER_DECRYPT_FINAL, (void (*)(void))aes_dfinal },
    { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void))aes_freectx },
    { OSSL_FUNC_CIPHER_DUPCTX, (void (*)(void))aes_dupctx },
    { OSSL_FUNC_CIPHER_KEY_LENGTH, (void (*)(void))key_length_192 },
    { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void))aes_get_params },
    { OSSL_FUNC_CIPHER_SET_PARAMS, (void (*)(void))aes_set_params },
    { 0, NULL }
};

const OSSL_DISPATCH aes128ecb_functions[] = {
    { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void))aes_128_ecb_newctx },
    { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void))aes_einit },
    { OSSL_FUNC_CIPHER_ENCRYPT_UPDATE, (void (*)(void))aes_update },
    { OSSL_FUNC_CIPHER_ENCRYPT_FINAL, (void (*)(void))aes_efinal },
    { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void))aes_dinit },
    { OSSL_FUNC_CIPHER_DECRYPT_UPDATE, (void (*)(void))aes_update },
    { OSSL_FUNC_CIPHER_DECRYPT_FINAL, (void (*)(void))aes_dfinal },
    { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void))aes_freectx },
    { OSSL_FUNC_CIPHER_DUPCTX, (void (*)(void))aes_dupctx },
    { OSSL_FUNC_CIPHER_KEY_LENGTH, (void (*)(void))key_length_128 },
    { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void))aes_get_params },
    { OSSL_FUNC_CIPHER_SET_PARAMS, (void (*)(void))aes_set_params },
    { 0, NULL }
};