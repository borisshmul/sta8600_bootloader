# STA8600 Bootloader — Complete Instructions

## 1. Overview

```
Linux Host                          STA8600 Target
──────────────────────────────────────────────────────
sta_flash (C tool)   <-- UART -->  ROM Bootloader (built-in)
                                         │
                                         │ loads BL2
                                         ▼
                                   bl2_sta8600.bin  (target/)
                                         │
                                         │ verifies App
                                         ▼
                                   Application
```

The flow has **two** stages:

| Stage | What runs | Where |
|-------|-----------|-------|
| ROM BL | ST factory ROM | On-chip ROM (0x00000000) |
| BL2 (this project) | Second-stage loader | Flash 0x08000000 |
| App | Your firmware | Flash 0x08010000 |

---

## 2. Prerequisites

### Host — CentOS 8 / Stream / RHEL 8+

```bash
# One-shot setup script (run as root or with sudo):
sudo bash scripts/setup_centos.sh
```

Or manually:

```bash
# Enable EPEL
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb   # CentOS Stream 8 (CodeReady Builder)
# On CentOS 8 powertools is the equivalent:
# sudo dnf config-manager --set-enabled powertools

# Build dependencies
sudo dnf install -y gcc make openssl openssl-devel python3 python3-pip

# ARM cross-compiler (EPEL provides this on CentOS 8+)
sudo dnf install -y arm-linux-gnu-gcc arm-linux-gnu-binutils

# Optional: GPIO reset support
sudo dnf install -y libgpiod-utils
```

### Host — CentOS 7 / RHEL 7

```bash
# Enable EPEL
sudo yum install -y epel-release

# Build dependencies (openssl-devel, NOT libssl-dev)
sudo yum install -y gcc make openssl openssl-devel python3

# ARM cross-compiler is NOT in the CentOS 7 repos.
# Use ARM's official GNU Toolchain (free download):
#   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
#   Download: arm-gnu-toolchain-*-x86_64-arm-none-linux-gnueabihf.tar.xz
sudo mkdir -p /opt/arm-gnu-toolchain
sudo tar -xf arm-gnu-toolchain-*.tar.xz -C /opt/arm-gnu-toolchain --strip-components=1
echo 'export PATH=$PATH:/opt/arm-gnu-toolchain/bin' | sudo tee /etc/profile.d/arm-gnu-toolchain.sh
source /etc/profile.d/arm-gnu-toolchain.sh
# Then build with: make CROSS=arm-none-linux-gnueabihf-

# Optional: GPIO reset
sudo yum install -y libgpiod-utils
```

### Host — Ubuntu / Debian (reference)

```bash
sudo apt-get install gcc make libssl-dev python3 python3-pip \
    gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf gpiod
```

### Verify OpenSSL

```bash
openssl version    # must be >= 1.1.0
```

> **CentOS note:** The OpenSSL development package is `openssl-devel` (not
> `libssl-dev`).  Libraries are in `/usr/lib64/` on x86-64 — the host
> Makefile detects this automatically.

---

## 3. Hardware Setup

### BOOT Pin Configuration

The STA8600 samples BOOT[1:0] pins at power-on reset (POR).  Set them
**before** applying power or asserting nRESET.

| BOOT[1] | BOOT[0] | Boot Source |
|---------|---------|-------------|
| 0       | 0       | Internal Flash (normal run) |
| 0       | 1       | **ROM Bootloader via UART** ← use this for flashing |
| 1       | 0       | ROM Bootloader via SPI |
| 1       | 1       | Reserved |

> Consult the STA8600 Reference Manual for exact pin names on your package.

### UART Connection

Connect a 3.3 V USB-UART adapter (e.g., CP2102, FT232R) as follows:

```
Linux host          STA8600
──────────          ───────
TX  ──────────────► UART0_RX  (check RM for pin)
RX  ◄────────────── UART0_TX
GND ────────────── GND
                    nRESET — pull low then release to start ROM BL
```

Default: **115200 8N1**, no flow control.  The ROM bootloader auto-detects
baud by measuring the 0x7F sync byte.

---

## 4. Building the Host Flash Tool

```bash
cd host
make            # builds host/sta_flash
```

If you don't have OpenSSL headers:

```bash
sudo apt-get install libssl-dev
```

---

## 5. Building BL2 (target-side bootloader)

```bash
cd target
make            # auto-detects cross-compiler prefix
```

The Makefile probes for three cross-compiler prefixes in order:
`arm-linux-gnueabihf-` (Debian/Ubuntu/ARM toolchain),
`arm-linux-gnu-` (CentOS 8 EPEL),
`armv7hl-redhat-linux-gnueabi-` (older RHEL devtoolset).

Override manually if needed:
```bash
# CentOS 7 with ARM GNU Toolchain from developer.arm.com
make CROSS=arm-none-linux-gnueabihf-

# Show which compiler was selected
make info
```

> The binary is linked to load at `0x08000000`.  If you're loading via SYSRAM
> first (no XIP), change `EXEC_BASE` and `LOAD_BASE` in `linker.ld` to
> `0x20000000`.

---

## 6. Two-Image Flash Layout (STA8600 requirement)

Unlike single-image chips (e.g. STA8100), the **STA8600 ROM bootloader
requires two binaries** described by a TOC (Table of Contents) at the
start of flash:

```
Flash address   Region   Content
─────────────   ──────   ───────────────────────────────────────────
0x08000000      TOC      8 KB — built automatically by sta_flash
0x08002000      FSBL     First Stage BL (target/bl2_sta8600.bin)
                         Loaded by ROM → SYSRAM (0x20000000), 128 KB max
0x08040000      SSBL     Second Stage / Application
                         Loaded by FSBL from flash (XIP or copy)
```

The `sta_flash` tool writes all three in the correct order in a single
invocation.  Never flash only one binary — the ROM BL will not boot.

## 7. Non-Secure Boot (development)

### 7.1 Check chip responds

```bash
host/sta_flash --info -d /dev/ttyUSB0
```

Expected output:
```
[HOST] Syncing with STA8600 ROM bootloader ...
[BL]   ROM BL version 0x31, 11 supported commands
[BL]   Product ID: 0x0880 (STA8600 OK)
```

### 7.2 Flash FSBL + SSBL (no signature, development)

```bash
host/sta_flash -d /dev/ttyUSB0 -e -v -g \
    --fsbl target/bl2_sta8600.bin \
    --ssbl app.bin
```

This writes the TOC (0x08000000), FSBL (0x08002000), and SSBL (0x08040000)
in sequence, then CMD_GO to the FSBL entry point.

---

## 8. Secure Boot Setup

Secure boot on STA8600 provides a **hardware-enforced chain of trust**:

```
OTP (SHA-256 of pub key) ──► BL2 verifies RSA-2048 signature
                                     │
                              SHA-256 of image body
                                     │
                              Anti-rollback (version in OTP)
                                     │
                           Optional AES-256-CBC decrypt
                                     │
                              JTAG lock (optional)
                                     │
                              Jump to Application
```

### 7.1 Generate Keys

```bash
mkdir -p keys
cd host && make keygen
# Creates: keys/priv.pem  (KEEP SECRET)
#          keys/pub.pem
```

Or with OpenSSL directly:

```bash
openssl genrsa -out keys/priv.pem 2048
openssl rsa -in keys/priv.pem -pubout -out keys/pub.pem
```

### 7.2 Embed Public Key in BL2

Edit `target/bootloader.c`:

1. Export the public key modulus:
   ```bash
   openssl rsa -in keys/pub.pem -pubin -text -noout 2>/dev/null \
     | grep -A 100 "Modulus:" | head -34
   ```

2. Replace the `rsa_modulus[256]` placeholder array with the 256 bytes
   of your public modulus (big-endian).

3. Compute `embedded_pubkey_hash`:
   ```bash
   openssl rsa -in keys/pub.pem -pubin -outform DER 2>/dev/null \
     | openssl dgst -sha256
   ```
   Place the 32-byte result in `embedded_pubkey_hash[]`.

4. Rebuild BL2:
   ```bash
   cd target && make
   ```

### 7.3 OTP Provisioning (ONE TIME — IRREVERSIBLE)

> **WARNING:** OTP (One-Time Programmable) memory cannot be erased.
> Test on a non-production unit first.  Ensure `keys/pub.pem` is the
> final production key.

```bash
python3 scripts/secure_provision.py --dev /dev/ttyUSB0 --dry-run
# Review keys/provisioning_summary.txt

python3 scripts/secure_provision.py --dev /dev/ttyUSB0
# Type YES when prompted
```

What gets written to OTP (`0x1FFF0000`):

```
Offset  Size  Content
0x00    4     Magic: 0x53424F54 ("SBOT")
0x04    4     Minimum firmware version (anti-rollback)
0x08    4     Default flags
0x0C    20    Reserved (0x00)
0x20    32    SHA-256 of RSA-2048 public key (DER format)
0x40    32    HMAC-SHA256 write key (for CMD_SECURE_WRITE)
0x60    160   Reserved (0xFF)
```

After provisioning, store these files in a **hardware security module (HSM)**
or encrypted offline storage:

| File | Sensitivity |
|------|-------------|
| `keys/priv.pem` | SECRET — used to sign every firmware release |
| `keys/hmac_write.key` | SECRET — used for authenticated flash writes |
| `keys/pub.pem` | Public — can be shared |
| `keys/provisioning_summary.txt` | Confidential — archive it |

### 8.4 Sign and Flash a Secure Image

```bash
# Sign both FSBL and SSBL, erase, verify, and jump
host/sta_flash -d /dev/ttyUSB0 \
    -e -v -g \
    --fsbl target/bl2_sta8600.bin \
    --sign-fsbl keys/priv.pem \
    --ssbl app.bin \
    --sign-ssbl keys/priv.pem
```

This signs both images, builds the TOC, and flashes all three regions.

### 8.5 Sign + Encrypt

Generate a 48-byte key file (32 bytes AES key + 16 bytes IV):

```bash
dd if=/dev/urandom of=keys/aes.key bs=48 count=1
```

Flash with encryption:

```bash
host/sta_flash -d /dev/ttyUSB0 \
    -e -v \
    --sign keys/priv.pem \
    --encrypt keys/aes.key \
    -a 0x08010000 \
    app.bin
```

> The AES key must also be provisioned into the STA8600 KMS (Key Management
> System) secure key slot so BL2 can decrypt at boot.  This requires calling
> the STA8600 CRYP peripheral from BL2 — integrate ST's KMS driver into
> `target/bootloader.c` at the `SBH_FLAG_ENCRYPTED` section.

### 7.6 Verify an Image (offline, no hardware needed)

```bash
host/sta_flash --verify-only signed_ssbl.bin --pub keys/pub.pem
```

---

## 8. Using the Python Helper

```bash
# Flash a signed image with GPIO reset
python3 scripts/flash.py \
    --dev /dev/ttyUSB0 \
    --image app.bin \
    --sign \
    --erase \
    --go \
    --gpio-chip gpiochip0 \
    --gpio-reset 17   # BCM 17 = physical pin 11 on RPi

# Check chip info only
python3 scripts/flash.py --dev /dev/ttyUSB0 --info --image /dev/null
```

---

## 9. Secure Boot on a Locked (Production) Device

Once secure boot is provisioned and BL2 is flashed, the chip will:

1. Power on → ROM BL runs from 0x00000000
2. ROM BL loads BL2 from 0x08000000
3. BL2 reads the application at 0x08010000
4. BL2 checks: magic → anti-rollback → RSA sig → SHA-256 → (decrypt)
5. If all checks pass → jump to app
6. If **any** check fails → BL2 halts (no fallback, by design)

To re-flash a locked device you **must** present a correctly signed image.
The UART bootloader (ROM BL) remains accessible until you burn the
read-protection OTP bits (if supported by your STA8600 revision).

### Secure Update Flow

```bash
# 1. Increment version in your build system
# 2. Build app_v2.bin with new version embedded
# 3. Sign and flash both images (TOC + FSBL stay the same; only SSBL changes)
host/sta_flash -d /dev/ttyUSB0 -v \
    --fsbl target/bl2_sta8600.bin \
    --sign-fsbl keys/priv.pem \
    --ssbl app_v2.bin \
    --sign-ssbl keys/priv.pem
```

---

## 10. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `Sync failed` | Wrong BOOT pin state | Ensure BOOT[0]=1, BOOT[1]=0 and reset |
| `Sync failed` | Wrong serial port | `ls /dev/ttyUSB*` or `ls /dev/ttyACM*` |
| `Sync failed` | Baud mismatch | Try `-b 9600` then `-b 115200` |
| `NACK` on write | Flash not erased | Add `-e` flag |
| `Product ID: 0xXXXX (UNEXPECTED)` | Wrong board | Verify STA8600 is populated |
| `Signature FAILED` (BL2 console) | Wrong key embedded in BL2 | Rebuild BL2 with correct modulus |
| `Anti-rollback check failed` | Image version < OTP minimum | Increment firmware version |
| `FATAL: Public key hash mismatch vs OTP` | BL2 built with different key than OTP | Re-provision or rebuild BL2 |
| `implicit declaration of cfmakeraw` | Missing `_DEFAULT_SOURCE` on CentOS | Makefile sets it; verify with `make info` |
| `cannot find -lcrypto` | Wrong OpenSSL lib path on CentOS | `make info` shows detected path; or `sudo dnf install openssl-devel` |
| `arm-linux-gnueabihf-gcc: command not found` | Cross-compiler not installed | On CentOS: `sudo dnf install arm-linux-gnu-gcc` (EPEL) or run `setup_centos.sh` |
| `No ARM cross-compiler found` | None of the three prefixes present | `sudo bash scripts/setup_centos.sh` to auto-install ARM toolchain |
| No UART output from BL2 | UART0 not initialized | Check UART0 base address matches silicon |

### UART Console (BL2 debug output)

BL2 prints to UART0 at 115200 8N1.  You will see:

```
[BL2] STA8600 Second-Stage Bootloader
[BL2] Image magic OK
[BL2] Anti-rollback OK
[BL2] Signature OK
[BL2] SHA-256 OK
[BL2] Jumping to application at 0x08010000
```

On failure:

```
[BL2] FATAL: RSA signature verification FAILED
[BL2] Halting.
```

---

## 11. File Map

```
Bootloader_STA8600/
├── host/
│   ├── main.c          CLI flash tool entry point
│   ├── protocol.c/h    STA8600 ROM bootloader UART protocol
│   ├── secure.c/h      RSA-2048 / AES-256 image signing (OpenSSL)
│   └── Makefile
├── target/
│   ├── start.S         ARM Cortex-A7 reset / vector table
│   ├── bootloader.c    BL2 main: verify + jump
│   ├── linker.ld       Memory map
│   └── Makefile
├── scripts/
│   ├── flash.py             High-level Python flash helper
│   ├── secure_provision.py  One-time OTP provisioning
│   └── setup_centos.sh      Dependency installer for CentOS 7/8/Stream
├── keys/               Generated keys (git-ignored)
└── docs/
    └── INSTRUCTIONS.md  This file
```

---

## 12. Security Checklist

- [ ] RSA-2048 private key generated offline on an air-gapped machine
- [ ] Private key stored in HSM or encrypted vault
- [ ] OTP provisioned on each unit individually (unique HMAC key per device recommended)
- [ ] BL2 built with `embedded_pubkey_hash` matching the OTP record
- [ ] Anti-rollback minimum version set in OTP
- [ ] Debug port locked in production (`SBH_FLAG_DEBUG_LOCK`)
- [ ] AES key stored in STA8600 secure key slot (not in plaintext flash)
- [ ] Read-protection OTP bits burned after production validation
- [ ] `pka_rsa2048_verify_pkcs1` replaced with ST's PKA hardware driver
- [ ] Firmware update transport is authenticated (e.g., HTTPS + signature)
