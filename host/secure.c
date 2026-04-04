/*
 * secure.c — STA8600 Secure Boot Image Creation and Verification
 *
 * Build: gcc -DHAVE_OPENSSL -lcrypto secure.c protocol.c main.c -o sta_flash
 */

#include "secure.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <openssl/rand.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

static void print_openssl_errors(void)
{
    unsigned long e;
    while ((e = ERR_get_error()) != 0)
        fprintf(stderr, "[SSL] %s\n", ERR_error_string(e, NULL));
}

static int sha256_of(const uint8_t *data, size_t len, uint8_t out[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)
          && EVP_DigestUpdate(ctx, data, len);
    unsigned dlen = 32;
    ok = ok && EVP_DigestFinal_ex(ctx, out, &dlen);
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

static int rsa_sign(const uint8_t *hash32, const char *pem_priv,
                    uint8_t sig_out[256])
{
    BIO *bio = BIO_new_mem_buf(pem_priv, -1);
    if (!bio) return -1;

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) { print_openssl_errors(); return -1; }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    EVP_PKEY_free(pkey);
    if (!ctx) return -1;

    int rc = -1;
    if (EVP_PKEY_sign_init(ctx)                               <= 0) goto out;
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING)  <= 0) goto out;
    if (EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256())       <= 0) goto out;

    size_t sig_len = 256;
    if (EVP_PKEY_sign(ctx, sig_out, &sig_len, hash32, 32)      <= 0) goto out;
    if (sig_len != 256) goto out;
    rc = 0;
out:
    if (rc) print_openssl_errors();
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

static int rsa_verify(const uint8_t *hash32, const uint8_t sig[256],
                      const char *pem_pub)
{
    BIO *bio = BIO_new_mem_buf(pem_pub, -1);
    if (!bio) return -1;

    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) { print_openssl_errors(); return -1; }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    EVP_PKEY_free(pkey);
    if (!ctx) return -1;

    int rc = -1;
    if (EVP_PKEY_verify_init(ctx)                              <= 0) goto out;
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING)  <= 0) goto out;
    if (EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256())       <= 0) goto out;

    int r = EVP_PKEY_verify(ctx, sig, 256, hash32, 32);
    if (r == 1)  rc = 0;
    else         print_openssl_errors();
out:
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

static int aes256cbc(const uint8_t *in, size_t in_len,
                     const uint8_t key[32], const uint8_t iv[16],
                     uint8_t **out, size_t *out_len, int encrypt)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    *out = malloc(in_len + 16);   /* worst-case padding */
    if (!*out) { EVP_CIPHER_CTX_free(ctx); return -1; }

    int len1 = 0, len2 = 0, rc = -1;
    if (!EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, encrypt))
        goto out;
    /* Firmware images may not be padded — disable auto-padding for encrypt
       and set PKCS7 for decrypt to strip it. */
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    if (!EVP_CipherUpdate(ctx, *out, &len1, in, (int)in_len))         goto out;
    if (!EVP_CipherFinal_ex(ctx,  *out + len1, &len2))                goto out;
    *out_len = (size_t)(len1 + len2);
    rc = 0;
out:
    if (rc) { print_openssl_errors(); free(*out); *out = NULL; }
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

sb_status_t sb_create_image(
    const uint8_t *firmware, size_t fw_len,
    uint32_t load_addr,      uint32_t entry_point,
    uint32_t version,        uint32_t flags,
    const char *rsa_pem_key,
    const uint8_t *aes_key,  const uint8_t *aes_iv,
    uint8_t **out_image,     size_t *out_len)
{
    uint8_t *body     = NULL;
    size_t   body_len = 0;

    /* 1. Optionally encrypt the firmware body */
    if (flags & SBH_FLAG_ENCRYPTED) {
        if (!aes_key || !aes_iv) return SB_ERR_OPENSSL;
        if (aes256cbc(firmware, fw_len, aes_key, aes_iv,
                      &body, &body_len, 1) < 0)
            return SB_ERR_OPENSSL;
    } else {
        body = malloc(fw_len);
        if (!body) return SB_ERR_MALLOC;
        memcpy(body, firmware, fw_len);
        body_len = fw_len;
    }

    /* 2. Build header (signature field zeroed for now) */
    size_t total = SBH_HEADER_SIZE + body_len;
    uint8_t *image = calloc(1, total);
    if (!image) { free(body); return SB_ERR_MALLOC; }

    sta8600_secure_header_t *hdr = (sta8600_secure_header_t *)image;
    hdr->magic       = SBH_MAGIC;
    hdr->version     = version;
    hdr->load_addr   = load_addr;
    hdr->entry_point = entry_point;
    hdr->image_len   = (uint32_t)body_len;
    hdr->flags       = flags;

    /* 3. SHA-256 of the body */
    if (sha256_of(body, body_len, hdr->sha256) < 0) {
        free(body); free(image); return SB_ERR_OPENSSL;
    }
    memcpy(image + SBH_HEADER_SIZE, body, body_len);
    free(body);

    /* 4. Sign the header bytes [0x00..0x37] (everything before signature) */
    uint8_t header_hash[32];
    size_t  signable_len = offsetof(sta8600_secure_header_t, signature);
    if (sha256_of(image, signable_len, header_hash) < 0) {
        free(image); return SB_ERR_OPENSSL;
    }
    if (rsa_sign(header_hash, rsa_pem_key, hdr->signature) < 0) {
        free(image); return SB_ERR_OPENSSL;
    }

    *out_image = image;
    *out_len   = total;
    return SB_OK;
}

sb_status_t sb_verify_image(
    const uint8_t *image, size_t image_len,
    const char *rsa_pem_pub,
    uint32_t min_version)
{
    if (image_len < SBH_HEADER_SIZE) return SB_ERR_BAD_MAGIC;

    const sta8600_secure_header_t *hdr = (const sta8600_secure_header_t *)image;

    if (hdr->magic != SBH_MAGIC)           return SB_ERR_BAD_MAGIC;
    if (min_version && hdr->version < min_version) return SB_ERR_BAD_VERSION;

    /* 1. Verify RSA signature over header bytes [0x00..0x37] */
    uint8_t header_hash[32];
    size_t  signable_len = offsetof(sta8600_secure_header_t, signature);
    if (sha256_of(image, signable_len, header_hash) < 0)
        return SB_ERR_OPENSSL;
    if (rsa_verify(header_hash, hdr->signature, rsa_pem_pub) < 0)
        return SB_ERR_BAD_SIGNATURE;

    /* 2. Verify SHA-256 of body */
    if (image_len < SBH_HEADER_SIZE + hdr->image_len) return SB_ERR_BAD_SHA256;
    uint8_t body_hash[32];
    if (sha256_of(image + SBH_HEADER_SIZE, hdr->image_len, body_hash) < 0)
        return SB_ERR_OPENSSL;
    if (memcmp(body_hash, hdr->sha256, 32) != 0)
        return SB_ERR_BAD_SHA256;

    return SB_OK;
}

sb_status_t sb_decrypt_image_body(
    uint8_t *image, size_t image_len,
    const uint8_t *aes_key, const uint8_t *aes_iv)
{
    if (image_len < SBH_HEADER_SIZE) return SB_ERR_BAD_MAGIC;
    sta8600_secure_header_t *hdr = (sta8600_secure_header_t *)image;
    if (!(hdr->flags & SBH_FLAG_ENCRYPTED)) return SB_OK;  /* nothing to do */

    uint8_t *plain;
    size_t   plain_len;
    if (aes256cbc(image + SBH_HEADER_SIZE, hdr->image_len,
                  aes_key, aes_iv, &plain, &plain_len, 0) < 0)
        return SB_ERR_OPENSSL;

    memcpy(image + SBH_HEADER_SIZE, plain, plain_len);
    hdr->image_len = (uint32_t)plain_len;
    hdr->flags    &= ~SBH_FLAG_ENCRYPTED;
    free(plain);
    return SB_OK;
}

int sb_keygen_rsa2048(const char *priv_pem_path, const char *pub_pem_path)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!ctx) return -1;

    EVP_PKEY *pkey = NULL;
    int rc = -1;
    if (EVP_PKEY_keygen_init(ctx)                    <= 0) goto out;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048)  <= 0) goto out;
    if (EVP_PKEY_keygen(ctx, &pkey)                  <= 0) goto out;

    /* Write private key */
    FILE *f = fopen(priv_pem_path, "w");
    if (!f) goto out;
    PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
    fclose(f);

    /* Write public key */
    f = fopen(pub_pem_path, "w");
    if (!f) goto out;
    PEM_write_PUBKEY(f, pkey);
    fclose(f);

    printf("[SEC] RSA-2048 keypair written:\n  priv: %s\n  pub:  %s\n",
           priv_pem_path, pub_pem_path);
    rc = 0;
out:
    if (pkey) EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    if (rc) print_openssl_errors();
    return rc;
}

int sb_build_otp_record(
    const char *pub_pem_path,
    uint32_t min_version, uint32_t default_flags,
    const uint8_t *hmac_write_key,
    uint8_t out_record[256])
{
    memset(out_record, 0xFF, 256);

    /* Magic + fields at offsets 0x00–0x0B */
    uint32_t magic = 0x53424F54UL;  /* "SBOT" */
    memcpy(out_record + 0x00, &magic,         4);
    memcpy(out_record + 0x04, &min_version,   4);
    memcpy(out_record + 0x08, &default_flags, 4);
    memset(out_record + 0x0C, 0x00, 20);  /* reserved */

    /* SHA-256 of DER-encoded public key at offset 0x20 */
    FILE *f = fopen(pub_pem_path, "r");
    if (!f) { perror("fopen pub key"); return -1; }
    BIO *bio = BIO_new_fp(f, BIO_NOCLOSE);
    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    fclose(f);
    if (!pkey) { print_openssl_errors(); return -1; }

    /* Encode to DER */
    uint8_t *der = NULL;
    int der_len = i2d_PUBKEY(pkey, &der);
    EVP_PKEY_free(pkey);
    if (der_len <= 0) { print_openssl_errors(); return -1; }

    uint8_t pub_hash[32];
    if (sha256_of(der, (size_t)der_len, pub_hash) < 0) { OPENSSL_free(der); return -1; }
    OPENSSL_free(der);
    memcpy(out_record + 0x20, pub_hash, 32);

    /* HMAC write key at offset 0x40 */
    if (hmac_write_key)
        memcpy(out_record + 0x40, hmac_write_key, 32);
    else
        memset(out_record + 0x40, 0x00, 32);

    /* Remaining 0x60..0xFF stays 0xFF (OTP unwritten) */
    return 0;
}

const char *sb_status_str(sb_status_t s)
{
    switch (s) {
        case SB_OK:                return "OK";
        case SB_ERR_BAD_MAGIC:     return "Bad magic / not a secure image";
        case SB_ERR_BAD_SIGNATURE: return "RSA signature verification failed";
        case SB_ERR_BAD_SHA256:    return "SHA-256 body hash mismatch";
        case SB_ERR_BAD_VERSION:   return "Anti-rollback version too old";
        case SB_ERR_MALLOC:        return "Memory allocation failed";
        case SB_ERR_OPENSSL:       return "OpenSSL error";
        default:                   return "Unknown error";
    }
}

#else /* !HAVE_OPENSSL */

sb_status_t sb_create_image(const uint8_t *f, size_t fl,
    uint32_t la, uint32_t ep, uint32_t v, uint32_t flags,
    const char *k, const uint8_t *ak, const uint8_t *ai,
    uint8_t **oi, size_t *ol)
{
    (void)f;(void)fl;(void)la;(void)ep;(void)v;(void)flags;
    (void)k;(void)ak;(void)ai;(void)oi;(void)ol;
    fprintf(stderr, "Build with -DHAVE_OPENSSL -lcrypto\n");
    return SB_ERR_OPENSSL;
}
sb_status_t sb_verify_image(const uint8_t *i, size_t il,
    const char *p, uint32_t mv)
{ (void)i;(void)il;(void)p;(void)mv; return SB_ERR_OPENSSL; }
sb_status_t sb_decrypt_image_body(uint8_t *i, size_t il,
    const uint8_t *k, const uint8_t *v)
{ (void)i;(void)il;(void)k;(void)v; return SB_ERR_OPENSSL; }
int sb_keygen_rsa2048(const char *a, const char *b)
{ (void)a;(void)b; return -1; }
int sb_build_otp_record(const char *p, uint32_t v, uint32_t f,
    const uint8_t *k, uint8_t o[256])
{ (void)p;(void)v;(void)f;(void)k;(void)o; return -1; }
const char *sb_status_str(sb_status_t s)
{ (void)s; return "OpenSSL not compiled in"; }

#endif /* HAVE_OPENSSL */
