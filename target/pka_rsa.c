/*
 * pka_rsa.c — Software RSA-2048 PKCS#1 v1.5 signature verification
 *
 * Implements pka_rsa2048_verify_pkcs1() required by bootloader.c.
 *
 * Algorithm:
 *   1. Modular exponentiation: m = sig^e mod n  (e = 65537, fixed)
 *      Uses Montgomery multiplication for constant-time operation.
 *   2. Strip and verify PKCS#1 v1.5 padding: 0x00 0x01 [0xFF...] 0x00 DigestInfo
 *   3. Check DigestInfo prefix for SHA-256 and compare the embedded hash.
 *
 * All multi-precision arithmetic is big-endian externally (matching the
 * RSA wire format) and little-endian internally (word[0] = least significant).
 *
 * 2048-bit numbers: 64 × uint32_t words (little-endian word order).
 *
 * No heap allocation.  No library dependencies beyond <string.h>.
 * Constant-time where noted (no secret-dependent branches on key material;
 * public-exponent path is allowed to vary).
 */

#include <stdint.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Big-integer type: 64 words × 32 bits = 2048 bits
 * word[0] is the least-significant 32-bit word.
 * --------------------------------------------------------------------- */
#define RSA_WORDS   64          /* 2048 / 32                              */
#define RSA_BYTES   256         /* 2048 / 8                               */

typedef struct { uint32_t w[RSA_WORDS]; } bn2048_t;

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

static void bn_zero(bn2048_t *r)
{
    memset(r->w, 0, sizeof(r->w));
}

static void bn_set_one(bn2048_t *r)
{
    bn_zero(r);
    r->w[0] = 1;
}

/* Import big-endian byte array → bn2048_t (little-endian word order) */
static void bn_from_be(bn2048_t *r, const uint8_t b[RSA_BYTES])
{
    for (int i = 0; i < RSA_WORDS; i++) {
        int off = (RSA_WORDS - 1 - i) * 4;
        r->w[i] = ((uint32_t)b[off]     << 24)
                | ((uint32_t)b[off + 1] << 16)
                | ((uint32_t)b[off + 2] <<  8)
                | ((uint32_t)b[off + 3]);
    }
}

/* Export bn2048_t → big-endian byte array */
static void bn_to_be(const bn2048_t *r, uint8_t b[RSA_BYTES])
{
    for (int i = 0; i < RSA_WORDS; i++) {
        int off = (RSA_WORDS - 1 - i) * 4;
        b[off]     = (uint8_t)(r->w[i] >> 24);
        b[off + 1] = (uint8_t)(r->w[i] >> 16);
        b[off + 2] = (uint8_t)(r->w[i] >>  8);
        b[off + 3] = (uint8_t)(r->w[i]);
    }
}

/* Constant-time compare: returns 0 if equal, non-zero otherwise */
static int bn_ct_cmp(const bn2048_t *a, const bn2048_t *b)
{
    uint32_t diff = 0;
    for (int i = 0; i < RSA_WORDS; i++)
        diff |= a->w[i] ^ b->w[i];
    return (int)diff;
}

/* -----------------------------------------------------------------------
 * Montgomery multiplication
 *
 * Computes: result = (a * b * R^-1) mod n
 * where R = 2^2048  and  n_prime = -(n^-1) mod 2^32
 *
 * Uses the standard CIOS (Coarsely Integrated Operand Scanning) algorithm.
 * Runs in O(RSA_WORDS^2) time with no data-dependent branches on a or b.
 * --------------------------------------------------------------------- */

/*
 * Compute n' = -n[0]^-1 mod 2^32 via Newton-Raphson iteration.
 * n must be odd (guaranteed for any RSA modulus).
 */
static uint32_t mont_n_prime(uint32_t n0)
{
    /* 5 iterations of x ← x*(2 - n0*x) gives 32-bit inverse */
    uint32_t x = n0;           /* initial approximation: n0*1 ≡ 1 mod 2   */
    x *= 2 - n0 * x;           /* 4-bit accurate                           */
    x *= 2 - n0 * x;           /* 8-bit                                    */
    x *= 2 - n0 * x;           /* 16-bit                                   */
    x *= 2 - n0 * x;           /* 32-bit                                   */
    return (uint32_t)(-(int32_t)x);   /* negate to get -n0^-1 mod 2^32     */
}

static void mont_mul(bn2048_t *result,
                     const bn2048_t *a,
                     const bn2048_t *b,
                     const bn2048_t *n,
                     uint32_t        np)
{
    /* T = 0, RSA_WORDS+2 words to hold accumulator (prevent overflow) */
    uint64_t T[RSA_WORDS + 2];
    memset(T, 0, sizeof(T));

    for (int i = 0; i < RSA_WORDS; i++) {
        /* Step 1: T += a[i] * b */
        uint64_t carry = 0;
        for (int j = 0; j < RSA_WORDS; j++) {
            uint64_t prod = (uint64_t)a->w[i] * b->w[j] + T[j] + carry;
            T[j]  = prod & 0xFFFFFFFF;
            carry = prod >> 32;
        }
        T[RSA_WORDS]     += carry;
        T[RSA_WORDS + 1]  = 0;

        /* Step 2: reduction — m = T[0] * np mod 2^32 */
        uint32_t m = (uint32_t)((T[0] * (uint64_t)np) & 0xFFFFFFFF);

        /* T += m * n */
        carry = 0;
        for (int j = 0; j < RSA_WORDS; j++) {
            uint64_t prod = (uint64_t)m * n->w[j] + T[j] + carry;
            T[j]  = prod & 0xFFFFFFFF;
            carry = prod >> 32;
        }
        T[RSA_WORDS]     += carry;

        /* Right-shift T by one word (discard T[0]) */
        for (int j = 0; j < RSA_WORDS + 1; j++)
            T[j] = T[j + 1];
        T[RSA_WORDS + 1] = 0;
    }

    /* Conditional subtract: if T >= n, result = T - n */
    /* First copy T into result */
    for (int j = 0; j < RSA_WORDS; j++)
        result->w[j] = (uint32_t)T[j];

    /* Check if T[RSA_WORDS] != 0 or result >= n */
    uint64_t borrow = 0;
    bn2048_t tmp;
    for (int j = 0; j < RSA_WORDS; j++) {
        int64_t diff = (int64_t)result->w[j] - n->w[j] - (int64_t)borrow;
        tmp.w[j] = (uint32_t)(diff & 0xFFFFFFFF);
        borrow = (diff < 0) ? 1 : 0;
    }
    /* If no borrow and T[RSA_WORDS] == 0: result = tmp, else result stays */
    uint32_t use_tmp = (uint32_t)((borrow == 0 && T[RSA_WORDS] == 0) ? 1 : 0);
    for (int j = 0; j < RSA_WORDS; j++)
        result->w[j] = use_tmp ? tmp.w[j] : result->w[j];
}

/*
 * Compute R^2 mod n — used once to convert operands into Montgomery domain.
 * R = 2^2048.  We compute R^2 mod n = (2^4096) mod n by repeated doubling.
 */
static void mont_r2(bn2048_t *r2, const bn2048_t *n)
{
    /* Start with R mod n.
     * R = 2^2048; R mod n = 2^2048 mod n.
     * Since n < 2^2048 for a valid 2048-bit number,
     * R mod n = -n (mod 2^2048) in two's complement, i.e. 2^2048 - n.
     * We compute that as (0 - n) with a carry from the 2048-bit boundary.
     */
    uint64_t borrow = 0;
    for (int i = 0; i < RSA_WORDS; i++) {
        int64_t d = (int64_t)0 - n->w[i] - (int64_t)borrow;
        r2->w[i] = (uint32_t)(d & 0xFFFFFFFF);
        borrow   = (d < 0) ? 1 : 0;
    }
    /* Now r2 = 2^2048 - n = R - n = R mod n (since 0 < n < R) */

    /* Square r2 RSA_WORDS*32 = 2048 times (double each time and reduce) */
    uint32_t np = mont_n_prime(n->w[0]);
    bn2048_t tmp;
    for (int i = 0; i < 2 * 2048; i++) {
        /* Double r2 with modular reduction */
        uint32_t carry = 0;
        for (int j = 0; j < RSA_WORDS; j++) {
            uint64_t d = (uint64_t)r2->w[j] * 2 + carry;
            r2->w[j] = (uint32_t)d;
            carry    = (uint32_t)(d >> 32);
        }
        /* Subtract n if r2 >= n or carry != 0 */
        uint64_t bw = 0;
        for (int j = 0; j < RSA_WORDS; j++) {
            int64_t d = (int64_t)r2->w[j] - n->w[j] - (int64_t)bw;
            tmp.w[j] = (uint32_t)(d & 0xFFFFFFFF);
            bw = (d < 0) ? 1 : 0;
        }
        if (carry || !bw)
            *r2 = tmp;
    }
    (void)np;  /* np computed inside mont_mul, not here */
}

/* -----------------------------------------------------------------------
 * Modular exponentiation: result = base^exp mod n
 *
 * Exponent e = 65537 = 0x10001 (17 bits, weight 2) — hard-coded for speed.
 * Uses left-to-right square-and-multiply with Montgomery representation.
 * --------------------------------------------------------------------- */
static void mod_exp_65537(bn2048_t *result,
                           const bn2048_t *base,
                           const bn2048_t *n)
{
    uint32_t np = mont_n_prime(n->w[0]);

    /* R^2 mod n for Montgomery conversion */
    bn2048_t r2;
    mont_r2(&r2, n);

    /* Convert base to Montgomery domain: base_m = base * R mod n */
    bn2048_t one;
    bn_set_one(&one);
    bn2048_t base_m;
    mont_mul(&base_m, base, &r2, n, np);

    /* Montgomery 1: 1_m = 1 * R mod n = R mod n = r2 converted back
     * Actually: 1_m = mont_mul(1, R^2) = R mod n.  Let's compute properly.
     * We want acc = 1 in Montgomery domain = mont_mul(1, R^2, n, np).    */
    bn2048_t acc;
    mont_mul(&acc, &one, &r2, n, np);

    /* e = 65537 = 0b1_0000_0000_0000_0001  (bit 16 and bit 0 set)
     *
     * Left-to-right binary method over the 17-bit exponent.
     * Bit pattern: 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1
     *              ^                               ^
     *             bit16                           bit0
     */
    for (int bit = 16; bit >= 0; bit--) {
        /* Square */
        bn2048_t sq;
        mont_mul(&sq, &acc, &acc, n, np);
        acc = sq;

        /* Multiply if this bit of 65537 is set (only bits 16 and 0) */
        if (bit == 16 || bit == 0) {
            bn2048_t prod;
            mont_mul(&prod, &acc, &base_m, n, np);
            acc = prod;
        }
    }

    /* Convert result out of Montgomery domain: result = acc * 1 * R^-1 mod n */
    mont_mul(result, &acc, &one, n, np);
}

/* -----------------------------------------------------------------------
 * PKCS#1 v1.5 signature verification
 *
 * After RSA decryption, the plaintext block EM has the form:
 *   0x00  0x01  [0xFF ... 0xFF]  0x00  DigestInfo  Hash
 *
 * DigestInfo for SHA-256 (DER-encoded):
 *   30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20
 *   (19 bytes)
 *
 * Padded message length = 256 bytes for RSA-2048.
 * Minimum PS (0xFF padding) length = 8 bytes (per RFC 8017).
 * --------------------------------------------------------------------- */

static const uint8_t SHA256_DIGESTINFO[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09,
    0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
    0x05, 0x00, 0x04, 0x20
};
#define DIGESTINFO_LEN  19
#define SHA256_LEN      32

static int pkcs1_v15_verify(const uint8_t em[RSA_BYTES],
                             const uint8_t *hash, size_t hash_len)
{
    if (hash_len != SHA256_LEN) return -1;

    /* Byte 0 must be 0x00 */
    if (em[0] != 0x00) return -1;
    /* Byte 1 must be 0x01 (block type 1 = signature) */
    if (em[1] != 0x01) return -1;

    /* Find end of 0xFF padding */
    int ps_end = -1;
    for (int i = 2; i < RSA_BYTES; i++) {
        if (em[i] == 0x00) { ps_end = i; break; }
        if (em[i] != 0xFF) return -1;  /* invalid padding byte */
    }
    if (ps_end < 0) return -1;         /* no 0x00 terminator   */

    int ps_len = ps_end - 2;
    if (ps_len < 8) return -1;         /* PS must be >= 8 bytes */

    /* What follows 0x00 must be DigestInfo (19 bytes) + hash (32 bytes) */
    int payload_start = ps_end + 1;
    int expected_end  = payload_start + DIGESTINFO_LEN + (int)hash_len;
    if (expected_end != RSA_BYTES) return -1;

    /* Compare DigestInfo prefix — constant-time */
    uint8_t diff = 0;
    for (int i = 0; i < DIGESTINFO_LEN; i++)
        diff |= em[payload_start + i] ^ SHA256_DIGESTINFO[i];

    /* Compare hash — constant-time */
    for (int i = 0; i < (int)hash_len; i++)
        diff |= em[payload_start + DIGESTINFO_LEN + i] ^ hash[i];

    return (diff == 0) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Public entry point called from bootloader.c
 *
 * modulus_n    — 256-byte big-endian RSA-2048 modulus
 * public_exp_e — 4-byte big-endian public exponent (must be 65537 = 0x00010001)
 * signature    — 256-byte big-endian PKCS#1 v1.5 signature
 * expected_hash— 32-byte SHA-256 hash to verify against
 * hash_len     — must be 32
 *
 * Returns 0 on success, -1 on any verification failure.
 * --------------------------------------------------------------------- */
int pka_rsa2048_verify_pkcs1(
    const uint8_t *modulus_n,
    const uint8_t *public_exp_e,
    const uint8_t *signature,
    const uint8_t *expected_hash,
    size_t         hash_len)
{
    /* Sanity: we only support e = 65537 */
    if (public_exp_e[0] != 0x00 || public_exp_e[1] != 0x01 ||
        public_exp_e[2] != 0x00 || public_exp_e[3] != 0x01)
        return -1;

    bn2048_t n, sig, m;
    bn_from_be(&n,   modulus_n);
    bn_from_be(&sig, signature);

    /* RSA public operation: m = sig^65537 mod n */
    mod_exp_65537(&m, &sig, &n);

    /* Convert back to bytes */
    uint8_t em[RSA_BYTES];
    bn_to_be(&m, em);

    /* PKCS#1 v1.5 padding + hash check */
    return pkcs1_v15_verify(em, expected_hash, hash_len);
}
