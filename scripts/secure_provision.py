#!/usr/bin/env python3
"""
secure_provision.py — STA8600 One-Time Secure Boot Provisioning

Steps performed:
  1. Generate RSA-2048 keypair  (or load existing)
  2. Generate a random 32-byte HMAC write-key
  3. Build the 256-byte OTP record (magic, version, pubkey hash, hmac key)
  4. Write OTP record to the chip via sta_flash --otp-write
  5. Save human-readable provisioning summary

WARNING: OTP writes are IRREVERSIBLE.  Test on a non-production unit first.

Usage:
    python3 secure_provision.py --dev /dev/ttyUSB0 [--dry-run]
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
from pathlib import Path

TOOL     = str(Path(__file__).parent.parent / "host" / "sta_flash")
KEYS_DIR = Path(__file__).parent.parent / "keys"

OTP_SBOT_MAGIC = 0x53424F54   # "SBOT"


def sha256_file(path: str) -> bytes:
    """SHA-256 of file contents."""
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).digest()


def gen_keypair() -> tuple[str, str]:
    """Generate RSA-2048 keypair, return (priv_path, pub_path)."""
    KEYS_DIR.mkdir(parents=True, exist_ok=True)
    priv = str(KEYS_DIR / "priv.pem")
    pub  = str(KEYS_DIR / "pub.pem")

    if Path(priv).exists() and Path(pub).exists():
        print("[KEY] Using existing keypair in keys/")
        return priv, pub

    print("[KEY] Generating RSA-2048 keypair ...")
    subprocess.run(
        ["openssl", "genrsa", "-out", priv, "2048"],
        check=True, capture_output=True
    )
    subprocess.run(
        ["openssl", "rsa", "-in", priv, "-pubout", "-out", pub],
        check=True, capture_output=True
    )
    os.chmod(priv, 0o600)
    print(f"[KEY]   Private: {priv}")
    print(f"[KEY]   Public:  {pub}")
    return priv, pub


def get_pubkey_der_hash(pub_pem_path: str) -> bytes:
    """Compute SHA-256(DER-encoded public key)."""
    der = subprocess.check_output(
        ["openssl", "rsa", "-in", pub_pem_path, "-pubin",
         "-outform", "DER", "-pubout"],
        stderr=subprocess.DEVNULL
    )
    return hashlib.sha256(der).digest()


def build_otp_record(pub_pem: str, min_version: int,
                     hmac_key: bytes) -> bytes:
    """
    Build the 256-byte OTP record:
      0x00  Magic (4B)         0x53424F54
      0x04  Min version (4B)
      0x08  Default flags (4B) 0
      0x0C  Reserved (20B)     0x00
      0x20  PubKey hash (32B)  SHA-256(DER(pubkey))
      0x40  HMAC key (32B)
      0x60  Reserved (160B)    0xFF
    """
    rec = bytearray(b'\xFF' * 256)

    struct.pack_into("<I", rec, 0x00, OTP_SBOT_MAGIC)
    struct.pack_into("<I", rec, 0x04, min_version)
    struct.pack_into("<I", rec, 0x08, 0)               # default flags
    rec[0x0C:0x20] = b'\x00' * 20                      # reserved

    pubkey_hash = get_pubkey_der_hash(pub_pem)
    rec[0x20:0x40] = pubkey_hash
    rec[0x40:0x60] = hmac_key
    rec[0x60:0x100] = b'\xFF' * 160                    # unwritten OTP

    return bytes(rec)


def save_provisioning_summary(pub_pem: str, hmac_key: bytes,
                               otp_record: bytes):
    summary_path = KEYS_DIR / "provisioning_summary.txt"
    pubkey_hash  = get_pubkey_der_hash(pub_pem)

    with open(summary_path, "w") as f:
        f.write("STA8600 Secure Boot Provisioning Summary\n")
        f.write("=" * 42 + "\n\n")
        f.write(f"Public key:        {pub_pem}\n")
        f.write(f"PubKey SHA-256:    {pubkey_hash.hex()}\n")
        f.write(f"HMAC write key:    {hmac_key.hex()}\n")
        f.write(f"OTP record SHA256: {hashlib.sha256(otp_record).hexdigest()}\n")
        f.write("\nOTP record (hex):\n")
        for i in range(0, 256, 16):
            row = otp_record[i:i+16]
            f.write(f"  {i:04X}  {row.hex(' ')}\n")

    print(f"[PROV] Summary saved to {summary_path}")
    # Also save the HMAC key as a raw binary for use with --hmac-key
    hmac_path = KEYS_DIR / "hmac_write.key"
    hmac_path.write_bytes(hmac_key)
    os.chmod(str(hmac_path), 0o600)
    print(f"[PROV] HMAC key saved to {hmac_path}")


def main():
    parser = argparse.ArgumentParser(
        description="STA8600 One-Time Secure Boot Provisioning"
    )
    parser.add_argument("--dev",         default="/dev/ttyUSB0")
    parser.add_argument("--baud",        type=int, default=115200)
    parser.add_argument("--min-version", type=int, default=1,
                        help="Minimum firmware version (anti-rollback)")
    parser.add_argument("--dry-run",     action="store_true",
                        help="Build OTP record but do NOT write to chip")
    args = parser.parse_args()

    print("=" * 50)
    print("  STA8600 Secure Boot Provisioning")
    print("  WARNING: OTP writes are IRREVERSIBLE")
    print("=" * 50)

    # 1. Keypair
    priv_pem, pub_pem = gen_keypair()

    # 2. Random HMAC write key
    hmac_key = os.urandom(32)
    print(f"[KEY] Generated HMAC write key: {hmac_key.hex()[:16]}...")

    # 3. OTP record
    otp_record = build_otp_record(pub_pem, args.min_version, hmac_key)
    pubkey_hash = get_pubkey_der_hash(pub_pem)
    print(f"[OTP] PubKey hash: {pubkey_hash.hex()}")
    print(f"[OTP] OTP record ({len(otp_record)} bytes) built")

    # 4. Save summary
    save_provisioning_summary(pub_pem, hmac_key, otp_record)

    if args.dry_run:
        print("[DRY-RUN] Not writing to chip.  OTP record saved in summary.")
        return

    # 5. Write to chip
    print(f"\n[PROV] About to write OTP to STA8600 on {args.dev}")
    print("[PROV] Type 'YES' to continue: ", end="", flush=True)
    if input().strip() != "YES":
        print("[PROV] Aborted.")
        return

    result = subprocess.run([
        TOOL,
        "-d", args.dev,
        "-b", str(args.baud),
        "--otp-write",
        "--pub", pub_pem,
        "--hmac-key", str(KEYS_DIR / "hmac_write.key"),
        # dummy image arg required by current CLI
        "/dev/null"
    ])
    if result.returncode == 0:
        print("[PROV] OTP provisioning COMPLETE.")
        print("[PROV] Store keys/priv.pem and keys/hmac_write.key SECURELY.")
        print("[PROV] NEVER flash a key signed with a different private key.")
    else:
        print("[PROV] Provisioning FAILED — check connection and BOOT pins.")


if __name__ == "__main__":
    main()
