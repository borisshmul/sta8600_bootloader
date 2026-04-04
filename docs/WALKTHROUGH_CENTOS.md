# STA8600 Bootloader — CentOS Step-by-Step Walkthrough

This document walks through every step on a **CentOS 8 / Stream** host,
showing the exact commands to run and the expected terminal output at each
stage.  Two paths are covered:

- **Part A — Development (no signature):** get firmware onto the chip as fast as possible
- **Part B — Secure Boot:** key generation → OTP provisioning → signed flash

> For reference-style documentation see [`INSTRUCTIONS.md`](INSTRUCTIONS.md).

---

## Environment assumed

| Item | Value |
|------|-------|
| Host OS | CentOS Stream 8 x86-64 |
| Serial adapter | CP2102 USB-UART at `/dev/ttyUSB0` |
| Working directory | repo root (`Bootloader_STA8600/`) |
| FSBL binary | `target/bl2_sta8600.bin` (built here) |
| SSBL / App binary | `app.bin` (your application) |

---

## Part A — Development Flash (no signature)

### Step 1 — Install dependencies

```bash
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb
sudo dnf install -y gcc make openssl openssl-devel python3 \
                    arm-linux-gnu-gcc arm-linux-gnu-binutils
```

**Expected output:**
```
Last metadata expiration check: 0:02:11 ago on Mon 04 Apr 2026 09:00:00 AM UTC.
Dependencies resolved.
================================================================================
 Package                       Arch    Version          Repository         Size
================================================================================
Installing:
 gcc                           x86_64  8.5.0-22.el8     baseos            23 M
 make                          x86_64  1:4.2.1-11.el8   baseos           498 k
 openssl                       x86_64  1:1.1.1k-9.el8   baseos           709 k
 openssl-devel                 x86_64  1:1.1.1k-9.el8   baseos           2.3 M
 python3                       x86_64  3.6.8-56.el8     baseos            78 k
 arm-linux-gnu-gcc             x86_64  10.3.1-1.el8     epel              32 M
 arm-linux-gnu-binutils        x86_64  2.35-17.el8      epel             4.9 M
...
Complete!
```

Verify the cross-compiler is available:

```bash
arm-linux-gnu-gcc --version
```

**Expected output:**
```
arm-linux-gnu-gcc (GCC) 10.3.1 20210422 (Red Hat 10.3.1-1)
Copyright (C) 2020 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.
```

---

### Step 2 — Build the host flash tool

```bash
cd host
make
```

**Expected output:**
```
gcc -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE -DHAVE_OPENSSL -c -o main.o main.c
gcc -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE -DHAVE_OPENSSL -c -o protocol.o protocol.c
gcc -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE -DHAVE_OPENSSL -c -o secure.o secure.c
gcc -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE -DHAVE_OPENSSL -o sta_flash main.o protocol.o secure.o -L/usr/lib64 -lcrypto
```

Confirm the binary exists:

```bash
ls -lh sta_flash
```

**Expected output:**
```
-rwxr-xr-x. 1 user user 87K Apr  4 09:05 sta_flash
```

```bash
cd ..
```

---

### Step 3 — Build BL2 (second-stage bootloader)

```bash
cd target
make
```

**Expected output:**
```
Cross prefix auto-detected: arm-linux-gnu-
arm-linux-gnu-gcc -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm \
  -mno-thumb-interwork -Os -Wall -Wextra -D_DEFAULT_SOURCE -ffreestanding \
  -fno-builtin -fno-stack-protector -fdata-sections -ffunction-sections \
  -nostdlib -nostdinc -I. -c -o start.o start.S
arm-linux-gnu-gcc ... -c -o bootloader.o bootloader.c
arm-linux-gnu-gcc ... -c -o pka_rsa.o pka_rsa.c
arm-linux-gnu-ld -T linker.ld -nostdlib --gc-sections --no-undefined \
  -Map=bl2_sta8600.map -o bl2_sta8600.elf start.o bootloader.o pka_rsa.o
   text    data     bss     dec     hex filename
  14336      64   16896   31296    7a40 bl2_sta8600.elf
arm-linux-gnu-objcopy -O binary --strip-debug bl2_sta8600.elf bl2_sta8600.bin
BL2 binary: bl2_sta8600.bin  (14336 bytes)
```

Check binary size is reasonable (should be well under 128 KB):

```bash
ls -lh bl2_sta8600.bin
```

**Expected output:**
```
-rw-r--r--. 1 user user 14K Apr  4 09:06 bl2_sta8600.bin
```

```bash
cd ..
```

---

### Step 4 — Check serial port permissions

```bash
ls -l /dev/ttyUSB0
```

**Expected output:**
```
crw-rw----. 1 root dialout 188, 0 Apr  4 09:07 /dev/ttyUSB0
```

If you get "Permission denied" when flashing, add yourself to the `dialout` group:

```bash
sudo usermod -aG dialout $USER
# Log out and back in, or run:
newgrp dialout
```

---

### Step 5 — Verify chip connection

Set **BOOT[0]=1, BOOT[1]=0** on the board, then assert and release nRESET.

```bash
host/sta_flash --info -d /dev/ttyUSB0
```

**Expected output:**
```
[HOST] Opening /dev/ttyUSB0 @ 921600 baud
[HOST] Syncing with STA8600 ROM bootloader ...
[HOST] Ensure BOOT[0]=1, BOOT[1]=0 and assert nRESET.
[BL]   Sync OK (attempt 1)
[BL]   ROM BL version 0x31, 11 supported commands
[BL]   Product ID: 0x0880 (STA8600 OK)
```

If the chip does not respond after 5 attempts:

```
[BL] Sync failed after 5 attempts
[HOST] Sync failed. Check BOOT pins, wiring, and reset.
```

→ Check BOOT pin state, confirm nRESET was pulsed, try `-b 115200`.

---

### Step 6 — Flash FSBL + SSBL (development, no signature)

Reset the board again (BOOT[0]=1), then:

```bash
host/sta_flash -e -v -g  target/bl2_sta8600.bin  app.bin
```

**Expected output:**
```
[HOST] Opening /dev/ttyUSB0 @ 921600 baud
[HOST] Syncing with STA8600 ROM bootloader ...
[HOST] Ensure BOOT[0]=1, BOOT[1]=0 and assert nRESET.
[BL]   Sync OK (attempt 1)
[BL]   ROM BL version 0x31, 11 supported commands
[BL]   Product ID: 0x0880 (STA8600 OK)
[HOST] FSBL: target/bl2_sta8600.bin (14336 bytes raw)
[HOST] SSBL: app.bin (65536 bytes raw)
[FLASH] Mass erasing flash (this may take 30 s) ...
[FLASH] Erase complete.
[FLASH] Writing TOC (8192 bytes) to 0x08000000 ...
  [########################################] 100% — OK
[FLASH] Writing FSBL (14336 bytes) to 0x08002000 ...
  [########################################] 100% — OK
[FLASH] FSBL written OK.
[FLASH] Writing SSBL (65536 bytes) to 0x08040000 ...
  [########################################] 100% — OK
[FLASH] SSBL written OK.
[HOST] CMD_GO → 0x20000000
[HOST] Two-image flash complete.
[HOST] Done.
```

After CMD_GO, BL2 starts from SYSRAM and you should see on the UART console
(921600 8N1):

```
[BL2] STA8600 Second-Stage Bootloader
[BL2] Build: Apr  4 2026 09:06:00
[BL2] SSBL found in TOC @ flash offset 0x08040000 size 0x00010000
[BL2] Image magic OK
[BL2] Anti-rollback OK
[BL2] Signature OK
[BL2] SHA-256 OK
[BL2] All checks PASSED — handing off
[BL2] Jumping to application at 0x08040000
```

> If the image has no secure header, BL2 will print "Bad image magic" and
> halt — this is expected for a raw `app.bin`.  For development without
> secure boot you can skip the signature checks by commenting out the magic
> check in `target/bootloader.c` and rebuilding BL2.

---

## Part B — Secure Boot

### Step 7 — Generate RSA-2048 keypair

```bash
mkdir -p keys
host/sta_flash --keygen
```

**Expected output:**
```
[SEC] Generating RSA-2048 keypair ...
[SEC] RSA-2048 keypair written:
  priv: keys/priv.pem
  pub:  keys/pub.pem
```

Verify the files:

```bash
ls -lh keys/
```

**Expected output:**
```
total 8.0K
-rw-------. 1 user user 1.7K Apr  4 09:10 priv.pem
-rw-r--r--. 1 user user  451 Apr  4 09:10 pub.pem
```

Inspect the public key:

```bash
openssl rsa -in keys/pub.pem -pubin -text -noout 2>/dev/null | head -6
```

**Expected output:**
```
RSA Public-Key: (2048 bit)
Modulus:
    00:c3:2a:f1:8e:...
    ...
Exponent: 65537 (0x10001)
```

---

### Step 8 — Extract the RSA modulus and embed it in BL2

The modulus bytes must be placed in `target/bootloader.c` so BL2 can
verify signatures without any external files.

```bash
openssl rsa -in keys/pub.pem -pubin -text -noout 2>/dev/null \
  | grep -A 50 "Modulus:" \
  | grep -oE '[0-9a-f]{2}(:[0-9a-f]{2})+' \
  | tr ':' '\n' \
  | grep -v '^00$' \
  | paste -sd, \
  | fold -s -w 72
```

**Expected output (your values will differ):**
```
c3,2a,f1,8e,4d,77,b2,09,aa,31,fc,e0,55,12,8d,3b,77,2c,19,f4,6e,9a,1d,
08,bc,44,e3,97,5f,22,11,ab,cd,...[256 bytes total]
```

Open `target/bootloader.c` and replace the `rsa_modulus` placeholder:

```c
/* BEFORE — placeholder */
static const uint8_t rsa_modulus[256] = {
    0xFF, [1 ... 254] = 0xFF, 0xFF
};

/* AFTER — your real modulus (256 bytes, big-endian) */
static const uint8_t rsa_modulus[256] = {
    0xc3, 0x2a, 0xf1, 0x8e, 0x4d, 0x77, ...
};
```

Also compute and embed the public key hash (used to cross-check OTP):

```bash
openssl rsa -in keys/pub.pem -pubin -outform DER 2>/dev/null \
  | openssl dgst -sha256
```

**Expected output:**
```
SHA256(stdin)= a7f3c9120d4e8b5f2a1c6d7e9b0f3a4c5d8e1f2b4c6d8e0f1a2b3c4d5e6f7a8
```

Place that 32-byte value in `embedded_pubkey_hash[]` in `target/bootloader.c`.

Rebuild BL2:

```bash
cd target && make && cd ..
```

**Expected output:**
```
arm-linux-gnu-gcc ... -c -o bootloader.o bootloader.c
arm-linux-gnu-gcc ... -c -o pka_rsa.o pka_rsa.c
arm-linux-gnu-ld ... -o bl2_sta8600.elf start.o bootloader.o pka_rsa.o
   text    data     bss     dec     hex filename
  15872      64   16896   32832    8040 bl2_sta8600.elf
arm-linux-gnu-objcopy -O binary --strip-debug bl2_sta8600.elf bl2_sta8600.bin
BL2 binary: bl2_sta8600.bin  (15872 bytes)
```

---

### Step 9 — OTP provisioning dry run

**Always dry-run first.  OTP writes are irreversible.**

```bash
python3 scripts/secure_provision.py \
    --dev /dev/ttyUSB0 \
    --min-version 1 \
    --dry-run
```

**Expected output:**
```
==================================================
  STA8600 Secure Boot Provisioning
  WARNING: OTP writes are IRREVERSIBLE
==================================================
[KEY] Using existing keypair in keys/
[KEY]   Private: keys/priv.pem
[KEY]   Public:  keys/pub.pem
[KEY] Generated HMAC write key: 8f3a21c0b4e7d591...
[OTP] PubKey hash: a7f3c9120d4e8b5f2a1c6d7e9b0f3a4c5d8e1f2b4c6d8e0f1a2b3c4d5e6f7a8
[OTP] OTP record (256 bytes) built
[PROV] Summary saved to keys/provisioning_summary.txt
[PROV] HMAC key saved to keys/hmac_write.key
[DRY-RUN] Not writing to chip.  OTP record saved in summary.
```

Review the OTP record that would be written:

```bash
cat keys/provisioning_summary.txt
```

**Expected output:**
```
STA8600 Secure Boot Provisioning Summary
==========================================

Public key:        keys/pub.pem
PubKey SHA-256:    a7f3c9120d4e8b5f2a1c6d7e9b0f3a4c5d8e1f2b4c6d8e0f1a2b3c4d5e6f7a8
HMAC write key:    8f3a21c0b4e7d591c2a0b3e4f5d6c7a8b9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4
OTP record SHA256: 3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f

OTP record (hex):
  0000  54 43 4f 4d 01 00 00 00 00 00 00 00 00 00 00 00
  0010  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0020  a7 f3 c9 12 0d 4e 8b 5f 2a 1c 6d 7e 9b 0f 3a 4c
  0030  5d 8e 1f 2b 4c 6d 8e 0f 1a 2b 3c 4d 5e 6f 7a 8b
  0040  8f 3a 21 c0 b4 e7 d5 91 c2 a0 b3 e4 f5 d6 c7 a8
  0050  b9 e0 f1 a2 b3 c4 d5 e6 f7 a8 b9 c0 d1 e2 f3 a4
  0060  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
  ...
  00f0  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
```

---

### Step 10 — Write OTP (production unit only)

Reset board in ROM-boot mode, then:

```bash
python3 scripts/secure_provision.py --dev /dev/ttyUSB0 --min-version 1
```

**Expected output:**
```
==================================================
  STA8600 Secure Boot Provisioning
  WARNING: OTP writes are IRREVERSIBLE
==================================================
[KEY] Using existing keypair in keys/
[KEY] Generated HMAC write key: 8f3a21c0b4e7d591...
[OTP] PubKey hash: a7f3c9120d4e8b5f...
[OTP] OTP record (256 bytes) built
[PROV] Summary saved to keys/provisioning_summary.txt
[PROV] HMAC key saved to keys/hmac_write.key

[PROV] About to write OTP to STA8600 on /dev/ttyUSB0
[PROV] Type 'YES' to continue: YES
[HOST] Opening /dev/ttyUSB0 @ 921600 baud
[BL]   Sync OK (attempt 1)
[OTP] Writing 256-byte OTP record to 0x1FFF0000
[OTP] IRREVERSIBLE — type YES to confirm: YES
[OTP] Provisioning complete.
[PROV] OTP provisioning COMPLETE.
[PROV] Store keys/priv.pem and keys/hmac_write.key SECURELY.
[PROV] NEVER flash a key signed with a different private key.
```

---

### Step 11 — Sign the SSBL image offline

```bash
host/sta_flash --verify-only app.bin --pub keys/pub.pem
```

Since `app.bin` is unsigned, this should fail — confirming the check works:

**Expected output:**
```
[SEC] app.bin: Bad magic / not a secure image
```

Now sign and flash both images together:

```bash
host/sta_flash -e -v -g \
    target/bl2_sta8600.bin  app.bin \
    --sign-fsbl keys/priv.pem \
    --sign-ssbl keys/priv.pem
```

**Expected output:**
```
[HOST] Opening /dev/ttyUSB0 @ 921600 baud
[HOST] Syncing with STA8600 ROM bootloader ...
[BL]   Sync OK (attempt 1)
[BL]   ROM BL version 0x31, 11 supported commands
[BL]   Product ID: 0x0880 (STA8600 OK)
[HOST] FSBL: target/bl2_sta8600.bin (15872 bytes raw)
[SEC]  Signed image: 15872 + 312 = 16184 bytes
[HOST] SSBL: app.bin (65536 bytes raw)
[SEC]  Signed image: 65536 + 312 = 65848 bytes
[FLASH] Mass erasing flash (this may take 30 s) ...
[FLASH] Erase complete.
[FLASH] Writing TOC (8192 bytes) to 0x08000000 ...
  [########################################] 100% — OK
[TOC]  Built TOC: FSBL@0x08002000 (16184 B), SSBL@0x08040000 (65848 B)
[FLASH] Writing FSBL (16184 bytes) to 0x08002000 ...
  [########################################] 100% — OK
[FLASH] FSBL written OK.
[FLASH] Writing SSBL (65848 bytes) to 0x08040000 ...
  [########################################] 100% — OK
[FLASH] SSBL written OK.
[HOST] CMD_GO → 0x20000000
[HOST] Two-image flash complete.
[HOST] Done.
```

---

### Step 12 — Verify signature offline (no hardware needed)

```bash
host/sta_flash --verify-only app_signed.bin --pub keys/pub.pem
```

**Expected output (pass):**
```
[SEC] app_signed.bin: OK
```

**Expected output (tampered or wrong key):**
```
[SEC] app_signed.bin: RSA signature verification failed
```

---

### Step 13 — Watch BL2 boot output on the secure image

Open a terminal emulator at **921600 8N1** (e.g. `minicom` or `screen`):

```bash
sudo dnf install -y minicom
minicom -D /dev/ttyUSB0 -b 921600
```

Set BOOT[0]=0, BOOT[1]=0 (normal flash boot) and reset the board.

**Expected BL2 console output:**
```
[BL2] STA8600 Second-Stage Bootloader
[BL2] Build: Apr  4 2026 09:06:00
[BL2] SSBL found in TOC @ flash offset 0x08040000 size 0x00010038
[BL2] Image magic OK
[BL2] Anti-rollback OK
[BL2] Signature OK
[BL2] SHA-256 OK
[BL2] All checks PASSED — handing off
[BL2] Jumping to application at 0x08040000
```

**If signature fails (e.g. wrong key embedded in BL2):**
```
[BL2] STA8600 Second-Stage Bootloader
[BL2] Build: Apr  4 2026 09:06:00
[BL2] SSBL found in TOC @ flash offset 0x08040000 size 0x00010038
[BL2] Image magic OK
[BL2] Anti-rollback OK
[BL2] FATAL: Signature verification FAILED
[BL2] Halting.
```

**If anti-rollback fires (image version < OTP minimum):**
```
[BL2] Image magic OK
[BL2] FATAL: Anti-rollback check failed
[BL2]  Image version: 0x00000000
[BL2]  Minimum allowed: 0x00000001
[BL2] Halting.
```

---

## Quick-reference command summary

| Goal | Command |
|------|---------|
| Install all deps (CentOS 8) | `sudo bash scripts/setup_centos.sh` |
| Build host tool | `cd host && make` |
| Build BL2 | `cd target && make` |
| Check cross-compiler detected | `cd target && make info` |
| Check chip responds | `host/sta_flash --info` |
| Dev flash (no sig) | `host/sta_flash -e -g target/bl2_sta8600.bin app.bin` |
| Generate keys | `host/sta_flash --keygen` |
| Provision OTP dry run | `python3 scripts/secure_provision.py --dry-run` |
| Provision OTP (real) | `python3 scripts/secure_provision.py --dev /dev/ttyUSB0` |
| Signed flash | `host/sta_flash -e -v -g target/bl2_sta8600.bin app.bin --sign-fsbl keys/priv.pem --sign-ssbl keys/priv.pem` |
| Verify image offline | `host/sta_flash --verify-only app.bin --pub keys/pub.pem` |
| Monitor BL2 console | `minicom -D /dev/ttyUSB0 -b 921600` |
