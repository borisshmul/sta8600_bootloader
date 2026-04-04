/*
 * protocol.c — STA8600 ROM Bootloader Host-Side Protocol Driver
 *
 * Also contains: TOC (Table of Contents) builder for the two-image
 * flash layout required by the STA8600 ROM bootloader.
 *
 * All USART frames follow the pattern:
 *   CMD  ~CMD        (command + complement, both sent)
 *   ACK / NACK
 *   ... payload ...
 *   ACK / NACK
 *
 * Timeouts use POSIX select().  All functions return 0 on success, -1 on
 * error (errno set), or -2 on NACK from target.
 *
 * CentOS / RHEL note:
 *   cfmakeraw() is a BSD extension.  _DEFAULT_SOURCE (or _BSD_SOURCE on
 *   glibc < 2.19, i.e. CentOS 7) must be defined before any system header
 *   to expose it.  The Makefile passes -D_DEFAULT_SOURCE; this guard is a
 *   belt-and-suspenders fallback.
 */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE   /* cfmakeraw, ETIMEDOUT on CentOS/RHEL glibc  */
#endif
#ifndef _BSD_SOURCE
#  define _BSD_SOURCE       /* CentOS 7 / glibc < 2.19 fallback           */
#endif

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

static int tty_configure(int fd, int baud_rate)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;

    cfmakeraw(&t);
    t.c_cflag  =  CS8 | CREAD | CLOCAL;  /* 8N1, no flow control          */
    t.c_cflag &= ~PARENB;
    t.c_cflag &= ~CSTOPB;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;

    speed_t spd;
    switch (baud_rate) {
        case 115200: spd = B115200; break;
        case 57600:  spd = B57600;  break;
        case 38400:  spd = B38400;  break;
        default:
            fprintf(stderr, "Unsupported baud %d\n", baud_rate);
            return -1;
    }
    cfsetispeed(&t, spd);
    cfsetospeed(&t, spd);

    if (tcsetattr(fd, TCSANOW, &t) < 0) return -1;
    tcflush(fd, TCIOFLUSH);
    return 0;
}

/* Wait up to `ms` milliseconds for `n` bytes.  Returns bytes read or -1. */
static int tty_read_timeout(int fd, uint8_t *buf, size_t n, int ms)
{
    size_t got = 0;
    struct timeval deadline;
    gettimeofday(&deadline, NULL);
    deadline.tv_usec += (ms % 1000) * 1000;
    deadline.tv_sec  += ms / 1000 + deadline.tv_usec / 1000000;
    deadline.tv_usec %= 1000000;

    while (got < n) {
        struct timeval now, rem;
        gettimeofday(&now, NULL);
        rem.tv_sec  = deadline.tv_sec  - now.tv_sec;
        rem.tv_usec = deadline.tv_usec - now.tv_usec;
        if (rem.tv_usec < 0) { rem.tv_sec--; rem.tv_usec += 1000000; }
        if (rem.tv_sec < 0) { errno = ETIMEDOUT; return -1; }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &rem);
        if (r < 0)  return -1;
        if (r == 0) { errno = ETIMEDOUT; return -1; }

        ssize_t nb = read(fd, buf + got, n - got);
        if (nb < 0) return -1;
        got += (size_t)nb;
    }
    return (int)got;
}

/* Read a single ACK/NACK byte, handling BUSY (0x76) by re-reading. */
static int read_ack(int fd)
{
    uint8_t b;
    for (int retry = 0; retry < 64; retry++) {
        if (tty_read_timeout(fd, &b, 1, 2000) < 0) {
            fprintf(stderr, "[BL] Timeout waiting for ACK\n");
            return -1;
        }
        if (b == BL_ACK)  return 0;
        if (b == BL_NACK) { fprintf(stderr, "[BL] NACK received\n"); return -2; }
        if (b == BL_BUSY) { usleep(10000); continue; }
        fprintf(stderr, "[BL] Unexpected byte 0x%02X waiting for ACK\n", b);
    }
    return -1;
}

static uint8_t xor_checksum(const uint8_t *data, size_t len)
{
    uint8_t c = 0;
    for (size_t i = 0; i < len; i++) c ^= data[i];
    return c;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

int bl_open(const char *dev, int baud)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) { perror("open"); return -1; }
    if (tty_configure(fd, baud) < 0) { close(fd); return -1; }
    return fd;
}

void bl_close(int fd)
{
    if (fd >= 0) close(fd);
}

/*
 * bl_sync — send 0x7F auto-baud byte and wait for ACK.
 * The chip must have BOOT pins in ROM-boot position and be freshly reset.
 * On STA8600 this may take up to 500 ms after reset de-assertion.
 */
int bl_sync(int fd)
{
    tcflush(fd, TCIOFLUSH);
    uint8_t sync = BL_SYNC_BYTE;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (write(fd, &sync, 1) != 1) return -1;
        int r = read_ack(fd);
        if (r == 0) {
            printf("[BL] Sync OK (attempt %d)\n", attempt + 1);
            return 0;
        }
        usleep(100000);
    }
    fprintf(stderr, "[BL] Sync failed after 5 attempts\n");
    return -1;
}

/*
 * bl_send_cmd — transmit a command byte and its complement, wait for ACK.
 */
int bl_send_cmd(int fd, uint8_t cmd)
{
    uint8_t frame[2] = { cmd, (uint8_t)(~cmd) };
    if (write(fd, frame, 2) != 2) return -1;
    return read_ack(fd);
}

int bl_get_info(int fd, uint8_t *ver_out, uint8_t *cmds_out, size_t *ncmds)
{
    if (bl_send_cmd(fd, CMD_GET) < 0) return -1;

    uint8_t hdr[2];   /* [0]=N-1 (count of following bytes-1), [1]=BL version */
    if (tty_read_timeout(fd, hdr, 2, 1000) < 0) return -1;
    uint8_t n = hdr[0];   /* number of additional command bytes */

    if (ver_out)  *ver_out = hdr[1];

    uint8_t buf[32] = {0};
    if (n > 0 && n <= 32) {
        if (tty_read_timeout(fd, buf, n, 1000) < 0) return -1;
    }
    if (cmds_out && ncmds) {
        size_t copy = n < *ncmds ? n : *ncmds;
        memcpy(cmds_out, buf, copy);
        *ncmds = n;
    }
    return read_ack(fd);
}

int bl_get_id(int fd, uint16_t *pid_out)
{
    if (bl_send_cmd(fd, CMD_GET_ID) < 0) return -1;

    uint8_t resp[3];  /* [0]=N-1=1, [1:2]=PID */
    if (tty_read_timeout(fd, resp, 3, 1000) < 0) return -1;

    if (pid_out)
        *pid_out = (uint16_t)((resp[1] << 8) | resp[2]);

    return read_ack(fd);
}

int bl_read_mem(int fd, uint32_t addr, uint8_t *buf, uint8_t len)
{
    if (bl_send_cmd(fd, CMD_READ_MEM) < 0) return -1;

    /* Address frame: 4-byte big-endian + XOR checksum */
    uint8_t aframe[5];
    aframe[0] = (addr >> 24) & 0xFF;
    aframe[1] = (addr >> 16) & 0xFF;
    aframe[2] = (addr >>  8) & 0xFF;
    aframe[3] = (addr      ) & 0xFF;
    aframe[4] = xor_checksum(aframe, 4);
    if (write(fd, aframe, 5) != 5) return -1;
    if (read_ack(fd) < 0) return -1;

    /* Length frame: N-1 + checksum */
    uint8_t lframe[2] = { (uint8_t)(len - 1), (uint8_t)(len - 1) };  /* XOR of single byte = itself, complement = ~byte but protocol uses N^(~N) = 0xFF which is also used; correct per AN4723 section 3.5 */
    lframe[1] = ~lframe[0];
    if (write(fd, lframe, 2) != 2) return -1;
    if (read_ack(fd) < 0) return -1;

    /* Read `len` data bytes */
    if (tty_read_timeout(fd, buf, len, 2000) < 0) return -1;
    return 0;
}

int bl_write_mem(int fd, uint32_t addr, const uint8_t *data, uint8_t len)
{
    if (bl_send_cmd(fd, CMD_WRITE_MEM) < 0) return -1;

    uint8_t aframe[5];
    aframe[0] = (addr >> 24) & 0xFF;
    aframe[1] = (addr >> 16) & 0xFF;
    aframe[2] = (addr >>  8) & 0xFF;
    aframe[3] = (addr      ) & 0xFF;
    aframe[4] = xor_checksum(aframe, 4);
    if (write(fd, aframe, 5) != 5) return -1;
    if (read_ack(fd) < 0) return -1;

    /* Data frame: [N-1][data...][checksum] */
    size_t frame_len = 1 + len + 1;
    uint8_t *frame = malloc(frame_len);
    if (!frame) return -1;

    frame[0] = len - 1;
    memcpy(frame + 1, data, len);
    /* checksum = XOR of N-1 and all data bytes */
    uint8_t ck = frame[0];
    for (int i = 0; i < len; i++) ck ^= data[i];
    frame[1 + len] = ck;

    int r = (write(fd, frame, frame_len) == (ssize_t)frame_len) ? 0 : -1;
    free(frame);
    if (r < 0) return -1;

    return read_ack(fd);
}

/*
 * bl_erase_ext — Extended erase: erase specific flash pages.
 * Pass pages=NULL and npage=0 to trigger global mass erase (special code 0xFFFF).
 */
int bl_erase_ext(int fd, const uint16_t *pages, uint16_t npage)
{
    if (bl_send_cmd(fd, CMD_EXT_ERASE) < 0) return -1;

    if (npage == 0 || pages == NULL) {
        /* Mass erase: 0xFF 0xFF + checksum (0x00) */
        uint8_t mass[3] = { 0xFF, 0xFF, 0x00 };
        if (write(fd, mass, 3) != 3) return -1;
    } else {
        size_t frame_len = 2 + npage * 2 + 1;
        uint8_t *frame = calloc(1, frame_len);
        if (!frame) return -1;

        frame[0] = (uint8_t)((npage - 1) >> 8);
        frame[1] = (uint8_t)((npage - 1) & 0xFF);
        for (uint16_t i = 0; i < npage; i++) {
            frame[2 + i*2]     = (uint8_t)(pages[i] >> 8);
            frame[2 + i*2 + 1] = (uint8_t)(pages[i] & 0xFF);
        }
        frame[frame_len - 1] = xor_checksum(frame, frame_len - 1);

        ssize_t written = write(fd, frame, frame_len);
        free(frame);
        if (written != (ssize_t)frame_len) return -1;
    }

    /* Erase can be slow — allow 30 s */
    uint8_t b;
    for (int i = 0; i < 300; i++) {
        if (tty_read_timeout(fd, &b, 1, 100) < 0) continue;
        if (b == BL_ACK)  return 0;
        if (b == BL_NACK) return -2;
        if (b == BL_BUSY) continue;
        fprintf(stderr, "[BL] Unexpected erase response 0x%02X\n", b);
        return -1;
    }
    fprintf(stderr, "[BL] Erase timeout\n");
    return -1;
}

int bl_erase_mass(int fd)
{
    return bl_erase_ext(fd, NULL, 0);
}

int bl_go(int fd, uint32_t addr)
{
    if (bl_send_cmd(fd, CMD_GO) < 0) return -1;

    uint8_t aframe[5];
    aframe[0] = (addr >> 24) & 0xFF;
    aframe[1] = (addr >> 16) & 0xFF;
    aframe[2] = (addr >>  8) & 0xFF;
    aframe[3] = (addr      ) & 0xFF;
    aframe[4] = xor_checksum(aframe, 4);
    if (write(fd, aframe, 5) != 5) return -1;
    return read_ack(fd);
}

/* -----------------------------------------------------------------------
 * CRC-32 (IEEE 802.3 polynomial 0xEDB88320) — used by the TOC
 * --------------------------------------------------------------------- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return crc;
}

static uint32_t crc32(const uint8_t *data, size_t len)
{
    return crc32_update(0xFFFFFFFFUL, data, len) ^ 0xFFFFFFFFUL;
}

/* -----------------------------------------------------------------------
 * bl_build_toc — build the 8 KB TOC binary for the STA8600.
 *
 * Caller provides pointers to the FSBL and SSBL image data (to get their
 * sizes).  The resulting TOC should be flashed to STA8600_TOC_BASE
 * (0x08000000) FIRST, then FSBL to STA8600_FSBL_BASE, then SSBL to
 * STA8600_SSBL_BASE.
 *
 * out_toc must point to a buffer of at least STA8600_TOC_SIZE (8192) bytes.
 * --------------------------------------------------------------------- */
void bl_build_toc(size_t fsbl_len, uint32_t fsbl_entry,
                  size_t ssbl_len, uint32_t ssbl_entry,
                  uint8_t out_toc[8192])
{
    memset(out_toc, 0xFF, 8192);

    sta8600_toc_t *toc = (sta8600_toc_t *)out_toc;
    toc->magic       = TOC_MAGIC;
    toc->version     = 1;
    toc->num_entries = 2;
    memset(toc->reserved, 0x00, sizeof(toc->reserved));

    /* Entry 0 — FSBL */
    sta8600_toc_entry_t *e0 = &toc->entries[0];
    e0->type         = TOC_TYPE_FSBL;
    e0->flash_offset = STA8600_FSBL_BASE;
    e0->load_addr    = STA8600_FSBL_LOAD_ADDR;
    e0->entry_point  = fsbl_entry ? fsbl_entry : STA8600_FSBL_LOAD_ADDR;
    e0->image_size   = (uint32_t)fsbl_len;
    e0->flags        = 0;
    e0->reserved     = 0;
    e0->crc32        = crc32((const uint8_t *)e0,
                              offsetof(sta8600_toc_entry_t, crc32));

    /* Entry 1 — SSBL */
    sta8600_toc_entry_t *e1 = &toc->entries[1];
    e1->type         = TOC_TYPE_SSBL;
    e1->flash_offset = STA8600_SSBL_BASE;
    e1->load_addr    = STA8600_SSBL_LOAD_ADDR;
    e1->entry_point  = ssbl_entry ? ssbl_entry : STA8600_SSBL_LOAD_ADDR;
    e1->image_size   = (uint32_t)ssbl_len;
    e1->flags        = 0;
    e1->reserved     = 0;
    e1->crc32        = crc32((const uint8_t *)e1,
                              offsetof(sta8600_toc_entry_t, crc32));

    /* Terminator entry */
    toc->entries[2].type = TOC_TERMINATOR;

    /* Header CRC covers magic + version + num_entries */
    toc->header_crc32 = crc32((const uint8_t *)toc,
                               offsetof(sta8600_toc_t, header_crc32));

    printf("[TOC] Built TOC: FSBL@0x%08X (%zu B), SSBL@0x%08X (%zu B)\n",
           STA8600_FSBL_BASE, fsbl_len,
           STA8600_SSBL_BASE, ssbl_len);
}

/*
 * bl_secure_write — CMD_SECURE_WRITE (0xA0)
 * The STA8600 secure write appends a 32-byte HMAC-SHA256 (using hmac_key)
 * over [address(4) || length(2) || data] to authenticate the write.
 * Key must match the device's OTP-provisioned write key.
 *
 * NOTE: Full HMAC computation requires OpenSSL; link with -lcrypto.
 */
#ifdef HAVE_OPENSSL
#include <openssl/hmac.h>

int bl_secure_write(int fd, uint32_t addr, const uint8_t *data, uint16_t len,
                    const uint8_t *hmac_key)
{
    if (bl_send_cmd(fd, CMD_SECURE_WRITE) < 0) return -1;

    /* Build authenticated payload */
    uint8_t aad[6];
    aad[0] = (addr >> 24) & 0xFF;
    aad[1] = (addr >> 16) & 0xFF;
    aad[2] = (addr >>  8) & 0xFF;
    aad[3] = (addr      ) & 0xFF;
    aad[4] = (len  >>  8) & 0xFF;
    aad[5] = (len       ) & 0xFF;

    /* HMAC-SHA256 over aad || data */
    HMAC_CTX *ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, hmac_key, 32, EVP_sha256(), NULL);
    HMAC_Update(ctx, aad, 6);
    HMAC_Update(ctx, data, len);
    uint8_t mac[32];
    unsigned mac_len = 32;
    HMAC_Final(ctx, mac, &mac_len);
    HMAC_CTX_free(ctx);

    /* Frame: aad(6) || data(len) || mac(32) */
    size_t frame_len = 6 + len + 32;
    uint8_t *frame = malloc(frame_len);
    if (!frame) return -1;
    memcpy(frame,           aad,  6);
    memcpy(frame + 6,       data, len);
    memcpy(frame + 6 + len, mac,  32);

    ssize_t written = write(fd, frame, frame_len);
    free(frame);
    if (written != (ssize_t)frame_len) return -1;

    return read_ack(fd);
}

int bl_secure_read(int fd, uint32_t addr, uint8_t *buf, uint16_t len,
                   const uint8_t *hmac_key)
{
    if (bl_send_cmd(fd, CMD_SECURE_READ) < 0) return -1;

    uint8_t aframe[7];
    aframe[0] = (addr >> 24) & 0xFF;
    aframe[1] = (addr >> 16) & 0xFF;
    aframe[2] = (addr >>  8) & 0xFF;
    aframe[3] = (addr      ) & 0xFF;
    aframe[4] = (len  >>  8) & 0xFF;
    aframe[5] = (len       ) & 0xFF;
    aframe[6] = xor_checksum(aframe, 6);
    if (write(fd, aframe, 7) != 7) return -1;
    if (read_ack(fd) < 0) return -1;

    /* Response: data(len) || mac(32) */
    size_t resp_len = len + 32;
    uint8_t *resp = malloc(resp_len);
    if (!resp) return -1;
    if (tty_read_timeout(fd, resp, resp_len, 3000) < 0) { free(resp); return -1; }

    /* Verify MAC */
    HMAC_CTX *ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, hmac_key, 32, EVP_sha256(), NULL);
    HMAC_Update(ctx, aframe, 6);   /* aad = addr || len */
    HMAC_Update(ctx, resp,  len);
    uint8_t expected_mac[32];
    unsigned mlen = 32;
    HMAC_Final(ctx, expected_mac, &mlen);
    HMAC_CTX_free(ctx);

    if (memcmp(expected_mac, resp + len, 32) != 0) {
        fprintf(stderr, "[BL] Secure read MAC mismatch — possible tampering!\n");
        free(resp);
        return -1;
    }
    memcpy(buf, resp, len);
    free(resp);
    return read_ack(fd);
}
#else
int bl_secure_write(int fd, uint32_t addr, const uint8_t *data, uint16_t len,
                    const uint8_t *hmac_key)
{
    (void)fd; (void)addr; (void)data; (void)len; (void)hmac_key;
    fprintf(stderr, "[BL] Secure write requires OpenSSL (build with -DHAVE_OPENSSL)\n");
    return -1;
}
int bl_secure_read(int fd, uint32_t addr, uint8_t *buf, uint16_t len,
                   const uint8_t *hmac_key)
{
    (void)fd; (void)addr; (void)buf; (void)len; (void)hmac_key;
    fprintf(stderr, "[BL] Secure read requires OpenSSL (build with -DHAVE_OPENSSL)\n");
    return -1;
}
#endif /* HAVE_OPENSSL */
