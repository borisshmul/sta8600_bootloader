# STA8600 Bootloader

Linux host tool + second-stage bootloader (BL2) for the STMicroelectronics STA8600 (ARM Cortex-A7, automotive GNSS).

> Full instructions: [`docs/INSTRUCTIONS.md`](docs/INSTRUCTIONS.md)

---

## Quick Start

### 1. Install dependencies

**CentOS 8 / Stream / RHEL:**
```bash
sudo bash scripts/setup_centos.sh
```

**Ubuntu / Debian:**
```bash
sudo apt-get install gcc make libssl-dev python3 gcc-arm-linux-gnueabihf
```

### 2. Build

```bash
cd host   && make          # builds host/sta_flash
cd target && make          # builds target/bl2_sta8600.bin
```

### 3. Flash (two binaries required)

Connect a 3.3 V USB-UART adapter to UART0, set **BOOT[0]=1 BOOT[1]=0**, then reset the board.

```bash
# Simplest — positional arguments
host/sta_flash -e -g  target/bl2_sta8600.bin  app.bin

# Or with named flags
host/sta_flash --fsbl target/bl2_sta8600.bin --ssbl app.bin -e -g

# Python wrapper (same thing)
python3 scripts/flash.py target/bl2_sta8600.bin app.bin --erase --go
```

Default baud: **921600**. Use `-b 115200` if your adapter cannot sustain it.

The tool writes three regions automatically:

| Address | Content |
|---------|---------|
| `0x08000000` | TOC (built automatically) |
| `0x08002000` | FSBL — `bl2_sta8600.bin` |
| `0x08040000` | SSBL — your application |

---

## Secure Boot

```bash
# Generate keys (once)
host/sta_flash --keygen                         # → keys/priv.pem, keys/pub.pem

# Provision OTP (IRREVERSIBLE — test unit first)
python3 scripts/secure_provision.py --dry-run   # review first
python3 scripts/secure_provision.py --dev /dev/ttyUSB0

# Sign and flash
host/sta_flash -e -v -g \
    target/bl2_sta8600.bin app.bin \
    --sign-fsbl keys/priv.pem \
    --sign-ssbl keys/priv.pem
```

BL2 enforces: RSA-2048 signature → SHA-256 body hash → anti-rollback version → optional AES-256 decryption → JTAG lock → jump.

---

## Project Layout

```
host/       C flash tool (sta_flash) — runs on Linux
target/     BL2 firmware — runs on STA8600 Cortex-A7
scripts/    flash.py, secure_provision.py, setup_centos.sh
keys/       Generated keys (git-ignored)
docs/       Full instructions
```

---

## Common Options

| Flag | Meaning |
|------|---------|
| `-d /dev/ttyUSB0` | Serial port |
| `-b 921600` | Baud rate (default) |
| `-e` | Mass-erase before write |
| `-v` | Read-back verify |
| `-g` | CMD_GO after flash |
| `--sign-fsbl / --sign-ssbl` | RSA-sign the image |
| `--info` | Read chip ID only |
| `--keygen` | Generate RSA-2048 keypair |
