#!/usr/bin/env python3
"""
flash.py — High-level Python flash script for STA8600

Wraps the sta_flash C tool and adds:
  - Automatic reset via GPIO (if gpio-toggle is available)
  - Pre-flash image header display
  - Positional or named bin file arguments

Usage (positional — simplest):
    python3 scripts/flash.py fsbl.bin ssbl.bin

Usage (named flags):
    python3 scripts/flash.py --fsbl fsbl.bin --ssbl ssbl.bin [options]

Default baud rate: 921600
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

    cmd = [TOOL, "-d", args.dev, "-b", str(args.baud),
           "--fsbl", args.fsbl, "--ssbl", args.ssbl]

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
        description="STA8600 flash helper (two-image: FSBL + SSBL)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python3 flash.py fsbl.bin ssbl.bin            # simplest\n"
            "  python3 flash.py fsbl.bin ssbl.bin --erase --go\n"
            "  python3 flash.py --fsbl fsbl.bin --ssbl ssbl.bin --sign-ssbl\n"
            "  python3 flash.py --info\n"
        )
    )
    # Positional: up to two .bin files; named flags override if both given
    parser.add_argument("bins",        nargs="*", metavar="BIN",
                        help="[fsbl.bin] [ssbl.bin]  (positional shorthand)")
    parser.add_argument("--dev",       default="/dev/ttyUSB0", help="Serial port (default /dev/ttyUSB0)")
    parser.add_argument("--baud",      type=int, default=921600,
                        help="Baud rate (default 921600)")
    parser.add_argument("--fsbl",      help="FSBL binary — overrides first positional")
    parser.add_argument("--ssbl",      help="SSBL / Application binary — overrides second positional")
    parser.add_argument("--erase",     action="store_true", help="Mass-erase before write")
    parser.add_argument("--go",        action="store_true", help="CMD_GO after flash")
    parser.add_argument("--verify",    action="store_true", help="Read-back verify")
    parser.add_argument("--sign-fsbl", action="store_true", help="Sign FSBL with keys/priv.pem")
    parser.add_argument("--sign-ssbl", action="store_true", help="Sign SSBL with keys/priv.pem")
    parser.add_argument("--encrypt",   metavar="KEY_FILE",
                        help="AES-256 key file (48 bytes: 32 key + 16 IV) for SSBL")
    parser.add_argument("--info",      action="store_true", help="Read chip info only")
    parser.add_argument("--keygen",    action="store_true", help="Generate RSA keypair")
    parser.add_argument("--gpio-chip",  default=None, help="GPIO chip for reset (e.g. gpiochip0)")
    parser.add_argument("--gpio-reset", type=int, default=None, help="GPIO line for nRESET")
    args = parser.parse_args()

    # Resolve positional → fsbl/ssbl (named flags take precedence)
    if len(args.bins) > 0 and not args.fsbl:
        args.fsbl = args.bins[0]
    if len(args.bins) > 1 and not args.ssbl:
        args.ssbl = args.bins[1]
    if len(args.bins) > 2:
        print("[WARN] More than two positional arguments — extras ignored.")

    check_tool()

    if args.keygen:
        subprocess.run([TOOL, "--keygen"])
        return

    if args.info:
        subprocess.run([TOOL, "-d", args.dev, "-b", str(args.baud), "--info"])
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
