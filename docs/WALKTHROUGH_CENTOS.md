# STA8600 Bootloader — CentOS Step-by-Step Walkthrough

This document walks through every step on a **CentOS 8 / Stream** host,
showing the exact commands to run and the expected terminal output at each
stage.  Two paths are covered:

- **Part A — Development (no signature):** get firmware onto the chip as fast as possible
- **Part B — Secure Boot:** key generation → OTP provisioning → signed flash

**Assumptions in this walkthrough:**
- nRESET is toggled **manually** (no automated GPIO reset)
- `/dev/ttyUSB0` may require `sudo` (handled automatically by the scripts)

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

### Step 4 — Check serial port permissions and adapter type

#### 4a — Identify which USB-serial driver loaded

```bash
lsusb | grep -iE "ftdi|cp210|prolific|ch340"
```

**CP2102 (Silicon Labs) — expected output:**
```
Bus 001 Device 003: ID 10c4:ea60 Silicon Laboratories CP210x UART Bridge
```

**FTDI (FT232R / FT2232H / FT4232H) — expected output:**
```
Bus 001 Device 004: ID 0403:6001 Future Technology Devices International, Ltd FT232 Serial (UART) IC
```

Check which kernel module is driving it:

```bash
dmesg | grep ttyUSB | tail -5
```

**CP2102:**
```
usb 1-1.2: cp210x converter now attached to ttyUSB0
```

**FTDI:**
```
usb 1-1.2: FTDI USB Serial Device converter now attached to ttyUSB0
```

Both appear as `/dev/ttyUSB0` — no command changes needed for that.

---

#### 4b — FTDI only: set the latency timer to 1 ms

> **Skip this step if you are using a CP2102, CH340, or PL2303 adapter.**

FTDI chips buffer incoming bytes and flush to the host every **16 ms** by
default.  Each bootloader round-trip (command → ACK → payload → ACK) hits
this buffer twice.  At 921600 baud the flash will run **10–20× slower than
expected** without this fix — a 64 KB SSBL that should take ~1 s will take
15–20 s.

Check the current timer value:

```bash
cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
```

**Expected output (bad — default):**
```
16
```

Set it to 1 ms:

```bash
echo 1 | sudo tee /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
```

**Expected output:**
```
1
```

Verify:

```bash
cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
```

```
1
```

**Make it permanent with a udev rule** (survives reboots and replug):

```bash
sudo tee /etc/udev/rules.d/99-ftdi-latency.rules <<'EOF'
# Set FTDI USB-serial latency timer to 1 ms for all FTDI devices.
# Without this, the 16 ms default buffer flush makes flashing ~15x slower.
ACTION=="add", SUBSYSTEM=="usb-serial", DRIVERS=="ftdi_sio", \
    ATTR{latency_timer}="1"
EOF
```

Reload udev rules and replug the adapter:

```bash
sudo udevadm control --reload-rules
# Unplug and replug the USB adapter, then verify:
cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
```

**Expected output:**
```
1
```

---

#### 4c — Check port permissions (all adapters)

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

### Step 5 — Verify chip connection (manual reset)

Use the `-w` flag so the tool pauses and tells you exactly when to toggle
nRESET.  This is the recommended approach whenever reset is manual.

```bash
host/sta_flash --info -w -d /dev/ttyUSB0
```

**Expected output — the tool opens the port and waits:**
```
[HOST] Opening /dev/ttyUSB0 @ 921600 baud

[HOST] -----------------------------------------------
[HOST]  Manual reset required
[HOST] -----------------------------------------------
[HOST]  1. Confirm BOOT[0]=1, BOOT[1]=0
[HOST]  2. Pull nRESET LOW  (e.g. short the reset pin)
[HOST]  3. Release nRESET HIGH
[HOST]  4. Press Enter here within ~2 seconds
[HOST] -----------------------------------------------
[HOST]  Waiting... (press Enter after reset):
```

Follow the printed steps on the board, then press Enter:

```
[HOST] Proceeding with sync.
[HOST] Syncing with STA8600 ROM bootloader ...
[BL]   Sync OK (attempt 1)
[BL]   ROM BL version 0x31, 11 supported commands
[BL]   Product ID: 0x0880 (STA8600 OK)
```

> **Timing note:** The ROM bootloader is only active for a short window
> after nRESET is released.  Toggle reset, then press Enter immediately —
> don't wait more than ~2 seconds.  The sync loop retries 5 times with
> short delays, so a little latency is fine.

**If the chip does not respond:**
```
[BL] Sync failed after 5 attempts
[HOST] Sync failed.
[HOST] Tip: use -w to be prompted before sync so you can reset manually.
```
→ Toggle reset again and re-run.  Try `-b 115200` if 921600 fails.

---

### Step 6 — Flash FSBL + SSBL (development, no signature)

Add `-w` so the tool pauses for your manual reset, and `--monitor` so
minicom opens automatically on the same port after flashing completes.

```bash
host/sta_flash -w -e -v -g  target/bl2_sta8600.bin  app.bin
```

Or using the Python wrapper (handles sudo for minicom automatically):

```bash
python3 scripts/flash.py -w --erase --go --monitor \
    target/bl2_sta8600.bin  app.bin
```

**Full expected output — flash tool:**
```
[HOST] Opening /dev/ttyUSB0 @ 921600 baud

[HOST] -----------------------------------------------
[HOST]  Manual reset required
[HOST] -----------------------------------------------
[HOST]  1. Confirm BOOT[0]=1, BOOT[1]=0
[HOST]  2. Pull nRESET LOW  (e.g. short the reset pin)
[HOST]  3. Release nRESET HIGH
[HOST]  4. Press Enter here within ~2 seconds
[HOST] -----------------------------------------------
[HOST]  Waiting... (press Enter after reset):
```

Toggle nRESET on the board, press Enter:

```
[HOST] Proceeding with sync.
[HOST] Syncing with STA8600 ROM bootloader ...
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

**If using `--monitor` and sudo is needed** (not in `dialout` group):
```
[MON] Flash complete. Opening serial monitor ...
[MON] Reset the board into normal boot mode (BOOT[0]=0) if needed.

[MON] No direct access to /dev/ttyUSB0.
[MON] Will launch minicom with sudo.
[MON] You can avoid this permanently by running:
[MON]   sudo usermod -aG dialout $USER  (then log out/in)

[MON] sudo password:
```

Type your sudo password (input is hidden), press Enter:

```
[MON] Launching: sudo minicom -D /dev/ttyUSB0 -b 921600
[MON] Press Ctrl-A X to exit minicom.
```

minicom opens. Toggle nRESET in normal boot mode (BOOT[0]=0, BOOT[1]=0).

**BL2 console output in minicom:**
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

Press **Ctrl-A X** then **Enter** to exit minicom.

> **"Bad image magic" from BL2?**  A raw unsigned `app.bin` has no secure
> header — BL2 will halt.  For development you can disable the magic check
> in `target/bootloader.c` and rebuild, or proceed to Part B to sign the image.

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

### Step 11 — Sign and flash both images (secure)

First confirm the unsigned image correctly fails verification:

```bash
host/sta_flash --verify-only app.bin --pub keys/pub.pem
```

**Expected output:**
```
[SEC] app.bin: Bad magic / not a secure image
```

Now sign and flash.  Use `-w` for manual reset and `--monitor` to open
minicom automatically when done:

```bash
python3 scripts/flash.py -w --erase --verify --go --monitor \
    --sign-fsbl --sign-ssbl \
    target/bl2_sta8600.bin  app.bin
```

Or directly with the C tool (no auto-monitor):

```bash
host/sta_flash -w -e -v -g \
    target/bl2_sta8600.bin  app.bin \
    --sign-fsbl keys/priv.pem \
    --sign-ssbl keys/priv.pem
```

**Expected output:**
```
[HOST] Opening /dev/ttyUSB0 @ 921600 baud

[HOST] -----------------------------------------------
[HOST]  Manual reset required
[HOST] -----------------------------------------------
[HOST]  1. Confirm BOOT[0]=1, BOOT[1]=0
[HOST]  2. Pull nRESET LOW  (e.g. short the reset pin)
[HOST]  3. Release nRESET HIGH
[HOST]  4. Press Enter here within ~2 seconds
[HOST] -----------------------------------------------
[HOST]  Waiting... (press Enter after reset):
```

Toggle reset, press Enter:

```
[HOST] Proceeding with sync.
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

#### Option A — via `--monitor` flag (recommended)

Pass `--monitor` to `flash.py` as shown in Step 11.  It opens minicom
automatically after flashing and handles sudo if needed.

#### Option B — open minicom manually

Install minicom if not present:

```bash
sudo dnf install -y minicom
```

Check if you can access the port directly:

```bash
ls -l /dev/ttyUSB0
```

**If you are in the `dialout` group** (run `groups` to check):
```bash
minicom -D /dev/ttyUSB0 -b 921600
```

**If you are NOT in the `dialout` group** (permission denied without sudo):
```bash
sudo minicom -D /dev/ttyUSB0 -b 921600
```

```
[sudo] password for youruser:
```
Type your password and press Enter.  minicom opens on the serial port.

> **Permanent fix** — add yourself to `dialout` so sudo is never needed:
> ```bash
> sudo usermod -aG dialout $USER
> ```
> Log out and back in.  Verify with `groups | grep dialout`.

Set BOOT[0]=0, BOOT[1]=0 (normal flash boot) and toggle nRESET manually.

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
| Check chip responds (manual reset) | `host/sta_flash --info -w` |
| Dev flash, manual reset | `host/sta_flash -w -e -g target/bl2_sta8600.bin app.bin` |
| Dev flash + open monitor after | `python3 scripts/flash.py -w --erase --go --monitor target/bl2_sta8600.bin app.bin` |
| Generate keys | `host/sta_flash --keygen` |
| Provision OTP dry run | `python3 scripts/secure_provision.py --dry-run` |
| Provision OTP (real) | `python3 scripts/secure_provision.py --dev /dev/ttyUSB0` |
| Signed flash, manual reset | `host/sta_flash -w -e -v -g target/bl2_sta8600.bin app.bin --sign-fsbl keys/priv.pem --sign-ssbl keys/priv.pem` |
| Signed flash + monitor | `python3 scripts/flash.py -w --erase --verify --go --monitor --sign-fsbl --sign-ssbl target/bl2_sta8600.bin app.bin` |
| Verify image offline | `host/sta_flash --verify-only app.bin --pub keys/pub.pem` |
| Set FTDI latency timer (once) | `echo 1 \| sudo tee /sys/bus/usb-serial/devices/ttyUSB0/latency_timer`
| Make FTDI latency permanent | `sudo tee /etc/udev/rules.d/99-ftdi-latency.rules` (see Step 4b)
| Monitor only (with sudo if needed) | `python3 -c "from scripts.flash import launch_monitor; launch_monitor('/dev/ttyUSB0', 921600)"` |
| Monitor directly (in dialout group) | `minicom -D /dev/ttyUSB0 -b 921600` |
| Monitor with sudo | `sudo minicom -D /dev/ttyUSB0 -b 921600` |
