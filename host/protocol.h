/*
 * sta8600_protocol.h — STA8600 ROM Bootloader Protocol Definitions
 *
 * The STA8600 ROM exposes a USART bootloader on POR when BOOT pins are
 * sampled in "ROM boot" state.  The protocol is byte-oriented, uses XOR
 * checksums, and follows the ST AN4723 / STA-series application note family.
 *
 * Baud: 115200 8N1 (auto-baud after 0x7F sync byte)
 */

#ifndef STA8600_PROTOCOL_H
#define STA8600_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Wire-level constants
 * --------------------------------------------------------------------- */
#define BL_SYNC_BYTE        0x7F   /* sent once to trigger auto-baud      */
#define BL_ACK              0x79
#define BL_NACK             0x1F
#define BL_BUSY             0x76   /* chip is processing, wait and re-read */

/* -----------------------------------------------------------------------
 * Command opcodes  (STA8600 ROM bootloader command set)
 * --------------------------------------------------------------------- */
#define CMD_GET             0x00   /* Get supported commands + BL version  */
#define CMD_GET_VERSION     0x01   /* Protocol version + option bytes       */
#define CMD_GET_ID          0x02   /* Product ID (2 bytes)                  */
#define CMD_READ_MEM        0x11   /* Read up to 256 bytes                  */
#define CMD_GO              0x21   /* Jump to address                       */
#define CMD_WRITE_MEM       0x31   /* Write up to 256 bytes                 */
#define CMD_ERASE           0x43   /* Mass/page erase (standard)            */
#define CMD_EXT_ERASE       0x44   /* Extended erase (STA8600 preferred)    */
#define CMD_WRITE_PROTECT   0x63
#define CMD_WRITE_UNPROTECT 0x73
#define CMD_READ_PROTECT    0x82
#define CMD_READ_UNPROTECT  0x92
#define CMD_SECURE_WRITE    0xA0   /* Authenticated write (secure variant)  */
#define CMD_SECURE_READ     0xA1   /* Authenticated read  (secure variant)  */

/* -----------------------------------------------------------------------
 * STA8600 memory map (from STA8600 Reference Manual Rev 1.2)
 * --------------------------------------------------------------------- */
#define STA8600_ROM_BASE        0x00000000UL
#define STA8600_ROM_SIZE        0x00040000UL   /* 256 KB ROM               */
#define STA8600_SYSRAM_BASE     0x20000000UL
#define STA8600_SYSRAM_SIZE     0x00080000UL   /* 512 KB system RAM        */
#define STA8600_FLASH_BASE      0x08000000UL
#define STA8600_FLASH_SIZE      0x00400000UL   /* 4 MB internal flash      */
#define STA8600_OTP_BASE        0x1FFF0000UL   /* OTP / eFuse region       */
#define STA8600_OTP_SIZE        0x00001000UL
#define STA8600_ENTRY_POINT     0x08000000UL   /* default application GO   */

/* Secure world (TrustZone) base — only accessible in secure state        */
#define STA8600_SEC_FLASH_BASE  0x0C000000UL
#define STA8600_SEC_RAM_BASE    0x30000000UL

/* -----------------------------------------------------------------------
 * Product ID
 * --------------------------------------------------------------------- */
#define STA8600_PID             0x0880         /* 2-byte PID from CMD_GET_ID */

/* -----------------------------------------------------------------------
 * STA8600 Flash partition layout
 *
 * The ROM bootloader reads a TOC (Table of Contents) at the very start of
 * internal flash.  The TOC contains descriptors for the two mandatory images:
 *
 *   Image 0 — FSBL (First Stage Boot Loader / BL2)
 *     Loaded by ROM BL into SYSRAM (0x20000000).
 *     Max size: 128 KB.  Must fit in SYSRAM.
 *     Flash offset: 0x08002000 (after TOC at 0x08000000)
 *
 *   Image 1 — SSBL (Second Stage / Application)
 *     Loaded by FSBL from flash.
 *     Flash offset: 0x08020000
 *     Max size: remainder of flash
 *
 * Partition map:
 *   0x08000000  0x2000   TOC (8 KB, 8 × 1 KB descriptor slots)
 *   0x08002000  0x20000  FSBL binary  (128 KB max)
 *   0x08022000  ...      Reserved / padding
 *   0x08040000  ...      SSBL / Application binary
 * --------------------------------------------------------------------- */
#define STA8600_TOC_BASE        0x08000000UL
#define STA8600_TOC_SIZE        0x00002000UL   /* 8 KB                     */
#define STA8600_FSBL_BASE       0x08002000UL   /* FSBL flash location      */
#define STA8600_FSBL_MAX        0x00020000UL   /* 128 KB max FSBL          */
#define STA8600_SSBL_BASE       0x08040000UL   /* SSBL / App flash location */
#define STA8600_FSBL_LOAD_ADDR  0x20000000UL   /* FSBL runs from SYSRAM    */
#define STA8600_SSBL_LOAD_ADDR  0x08040000UL   /* SSBL runs XIP from flash */

/* TOC magic and entry type codes */
#define TOC_MAGIC               0x54434F43UL   /* "TOCM"                   */
#define TOC_TERMINATOR          0xFFFFFFFFUL
#define TOC_TYPE_FSBL           0x00000001UL
#define TOC_TYPE_SSBL           0x00000002UL
#define TOC_TYPE_UNUSED         0xFFFFFFFFUL

/*
 * sta8600_toc_entry_t — one 32-byte slot in the TOC
 *
 * The ROM BL scans entries until it hits TOC_TERMINATOR in the type field.
 * It loads the entry whose type == TOC_TYPE_FSBL to load_addr and jumps.
 */
typedef struct __attribute__((packed)) {
    uint32_t type;          /* TOC_TYPE_*                                  */
    uint32_t flash_offset;  /* byte offset from start of flash             */
    uint32_t load_addr;     /* destination address in RAM/flash            */
    uint32_t entry_point;   /* execution entry point after load            */
    uint32_t image_size;    /* byte count of the image (excl. header)      */
    uint32_t flags;         /* SBH_FLAG_* if image has a secure header     */
    uint32_t crc32;         /* CRC-32 of this entry (all fields before crc)*/
    uint32_t reserved;      /* must be 0                                   */
} sta8600_toc_entry_t;      /* 32 bytes per entry                          */

#define TOC_MAX_ENTRIES         8               /* fits in 8 KB TOC region  */

typedef struct __attribute__((packed)) {
    uint32_t           magic;                   /* TOC_MAGIC                */
    uint32_t           version;                 /* TOC format version = 1   */
    uint32_t           num_entries;             /* valid entry count        */
    uint32_t           header_crc32;            /* CRC of [magic..num_entries] */
    uint8_t            reserved[48];            /* pad to 64 bytes          */
    sta8600_toc_entry_t entries[TOC_MAX_ENTRIES];
} sta8600_toc_t;            /* 64 + 8*32 = 320 bytes, fits in 8KB TOC      */

/* -----------------------------------------------------------------------
 * Secure boot header (prepended to every authenticated image)
 *
 *  Offset  Size  Field
 *  0x00    4     Magic  (0x53544131 = "STA1")
 *  0x04    4     Version
 *  0x08    4     Load address
 *  0x0C    4     Entry point
 *  0x10    4     Image length (bytes, not including header)
 *  0x14    4     Flags  (see SBH_FLAG_*)
 *  0x18    32    SHA-256 of image body
 *  0x38    256   RSA-2048 PKCS#1 v1.5 signature of bytes [0x00..0x37]
 *  0x138   N     Image body
 * --------------------------------------------------------------------- */
#define SBH_MAGIC               0x53544131UL
#define SBH_HEADER_SIZE         0x138
#define SBH_FLAG_ENCRYPTED      (1u << 0)  /* image body is AES-256-CBC     */
#define SBH_FLAG_ROLLBACK_CHECK (1u << 1)  /* check version >= OTP minimum  */
#define SBH_FLAG_DEBUG_LOCK     (1u << 2)  /* disable JTAG after boot       */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t load_addr;
    uint32_t entry_point;
    uint32_t image_len;
    uint32_t flags;
    uint8_t  sha256[32];
    uint8_t  signature[256];   /* RSA-2048 */
} sta8600_secure_header_t;

/* -----------------------------------------------------------------------
 * API exposed by protocol.c
 * --------------------------------------------------------------------- */
int  bl_open(const char *dev, int baud);
void bl_close(int fd);

int  bl_sync(int fd);
int  bl_send_cmd(int fd, uint8_t cmd);
int  bl_get_info(int fd, uint8_t *ver_out, uint8_t *cmds_out, size_t *ncmds);
int  bl_get_id(int fd, uint16_t *pid_out);
int  bl_read_mem(int fd, uint32_t addr, uint8_t *buf, uint8_t len);
int  bl_write_mem(int fd, uint32_t addr, const uint8_t *data, uint8_t len);
int  bl_erase_ext(int fd, const uint16_t *pages, uint16_t npage);
int  bl_erase_mass(int fd);
int  bl_go(int fd, uint32_t addr);

/* Authenticated (secure) variants — require session key established first */
int  bl_secure_write(int fd, uint32_t addr, const uint8_t *data, uint16_t len,
                     const uint8_t *hmac_key);
int  bl_secure_read(int fd, uint32_t addr, uint8_t *buf, uint16_t len,
                    const uint8_t *hmac_key);

/*
 * bl_build_toc — construct the 8 KB TOC binary that the STA8600 ROM BL
 * expects at 0x08000000.  Must be flashed before the FSBL or SSBL images.
 * out_toc must be 8192 bytes.
 */
void bl_build_toc(size_t fsbl_len, uint32_t fsbl_entry,
                  size_t ssbl_len, uint32_t ssbl_entry,
                  uint8_t out_toc[8192]);

#endif /* STA8600_PROTOCOL_H */
