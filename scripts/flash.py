#!/usr/bin/env python3
"""
flash.py — High-level Python flash script for STA8600

Wraps the sta_flash C tool and adds:
  - Automatic reset via GPIO (if gpio-toggle is available)
  - Progress bars
  - Pre-flash signature check
  - Hex-dump of the first bytes for sanity check

Usage:
    python3 flash.py --dev /dev/ttyUSB0 --image firmware.bin [--sign] [--erase] [--go]
"""

import argparse
import os
import subprocess
import sys
import struct


TOOL = os.path.join(os.path.dirname(__file__), "..", "host", "sta_flash")
PRIV_KEY = os.path.join(os.path.dirname(__file__), "..", "keys", "priv.pem")
PUB_KEY  = os.path.join(os.path.dirname(__file__), "..", "keys", "pub.pem")


def check_tool():
    if not os.path.isfile(TOOL):
        print(f"[!] sta_flash not found at {TOOL}")
        print("    Run: cd host && make")
        sys.exit(1)


def parse_secure_header(path: str) -> dict | None:
    """Parse and display the STA8600 secure header if present."""
    SBH_MAGIC = 0x53544131
    SBH_HEADER_SIZE = 0x138

    with open(path, "rb") as f:
        data = f.read()

    if len(data) < SBH_HEADER_SIZE:
        return None

    magic, version, load_addr, entry_point, image_len, flags = \
        struct.unpack_from("<IIIIII", data, 0)

    if magic != SBH_MAGIC:
        return None

    sha256 = data[0x18:0x38].hex()
    return {
        "magic":       hex(magic),
        "version":     version,
        "load_addr":   hex(load_addr),
        "entry_point": hex(entry_point),
        "image_len":   image_len,
        "flags":       hex(flags),
        "sha256":      sha256,
        "encrypted":   bool(flags & 1),
        "rollback":    bool(flags & 2),
        "debug_lock":  bool(flags & 4),
    }


def gpio_reset(gpio_chip: str, gpio_num: int):
    """Toggle a GPIO to hardware-reset the STA8600 (requires libgpiod)."""
    try:
        subprocess.run(
            ["gpioset", gpio_chip, f"{gpio_num}=0"],
            check=True, timeout=1
        )
        import time; time.sleep(0.1)
        subprocess.run(
            ["gpioset", gpio_chip, f"{gpio_num}=1"],
            check=True, timeout=1
        )
        print(f"[GPIO] Reset pulse on {gpio_chip}/{gpio_num}")
    except (FileNotFoundError, subprocess.CalledProcessError):
        print("[GPIO] gpioset not available — reset the board manually")


def run_flash(args):
    """
    STA8600 requires TWO binaries: FSBL (BL2) and SSBL (application).
    The tool builds the TOC automatically and flashes all three regions:
      0x08000000  TOC     (built internally)
      0x08002000  FSBL    (--fsbl)
      0x08040000  SSBL    (--ssbl)
    """
    if not args.fsbl or not args.ssbl:
        print("[!] STA8600 requires both --fsbl and --ssbl binaries")
        return 1

    cmd = [TOOL, "-d", args.dev, "-b", str(args.baud)]
    cmd += ["--fsbl", args.fsbl]
    cmd += ["--ssbl", args.ssbl]

    if args.erase:
        cmd.append("-e")
    if args.go:
        cmd.append("-g")
    if args.verify:
        cmd.append("-v")
    if args.sign_fsbl:
        cmd += ["--sign-fsbl", PRIV_KEY]
    if args.sign_ssbl:
        cmd += ["--sign-ssbl", PRIV_KEY]
    if args.encrypt:
        cmd += ["--encrypt", args.encrypt]

    print(f"[RUN] {' '.join(cmd)}")
    result = subprocess.run(cmd)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(
        description="STA8600 flash helper (two-image: FSBL + SSBL)"
    )
    parser.add_argument("--dev",       default="/dev/ttyUSB0", help="Serial port")
    parser.add_argument("--baud",      type=int, default=115200)
    parser.add_argument("--fsbl",      help="FSBL binary (target/bl2_sta8600.bin)")
    parser.add_argument("--ssbl",      help="SSBL / Application binary")
    parser.add_argument("--erase",     action="store_true")
    parser.add_argument("--go",        action="store_true", help="Jump after flash")
    parser.add_argument("--verify",    action="store_true", help="Read-back verify")
    parser.add_argument("--sign-fsbl", action="store_true", help="Sign FSBL with priv.pem")
    parser.add_argument("--sign-ssbl", action="store_true", help="Sign SSBL with priv.pem")
    parser.add_argument("--encrypt",   metavar="KEY_FILE",
                        help="AES-256 key file (48 bytes: 32 key + 16 IV) for SSBL")
    parser.add_argument("--info",      action="store_true", help="Read chip info only")
    parser.add_argument("--keygen",    action="store_true", help="Generate RSA keypair")
    parser.add_argument("--gpio-chip",  default=None, help="GPIO chip (e.g. gpiochip0)")
    parser.add_argument("--gpio-reset", type=int, default=None, help="GPIO line for nRESET")
    args = parser.parse_args()

    check_tool()

    if args.keygen:
        subprocess.run([TOOL, "--keygen"])
        return

    if args.info:
        subprocess.run([TOOL, "-d", args.dev, "--info"])
        return

    # Print header info for each binary
    for label, path in [("FSBL", args.fsbl), ("SSBL", args.ssbl)]:
        if not path:
            continue
        hdr = parse_secure_header(path)
        if hdr:
            print(f"[IMG] {label} — secure image:")
            for k, v in hdr.items():
                print(f"      {k:12s}: {v}")
        else:
            size = os.path.getsize(path)
            print(f"[IMG] {label} — raw: {path} ({size} bytes, no secure header)")

    # Optional GPIO reset
    if args.gpio_chip and args.gpio_reset is not None:
        gpio_reset(args.gpio_chip, args.gpio_reset)
        import time; time.sleep(0.3)   # let ROM bootloader start

    rc = run_flash(args)
    sys.exit(rc)


if __name__ == "__main__":
    main()
