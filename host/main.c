/*
 * main.c — STA8600 Flash Tool  (Linux host side)
 *
 * The STA8600 ROM bootloader requires TWO binaries to be flashed, unlike
 * single-image chips (e.g. STA8100).  The ROM reads a TOC at 0x08000000
 * which describes both images:
 *
 *   FSBL (First Stage Boot Loader / BL2)
 *     → built from target/  → flashed to 0x08002000
 *     → ROM loads it to SYSRAM (0x20000000) and jumps there
 *
 *   SSBL (Second Stage / Application)
 *     → your application binary → flashed to 0x08040000
 *     → FSBL verifies and jumps there
 *
 * Flash order (enforced by this tool):
 *   1. Erase
 *   2. Write TOC       (0x08000000, 8 KB)
 *   3. Write FSBL      (0x08002000)
 *   4. Write SSBL/App  (0x08040000)
 *   5. Optionally CMD_GO to FSBL entry point
 *
 * Usage:
 *   sta_flash [options] --fsbl <fsbl.bin> --ssbl <app.bin>
 *
 * Options:
 *   -d <dev>            Serial device (default /dev/ttyUSB0)
 *   -b <baud>           Baud rate     (default 115200)
 *   -e                  Mass-erase flash before writing
 *   -g                  CMD_GO to FSBL entry after flashing
 *   -v                  Read-back verify after each write
 *   --fsbl <file>       FSBL binary (BL2, target/bl2_sta8600.bin)
 *   --ssbl <file>       SSBL / Application binary
 *   --sign-fsbl <pem>   Sign FSBL with RSA-2048 private key
 *   --sign-ssbl <pem>   Sign SSBL with RSA-2048 private key
 *   --encrypt <kfile>   AES-256 key file (32B key + 16B IV) for SSBL
 *   --verify-only <f>   Verify signature of a signed image, no flash
 *   --pub <pem>         Public key for --verify-only
 *   --keygen            Generate RSA-2048 keypair → keys/priv.pem, pub.pem
 *   --otp-write         Provision OTP (use with --pub and --hmac-key)
 *   --hmac-key <file>   32-byte HMAC write key for --otp-write
 *   --info              Read chip info only (no flash)
 */

#include "protocol.h"
#include "secure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>

#define CHUNK_SIZE  256

/* -----------------------------------------------------------------------
 * File helpers
 * --------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(sz ? sz : 1);
    if (!buf || (sz && fread(buf, 1, sz, f) != sz)) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    if (len_out) *len_out = sz;
    return buf;
}

static char *read_text_file(const char *path)
{
    size_t len;
    char *buf = (char *)read_file(path, &len);
    if (!buf) return NULL;
    char *out = realloc(buf, len + 1);
    if (!out) { free(buf); return NULL; }
    out[len] = '\0';
    return out;
}

/* -----------------------------------------------------------------------
 * Flash write with progress bar
 * --------------------------------------------------------------------- */

static int flash_region(int fd, uint32_t base_addr,
                         const char *label,
                         const uint8_t *image, size_t image_len,
                         int verify)
{
    size_t offset = 0;

    printf("[FLASH] %s: %zu bytes → 0x%08X\n", label, image_len, base_addr);

    while (offset < image_len) {
        size_t remain = image_len - offset;
        uint8_t chunk = (uint8_t)(remain > CHUNK_SIZE ? CHUNK_SIZE : remain);
        uint32_t addr = base_addr + (uint32_t)offset;

        if (bl_write_mem(fd, addr, image + offset, chunk) < 0) {
            fprintf(stderr, "\n[FLASH] Write error at 0x%08X\n", addr);
            return -1;
        }

        if (verify) {
            uint8_t rb[CHUNK_SIZE];
            if (bl_read_mem(fd, addr, rb, chunk) < 0) {
                fprintf(stderr, "\n[FLASH] Read-back error at 0x%08X\n", addr);
                return -1;
            }
            if (memcmp(rb, image + offset, chunk) != 0) {
                fprintf(stderr, "\n[FLASH] Verify mismatch at 0x%08X\n", addr);
                return -1;
            }
        }

        offset += chunk;
        printf("\r  [%-40s] %3d%%",
               "########################################" + (40 - (int)(offset * 40 / image_len)),
               (int)(offset * 100 / image_len));
        fflush(stdout);
    }
    printf("\r  [########################################] 100%% — OK\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * Optionally sign an image; returns a malloc'd signed image (caller frees)
 * or returns the original pointer with owned=0.
 * --------------------------------------------------------------------- */

static uint8_t *maybe_sign(const uint8_t *raw, size_t raw_len,
                            uint32_t load_addr, uint32_t entry,
                            const char *sign_key_path,
                            const char *enc_key_path,
                            size_t *out_len, int *owned)
{
    *owned = 0;
    *out_len = raw_len;

    if (!sign_key_path) {
        /* Return a non-owning copy so caller can always free() */
        uint8_t *copy = malloc(raw_len);
        if (!copy) return NULL;
        memcpy(copy, raw, raw_len);
        *owned = 1;
        return copy;
    }

    char *priv_pem = read_text_file(sign_key_path);
    if (!priv_pem) return NULL;

    uint8_t *aes_key = NULL;
    uint8_t  aes_iv[16] = {0};
    uint32_t enc_flags  = 0;

    if (enc_key_path) {
        size_t klen;
        aes_key = read_file(enc_key_path, &klen);
        if (!aes_key || klen < 48) {
            fprintf(stderr, "AES key file must be 48 bytes (32 key + 16 IV)\n");
            free(priv_pem); free(aes_key);
            return NULL;
        }
        memcpy(aes_iv, aes_key + 32, 16);
        enc_flags = SBH_FLAG_ENCRYPTED;
    }

    uint8_t *signed_img = NULL;
    sb_status_t st = sb_create_image(
        raw, raw_len, load_addr, entry, 1,
        SBH_FLAG_ROLLBACK_CHECK | enc_flags,
        priv_pem, aes_key, enc_flags ? aes_iv : NULL,
        &signed_img, out_len);

    free(priv_pem);
    free(aes_key);

    if (st != SB_OK) {
        fprintf(stderr, "[SEC] Signing failed: %s\n", sb_status_str(st));
        return NULL;
    }
    printf("[SEC] Signed image: %zu bytes\n", *out_len);
    *owned = 1;
    return signed_img;
}

/* -----------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *dev          = "/dev/ttyUSB0";
    int         baud         = 115200;
    int         do_erase     = 0;
    int         do_go        = 0;
    int         do_verify    = 0;
    int         do_keygen    = 0;
    int         do_otp       = 0;
    int         do_info      = 0;
    const char *fsbl_path    = NULL;
    const char *ssbl_path    = NULL;
    const char *sign_fsbl    = NULL;
    const char *sign_ssbl    = NULL;
    const char *enc_key_f    = NULL;
    const char *verify_only  = NULL;
    const char *pub_key_f    = NULL;
    const char *hmac_key_f   = NULL;

    static struct option long_opts[] = {
        { "fsbl",        required_argument, 0, 'F' },
        { "ssbl",        required_argument, 0, 'S' },
        { "sign-fsbl",   required_argument, 0, 'A' },
        { "sign-ssbl",   required_argument, 0, 'B' },
        { "encrypt",     required_argument, 0, 'E' },
        { "verify-only", required_argument, 0, 'V' },
        { "pub",         required_argument, 0, 'p' },
        { "keygen",      no_argument,       0, 'K' },
        { "otp-write",   no_argument,       0, 'O' },
        { "hmac-key",    required_argument, 0, 'H' },
        { "info",        no_argument,       0, 'I' },
        { 0, 0, 0, 0 }
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "d:b:egvF:S:A:B:E:V:p:KOHI",
                               long_opts, &idx)) != -1) {
        switch (opt) {
            case 'd': dev        = optarg;        break;
            case 'b': baud       = atoi(optarg);  break;
            case 'e': do_erase   = 1;             break;
            case 'g': do_go      = 1;             break;
            case 'v': do_verify  = 1;             break;
            case 'F': fsbl_path  = optarg;        break;
            case 'S': ssbl_path  = optarg;        break;
            case 'A': sign_fsbl  = optarg;        break;
            case 'B': sign_ssbl  = optarg;        break;
            case 'E': enc_key_f  = optarg;        break;
            case 'V': verify_only = optarg;       break;
            case 'p': pub_key_f  = optarg;        break;
            case 'K': do_keygen  = 1;             break;
            case 'O': do_otp     = 1;             break;
            case 'H': hmac_key_f = optarg;        break;
            case 'I': do_info    = 1;             break;
            default:
                fprintf(stderr, "Unknown option\n");
                return 1;
        }
    }

    /* --keygen: offline operation, no serial port needed */
    if (do_keygen) {
        printf("[SEC] Generating RSA-2048 keypair ...\n");
        return sb_keygen_rsa2048("keys/priv.pem", "keys/pub.pem") == 0 ? 0 : 1;
    }

    /* --verify-only: offline signature check */
    if (verify_only) {
        if (!pub_key_f) {
            fprintf(stderr, "--verify-only requires --pub <key.pem>\n");
            return 1;
        }
        size_t ilen;
        uint8_t *img = read_file(verify_only, &ilen);
        char *pub    = read_text_file(pub_key_f);
        if (!img || !pub) { free(img); free(pub); return 1; }
        sb_status_t st = sb_verify_image(img, ilen, pub, 0);
        printf("[SEC] %s: %s\n", verify_only, sb_status_str(st));
        free(img); free(pub);
        return st == SB_OK ? 0 : 1;
    }

    /* All other operations need a serial connection */
    if (!do_info && !fsbl_path && !ssbl_path && !do_otp) {
        fprintf(stderr,
            "Usage: %s [options] --fsbl <fsbl.bin> --ssbl <ssbl.bin>\n"
            "       %s --info -d <dev>\n"
            "       %s --keygen\n"
            "       %s --verify-only <image.bin> --pub <pub.pem>\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    printf("[HOST] Opening %s @ %d baud\n", dev, baud);
    int fd = bl_open(dev, baud);
    if (fd < 0) return 1;

    printf("[HOST] Syncing with STA8600 ROM bootloader ...\n");
    printf("[HOST] Ensure BOOT[0]=1, BOOT[1]=0 and assert nRESET.\n");
    if (bl_sync(fd) < 0) {
        fprintf(stderr, "[HOST] Sync failed. Check BOOT pins, wiring, and reset.\n");
        bl_close(fd);
        return 1;
    }

    uint8_t bl_ver = 0;
    uint8_t cmds[32];
    size_t  ncmds = sizeof(cmds);
    if (bl_get_info(fd, &bl_ver, cmds, &ncmds) == 0)
        printf("[BL] ROM BL version 0x%02X, %zu supported commands\n",
               bl_ver, ncmds);

    uint16_t pid = 0;
    if (bl_get_id(fd, &pid) == 0)
        printf("[BL] Product ID: 0x%04X %s\n", pid,
               pid == STA8600_PID ? "(STA8600 OK)" : "(UNEXPECTED)");

    if (do_info) { bl_close(fd); return 0; }

    /* --- Load FSBL --- */
    size_t   fsbl_raw_len = 0;
    uint8_t *fsbl_raw     = NULL;
    uint8_t *fsbl_image   = NULL;
    size_t   fsbl_len     = 0;
    int      fsbl_owned   = 0;

    if (fsbl_path) {
        fsbl_raw = read_file(fsbl_path, &fsbl_raw_len);
        if (!fsbl_raw) { bl_close(fd); return 1; }
        printf("[HOST] FSBL: %s (%zu bytes raw)\n", fsbl_path, fsbl_raw_len);

        if (fsbl_raw_len > STA8600_FSBL_MAX) {
            fprintf(stderr, "[HOST] FSBL too large (max %lu bytes)\n",
                    (unsigned long)STA8600_FSBL_MAX);
            free(fsbl_raw); bl_close(fd); return 1;
        }

        fsbl_image = maybe_sign(fsbl_raw, fsbl_raw_len,
                                STA8600_FSBL_LOAD_ADDR,
                                STA8600_FSBL_LOAD_ADDR,
                                sign_fsbl, NULL,
                                &fsbl_len, &fsbl_owned);
        free(fsbl_raw);
        if (!fsbl_image) { bl_close(fd); return 1; }
    }

    /* --- Load SSBL --- */
    size_t   ssbl_raw_len = 0;
    uint8_t *ssbl_raw     = NULL;
    uint8_t *ssbl_image   = NULL;
    size_t   ssbl_len     = 0;
    int      ssbl_owned   = 0;

    if (ssbl_path) {
        ssbl_raw = read_file(ssbl_path, &ssbl_raw_len);
        if (!ssbl_raw) {
            if (fsbl_owned) free(fsbl_image);
            bl_close(fd); return 1;
        }
        printf("[HOST] SSBL: %s (%zu bytes raw)\n", ssbl_path, ssbl_raw_len);

        ssbl_image = maybe_sign(ssbl_raw, ssbl_raw_len,
                                STA8600_SSBL_LOAD_ADDR,
                                STA8600_SSBL_LOAD_ADDR,
                                sign_ssbl, enc_key_f,
                                &ssbl_len, &ssbl_owned);
        free(ssbl_raw);
        if (!ssbl_image) {
            if (fsbl_owned) free(fsbl_image);
            bl_close(fd); return 1;
        }
    }

    /* --- OTP provisioning --- */
    if (do_otp) {
        if (!pub_key_f) {
            fprintf(stderr, "--otp-write requires --pub <key.pem>\n");
            goto cleanup;
        }
        uint8_t hmac_key[32] = {0};
        if (hmac_key_f) {
            size_t klen;
            uint8_t *kb = read_file(hmac_key_f, &klen);
            if (!kb || klen < 32) {
                fprintf(stderr, "HMAC key file must be >= 32 bytes\n");
                free(kb); goto cleanup;
            }
            memcpy(hmac_key, kb, 32);
            free(kb);
        }

        uint8_t otp_rec[256];
        if (sb_build_otp_record(pub_key_f, 1, 0, hmac_key, otp_rec) < 0) {
            fprintf(stderr, "[SEC] OTP record build failed\n");
            goto cleanup;
        }
        printf("[OTP] Writing 256-byte OTP record to 0x%08X\n", STA8600_OTP_BASE);
        printf("[OTP] IRREVERSIBLE — type YES to confirm: ");
        char confirm[16] = {0};
        if (fgets(confirm, sizeof(confirm), stdin) && strncmp(confirm, "YES", 3) == 0) {
            if (bl_write_mem(fd, STA8600_OTP_BASE,       otp_rec,       128) < 0 ||
                bl_write_mem(fd, STA8600_OTP_BASE + 128, otp_rec + 128, 128) < 0)
                fprintf(stderr, "[OTP] Write FAILED\n");
            else
                printf("[OTP] Provisioning complete.\n");
        } else {
            printf("[OTP] Aborted.\n");
        }
    }

    /* --- Nothing to flash --- */
    if (!fsbl_image && !ssbl_image) {
        bl_close(fd); return 0;
    }

    /* --- Erase --- */
    if (do_erase) {
        printf("[FLASH] Mass erasing flash (this may take 30 s) ...\n");
        if (bl_erase_mass(fd) < 0) {
            fprintf(stderr, "[FLASH] Erase FAILED\n");
            goto cleanup;
        }
        printf("[FLASH] Erase complete.\n");
    }

    /* ===================================================================
     * TWO-IMAGE FLASH SEQUENCE
     *
     * Step 1: Write TOC (describes both images to the ROM BL)
     * Step 2: Write FSBL to 0x08002000
     * Step 3: Write SSBL to 0x08040000
     * ================================================================ */

    /* Step 1 — Build and write TOC */
    {
        uint8_t toc_buf[8192];
        bl_build_toc(
            fsbl_image ? fsbl_len : 0,
            STA8600_FSBL_LOAD_ADDR,
            ssbl_image ? ssbl_len : 0,
            STA8600_SSBL_LOAD_ADDR,
            toc_buf);

        printf("[FLASH] Writing TOC (8192 bytes) to 0x%08X ...\n",
               STA8600_TOC_BASE);
        if (flash_region(fd, STA8600_TOC_BASE, "TOC",
                         toc_buf, sizeof(toc_buf), do_verify) < 0)
            goto cleanup;
    }

    /* Step 2 — FSBL */
    if (fsbl_image) {
        if (flash_region(fd, STA8600_FSBL_BASE, "FSBL",
                         fsbl_image, fsbl_len, do_verify) < 0)
            goto cleanup;
        printf("[FLASH] FSBL written OK.\n");
    }

    /* Step 3 — SSBL */
    if (ssbl_image) {
        if (flash_region(fd, STA8600_SSBL_BASE, "SSBL",
                         ssbl_image, ssbl_len, do_verify) < 0)
            goto cleanup;
        printf("[FLASH] SSBL written OK.\n");
    }

    /* --- CMD_GO to FSBL entry (restarts execution from flash) --- */
    if (do_go) {
        uint32_t entry = STA8600_FSBL_LOAD_ADDR;
        printf("[HOST] CMD_GO → 0x%08X\n", entry);
        bl_go(fd, entry);
    }

    printf("[HOST] Two-image flash complete.\n");
    bl_close(fd);
    if (fsbl_owned) free(fsbl_image);
    if (ssbl_owned) free(ssbl_image);
    return 0;

cleanup:
    bl_close(fd);
    if (fsbl_owned) free(fsbl_image);
    if (ssbl_owned) free(ssbl_image);
    return 1;
}
