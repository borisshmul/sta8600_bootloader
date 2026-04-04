/*
 * secure.h — STA8600 Secure Boot Image Creation and Verification
 *
 * Handles:
 *   1. Signing firmware images with RSA-2048 (PKCS#1 v1.5)
 *   2. Optional AES-256-CBC encryption of the image body
 *   3. Secure header construction / parsing
 *   4. Key generation and management helpers
 *   5. OTP key provisioning record building
 *
 * Requires: OpenSSL >= 1.1.0
 */

#ifndef STA8600_SECURE_H
#define STA8600_SECURE_H

#include "protocol.h"
#include <stdint.h>
#include <stddef.h>

/* Returned by sb_verify_image() */
typedef enum {
    SB_OK                  = 0,
    SB_ERR_BAD_MAGIC       = -1,
    SB_ERR_BAD_SIGNATURE   = -2,
    SB_ERR_BAD_SHA256      = -3,
    SB_ERR_BAD_VERSION     = -4,
    SB_ERR_MALLOC          = -5,
    SB_ERR_OPENSSL         = -6,
} sb_status_t;

/*
 * sb_create_image
 *
 * Signs (and optionally encrypts) a raw firmware blob, prepending the
 * sta8600_secure_header_t.  The caller must free() *out_image.
 *
 * Parameters:
 *   firmware    — raw firmware bytes
 *   fw_len      — byte count
 *   load_addr   — where on the STA8600 the image will be loaded
 *   entry_point — address passed to CMD_GO after flash
 *   version     — monotonic version (anti-rollback)
 *   flags       — SBH_FLAG_* bitmask
 *   rsa_pem_key — PEM string of RSA-2048 private key
 *   aes_key     — 32-byte AES-256 key (may be NULL if !SBH_FLAG_ENCRYPTED)
 *   aes_iv      — 16-byte AES IV              (may be NULL if not encrypted)
 *   out_image   — receives malloc'd signed image
 *   out_len     — receives total length (header + body)
 *
 * Returns: SB_OK or negative error code
 */
sb_status_t sb_create_image(
    const uint8_t  *firmware,    size_t fw_len,
    uint32_t        load_addr,   uint32_t entry_point,
    uint32_t        version,     uint32_t flags,
    const char     *rsa_pem_key,
    const uint8_t  *aes_key,     const uint8_t *aes_iv,
    uint8_t       **out_image,   size_t *out_len
);

/*
 * sb_verify_image
 *
 * Verifies a signed image using the RSA-2048 public key.
 * Also validates the SHA-256 of the image body.
 *
 * If min_version > 0, rejects images with version < min_version.
 *
 * rsa_pem_pub — PEM string of RSA-2048 public key
 */
sb_status_t sb_verify_image(
    const uint8_t *image,    size_t image_len,
    const char    *rsa_pem_pub,
    uint32_t       min_version
);

/*
 * sb_decrypt_image_body
 *
 * Decrypts the body of an image that has SBH_FLAG_ENCRYPTED set.
 * Operates in-place on the image buffer (after the header).
 * Call AFTER sb_verify_image succeeds.
 */
sb_status_t sb_decrypt_image_body(
    uint8_t        *image,       size_t image_len,
    const uint8_t  *aes_key,
    const uint8_t  *aes_iv
);

/*
 * sb_keygen_rsa2048
 *
 * Generates a fresh RSA-2048 keypair, writes:
 *   priv_pem_path — private key in PEM format (chmod 600 recommended)
 *   pub_pem_path  — public  key in PEM format
 */
int sb_keygen_rsa2048(const char *priv_pem_path, const char *pub_pem_path);

/*
 * sb_build_otp_record
 *
 * Builds the 256-byte OTP provisioning record that must be written to
 * STA8600_OTP_BASE via bl_write_mem before locking secure boot.
 *
 * The record contains the SHA-256 hash of the RSA public key (the
 * "root of trust" hash, per ST Secure Boot Application Note).
 *
 * Layout (STA8600 OTP layout, offset from STA8600_OTP_BASE):
 *   0x00  Magic          4 bytes  0x53424F54 ("SBOT")
 *   0x04  Version        4 bytes  min anti-rollback version
 *   0x08  Flags          4 bytes  SBH_FLAG_* defaults for this device
 *   0x0C  Reserved       20 bytes
 *   0x20  PubKey Hash    32 bytes SHA-256(DER-encoded public key)
 *   0x40  HMAC Write Key 32 bytes key for CMD_SECURE_WRITE authentication
 *   0x60  Reserved       160 bytes (must be 0xFF for future OTP fields)
 *
 * out_record must point to a 256-byte buffer.
 */
int sb_build_otp_record(
    const char     *pub_pem_path,
    uint32_t        min_version,
    uint32_t        default_flags,
    const uint8_t  *hmac_write_key,   /* 32 bytes */
    uint8_t         out_record[256]
);

/* Human-readable status string */
const char *sb_status_str(sb_status_t s);

#endif /* STA8600_SECURE_H */
