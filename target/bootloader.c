/*
 * bootloader.c — STA8600 Second-Stage Bootloader
 *
 * Responsibilities:
 *   1. Verify the application image (secure header + RSA signature + SHA-256)
 *   2. Decrypt the image body if SBH_FLAG_ENCRYPTED is set
 *   3. Enforce anti-rollback via the OTP version field
 *   4. Optionally lock JTAG before jumping if SBH_FLAG_DEBUG_LOCK is set
 *   5. Jump to the verified application entry point
 *
 * This runs on the STA8600 Cortex-A7 core after the ROM bootloader
 * has loaded it into SYSRAM or internal flash.
 *
 * Hardware-specific notes:
 *   - UART0 base: 0x40008000 (console output at 115200 8N1)
 *   - WDT base:   0x40001000 (kick every 500 ms while loading)
 *   - DBGDSCR:    accessed via CP14 c1 c0 — set bit 15 to disable JTAG
 *   - OTP base:   0x1FFF0000
 *
 * Crypto:
 *   This file provides a minimal SHA-256 and RSA-2048 PKCS#1 v1.5
 *   verify-only implementation.  For production, replace with ST's
 *   hardware crypto accelerator (HASH/CRYP peripheral).
 */

#include <stdint.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Forward-declarations from secure header (shared with host tool)
 * --------------------------------------------------------------------- */

#define SBH_MAGIC               0x53544131UL
#define SBH_HEADER_SIZE         0x138
#define SBH_FLAG_ENCRYPTED      (1u << 0)
#define SBH_FLAG_ROLLBACK_CHECK (1u << 1)
#define SBH_FLAG_DEBUG_LOCK     (1u << 2)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t load_addr;
    uint32_t entry_point;
    uint32_t image_len;
    uint32_t flags;
    uint8_t  sha256[32];
    uint8_t  signature[256];
} secure_header_t;

/* -----------------------------------------------------------------------
 * Hardware register access
 * --------------------------------------------------------------------- */

#define REG32(addr)   (*((volatile uint32_t *)(addr)))

/* UART0 (PL011) */
#define UART0_BASE    0x40008000UL
#define UART_DR       REG32(UART0_BASE + 0x00)
#define UART_FR       REG32(UART0_BASE + 0x18)
#define UART_FR_TXFF  (1u << 5)   /* TX FIFO full */

/* Watchdog */
#define WDT_BASE      0x40001000UL
#define WDT_LOAD      REG32(WDT_BASE + 0x00)
#define WDT_CTRL      REG32(WDT_BASE + 0x08)
#define WDT_INTCLR    REG32(WDT_BASE + 0x0C)
#define WDT_KICK_VAL  0x00100000UL   /* ~500 ms at 32 MHz WDT clock */

/* OTP */
#define OTP_BASE      0x1FFF0000UL
#define OTP_MAGIC     REG32(OTP_BASE + 0x00)
#define OTP_MINVER    REG32(OTP_BASE + 0x04)
#define OTP_PUBHASH   ((const uint8_t *)(OTP_BASE + 0x20))
#define OTP_SBOT_MAGIC 0x53424F54UL   /* "SBOT" */

/* -----------------------------------------------------------------------
 * TOC (Table of Contents) — mirrors the host-side definition
 * --------------------------------------------------------------------- */
#define TOC_BASE            0x08000000UL
#define TOC_MAGIC           0x54434F43UL
#define TOC_TERMINATOR      0xFFFFFFFFUL
#define TOC_TYPE_FSBL       0x00000001UL
#define TOC_TYPE_SSBL       0x00000002UL

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flash_offset;
    uint32_t load_addr;
    uint32_t entry_point;
    uint32_t image_size;
    uint32_t flags;
    uint32_t crc32;
    uint32_t reserved;
} toc_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t   magic;
    uint32_t   version;
    uint32_t   num_entries;
    uint32_t   header_crc32;
    uint8_t    reserved[48];
    toc_entry_t entries[8];
} toc_t;

/* CRC-32 for TOC validation */
static uint32_t toc_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* Locate the SSBL entry in the TOC.  Returns NULL if TOC is invalid. */
static const toc_entry_t *toc_find_ssbl(void)
{
    const toc_t *toc = (const toc_t *)TOC_BASE;
    if (toc->magic != TOC_MAGIC) {
        uart_puts("[BL2] TOC magic invalid\n");
        return NULL;
    }
    /* Validate header CRC */
    uint32_t computed = toc_crc32((const uint8_t *)toc,
                                   offsetof(toc_t, header_crc32));
    if (computed != toc->header_crc32) {
        uart_puts("[BL2] TOC header CRC mismatch\n");
        return NULL;
    }
    for (uint32_t i = 0; i < toc->num_entries && i < 8; i++) {
        const toc_entry_t *e = &toc->entries[i];
        if (e->type == TOC_TERMINATOR) break;
        if (e->type == TOC_TYPE_SSBL)  return e;
    }
    uart_puts("[BL2] SSBL entry not found in TOC\n");
    return NULL;
}

/* -----------------------------------------------------------------------
 * Console UART helpers
 * --------------------------------------------------------------------- */

static void uart_putc(char c)
{
    while (UART_FR & UART_FR_TXFF) ;
    UART_DR = (uint32_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_puthex32(uint32_t v)
{
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        uart_putc(hex[(v >> i) & 0xF]);
}

/* -----------------------------------------------------------------------
 * Watchdog kick
 * --------------------------------------------------------------------- */

static void wdt_kick(void)
{
    WDT_INTCLR = 1;
    WDT_LOAD   = WDT_KICK_VAL;
}

/* -----------------------------------------------------------------------
 * Minimal SHA-256
 * --------------------------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint32_t count[2];
    uint8_t  buf[64];
} sha256_ctx_t;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z)   (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z)  (((x)&(y))^((x)&(z))^((y)&(z)))
#define SIG0(x)     (ROTR32(x,2)^ROTR32(x,13)^ROTR32(x,22))
#define SIG1(x)     (ROTR32(x,6)^ROTR32(x,11)^ROTR32(x,25))
#define sig0(x)     (ROTR32(x,7)^ROTR32(x,18)^((x)>>3))
#define sig1(x)     (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t *block)
{
    uint32_t W[64], a,b,c,d,e,f,g,h,T1,T2;
    for (int i=0;i<16;i++)
        W[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
             ((uint32_t)block[i*4+2]<<8)|(uint32_t)block[i*4+3];
    for (int i=16;i<64;i++)
        W[i]=sig1(W[i-2])+W[i-7]+sig0(W[i-15])+W[i-16];

    a=ctx->state[0];b=ctx->state[1];c=ctx->state[2];d=ctx->state[3];
    e=ctx->state[4];f=ctx->state[5];g=ctx->state[6];h=ctx->state[7];

    for(int i=0;i<64;i++){
        T1=h+SIG1(e)+CH(e,f,g)+K256[i]+W[i];
        T2=SIG0(a)+MAJ(a,b,c);
        h=g;g=f;f=e;e=d+T1;d=c;c=b;b=a;a=T1+T2;
    }
    ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;
    ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0]=0x6a09e667;ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372;ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f;ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab;ctx->state[7]=0x5be0cd19;
    ctx->count[0]=ctx->count[1]=0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    uint32_t lo = ctx->count[0];
    ctx->count[0] += (uint32_t)(len << 3);
    if (ctx->count[0] < lo) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    size_t i = 0, partial = (lo >> 3) & 63;
    if (partial) {
        size_t space = 64 - partial;
        size_t fill  = len < space ? len : space;
        memcpy(ctx->buf + partial, data, fill);
        i = fill;
        if (partial + fill < 64) return;
        sha256_transform(ctx, ctx->buf);
    }
    for (; i + 63 < len; i += 64)
        sha256_transform(ctx, data + i);
    if (i < len)
        memcpy(ctx->buf, data + i, len - i);
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32])
{
    uint8_t pad[64] = {0x80};
    uint32_t lo = ctx->count[0], hi = ctx->count[1];
    size_t partial = (lo >> 3) & 63;
    size_t padlen  = partial < 56 ? 56 - partial : 120 - partial;
    sha256_update(ctx, pad, padlen);
    uint8_t bits[8] = {
        hi>>24,hi>>16,hi>>8,hi,lo>>24,lo>>16,lo>>8,lo
    };
    sha256_update(ctx, bits, 8);
    for (int i=0;i<8;i++){
        digest[i*4]   = ctx->state[i]>>24;
        digest[i*4+1] = ctx->state[i]>>16;
        digest[i*4+2] = ctx->state[i]>>8;
        digest[i*4+3] = ctx->state[i];
    }
}

static void sha256(const uint8_t *data, size_t len, uint8_t digest[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

/* -----------------------------------------------------------------------
 * Constant-time memory compare (prevent timing side-channels)
 * --------------------------------------------------------------------- */

static int ct_memcmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return diff;   /* 0 = equal */
}

/* -----------------------------------------------------------------------
 * RSA-2048 PKCS#1 v1.5 signature verification
 *
 * Implemented in pka_rsa.c — full software Montgomery multiplication,
 * square-and-multiply modular exponentiation (e=65537), and PKCS#1 v1.5
 * padding verification.  No external library required.
 *
 * To use the STA8600 hardware PKA accelerator instead, remove pka_rsa.c
 * from the build and re-implement this function using the CRYP/PKA
 * peripheral registers per the STA8600 Reference Manual chapter on PKA.
 * --------------------------------------------------------------------- */
int pka_rsa2048_verify_pkcs1(
    const uint8_t *modulus_n,
    const uint8_t *public_exp_e,
    const uint8_t *signature,
    const uint8_t *expected_hash,
    size_t         hash_len
);

/* Embedded RSA-2048 public key (replace at key-provisioning time) */
static const uint8_t rsa_modulus[256] = {
    /* PLACEHOLDER: replace with actual 2048-bit modulus from pub.pem */
    0xFF, [1 ... 254] = 0xFF, 0xFF
};
static const uint8_t rsa_public_exp[4] = { 0x00, 0x01, 0x00, 0x01 }; /* 65537 */

/* Embedded SHA-256(DER(pubkey)) — must match OTP at offset 0x20 */
static const uint8_t embedded_pubkey_hash[32] = {
    /* PLACEHOLDER: fill in during secure boot provisioning */
    0x00
};

static int verify_rsa_signature(const uint8_t *header_hash,
                                 const uint8_t *signature)
{
    /* Step 1: Verify embedded key hash matches OTP */
    if (OTP_MAGIC == OTP_SBOT_MAGIC) {
        if (ct_memcmp(OTP_PUBHASH, embedded_pubkey_hash, 32) != 0) {
            uart_puts("[BL2] FATAL: Public key hash mismatch vs OTP!\n");
            return -1;
        }
    }
    /* Step 2: RSA verify (call PKA accelerator) */
    return pka_rsa2048_verify_pkcs1(
        rsa_modulus, rsa_public_exp, signature, header_hash, 32);
}

/* -----------------------------------------------------------------------
 * JTAG / debug port lockout
 * (STA8600: clear DBGDSCR.HDBGEN via CP14 to disable halting debug)
 * --------------------------------------------------------------------- */

static void lock_debug_port(void)
{
    uint32_t dbgdscr;
    __asm__ volatile (
        "mrc p14, 0, %0, c0, c1, 0\n\t"   /* Read DBGDSCR         */
        "bic %0, %0, #(1 << 14)\n\t"       /* Clear HDBGEN         */
        "bic %0, %0, #(1 << 15)\n\t"       /* Clear MDBGEN         */
        "mcr p14, 0, %0, c0, c1, 0\n\t"   /* Write back DBGDSCR   */
        : "=r"(dbgdscr)
    );
    uart_puts("[BL2] Debug port locked.\n");
}

/* -----------------------------------------------------------------------
 * Jump to application
 * --------------------------------------------------------------------- */

typedef void (*app_entry_t)(void) __attribute__((noreturn));

static void __attribute__((noreturn)) jump_to_app(uint32_t entry)
{
    uart_puts("[BL2] Jumping to application at ");
    uart_puthex32(entry);
    uart_putc('\n');

    /* Flush and invalidate caches before handoff */
    __asm__ volatile (
        "dsb\n\t"
        "isb\n\t"
        "mcr p15, 0, %0, c7, c5, 0\n\t"   /* ICIALLU   */
        "mcr p15, 0, %0, c7, c5, 6\n\t"   /* BPIALL    */
        "dsb\n\t"
        "isb\n\t"
        :: "r"(0)
    );

    app_entry_t entry_fn = (app_entry_t)entry;
    entry_fn();
    __builtin_unreachable();
}

/* -----------------------------------------------------------------------
 * Main second-stage bootloader logic
 * --------------------------------------------------------------------- */

void __attribute__((noreturn)) bootloader_main(void)
{
    uart_puts("\n[BL2] STA8600 Second-Stage Bootloader\n");
    uart_puts("[BL2] Build: " __DATE__ " " __TIME__ "\n");

    /* --- Locate SSBL via TOC --- */
    const toc_entry_t *ssbl_entry = toc_find_ssbl();
    if (!ssbl_entry) {
        uart_puts("[BL2] FATAL: Cannot locate SSBL in TOC\n");
        goto panic;
    }
    uart_puts("[BL2] SSBL found in TOC @ flash offset ");
    uart_puthex32(ssbl_entry->flash_offset);
    uart_puts(" size ");
    uart_puthex32(ssbl_entry->image_size);
    uart_putc('\n');

    const uint8_t *image    = (const uint8_t *)ssbl_entry->flash_offset;
    const secure_header_t *hdr = (const secure_header_t *)image;

    /* --- 1. Magic check --- */
    if (hdr->magic != SBH_MAGIC) {
        uart_puts("[BL2] FATAL: Bad image magic\n");
        goto panic;
    }
    uart_puts("[BL2] Image magic OK\n");

    /* --- 2. Anti-rollback --- */
    if (hdr->flags & SBH_FLAG_ROLLBACK_CHECK) {
        uint32_t min_ver = (OTP_MAGIC == OTP_SBOT_MAGIC) ? OTP_MINVER : 0;
        if (hdr->version < min_ver) {
            uart_puts("[BL2] FATAL: Anti-rollback check failed\n");
            uart_puts("[BL2]  Image version: ");
            uart_puthex32(hdr->version);
            uart_puts("\n[BL2]  Minimum allowed: ");
            uart_puthex32(min_ver);
            uart_putc('\n');
            goto panic;
        }
    }
    uart_puts("[BL2] Anti-rollback OK\n");

    /* --- 3. RSA signature over header [0x00..0x37] --- */
    wdt_kick();
    {
        uint8_t header_hash[32];
        size_t  signable_len = (size_t)((uint8_t *)hdr->signature - image);
        sha256(image, signable_len, header_hash);

        if (verify_rsa_signature(header_hash, hdr->signature) != 0) {
            uart_puts("[BL2] FATAL: Signature verification FAILED\n");
            goto panic;
        }
    }
    uart_puts("[BL2] Signature OK\n");

    /* --- 4. SHA-256 of image body --- */
    wdt_kick();
    {
        uint8_t body_hash[32];
        sha256(image + SBH_HEADER_SIZE, hdr->image_len, body_hash);
        if (ct_memcmp(body_hash, hdr->sha256, 32) != 0) {
            uart_puts("[BL2] FATAL: Image body hash mismatch\n");
            goto panic;
        }
    }
    uart_puts("[BL2] SHA-256 OK\n");

    /* --- 5. Decryption (in-place; requires unlocked flash writes) --- */
    if (hdr->flags & SBH_FLAG_ENCRYPTED) {
        /*
         * In production: call the CRYP peripheral for AES-256-CBC.
         * The AES key should be stored in a secure OTP key slot, NOT
         * in plaintext flash.  Key unwrap via the STA8600 KMS is the
         * recommended approach.
         *
         * For now: signal that decryption is needed but not performed.
         */
        uart_puts("[BL2] WARNING: Encrypted image — CRYP peripheral call needed\n");
        uart_puts("[BL2] Integrate STA8600 KMS/CRYP driver here\n");
        goto panic;
    }

    /* --- 6. Optional JTAG lock --- */
    if (hdr->flags & SBH_FLAG_DEBUG_LOCK) {
        lock_debug_port();
    }

    /* --- 7. Hand off to application --- */
    uart_puts("[BL2] All checks PASSED — handing off\n");
    jump_to_app(hdr->entry_point);

panic:
    uart_puts("[BL2] Halting.\n");
    while (1) {
        wdt_kick();   /* keep kicking so a watchdog reset does not occur       */
        __asm__ volatile ("wfi");
    }
}
