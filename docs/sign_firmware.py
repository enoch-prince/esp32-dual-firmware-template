#!/usr/bin/env python3
"""
sign_firmware.py
~~~~~~~~~~~~~~~~
Build-server utility: signs a firmware binary with ECDSA-P256 and
produces a ready-to-upload manifest entry.

Usage:
    python sign_firmware.py \
        --binary  build_fw_a/esp32s2_dual_fw.bin \
        --key     keys/ecdsa_private.pem \
        --version 1.2.0 \
        --slot    A \
        --url     https://ota.example.com/fw/firmware_a_1.2.0.bin

Requirements:
    pip install cryptography
"""

import argparse
import base64
import hashlib
import json
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.backends import default_backend
except ImportError:
    sys.exit("Install required package:  pip install cryptography")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sign_ecdsa_p256(data: bytes, private_key_pem: bytes) -> str:
    """Returns base64-encoded DER signature."""
    key = serialization.load_pem_private_key(
        private_key_pem, password=None, backend=default_backend()
    )
    sig_der = key.sign(data, ec.ECDSA(hashes.SHA256()))
    return base64.b64encode(sig_der).decode()


def main():
    parser = argparse.ArgumentParser(description="Sign firmware for OTA")
    parser.add_argument("--binary",  required=True, help="Path to .bin file")
    parser.add_argument("--key",     required=True, help="Path to ECDSA private key PEM")
    parser.add_argument("--version", required=True, help="Semver string e.g. 1.2.0")
    parser.add_argument("--slot",    required=True, choices=["A", "B"],
                        help="Firmware slot: A or B")
    parser.add_argument("--url",     required=True, help="HTTPS download URL for the binary")
    parser.add_argument("--out",     default="manifest_entry.json",
                        help="Output JSON file (default: manifest_entry.json)")
    args = parser.parse_args()

    binary = Path(args.binary).read_bytes()
    private_key_pem = Path(args.key).read_bytes()

    print(f"Binary size : {len(binary):,} bytes")

    sha = sha256_hex(binary)
    print(f"SHA-256     : {sha}")

    sig = sign_ecdsa_p256(binary, private_key_pem)
    print(f"Signature   : {sig[:40]}...")

    fw_key = f"firmware_{args.slot.lower()}"
    entry = {
        fw_key: {
            "version":      args.version,
            "url":          args.url,
            "sha256":       sha,
            "signature":    sig,
            "min_hw_version": 1,
        }
    }

    out_path = Path(args.out)
    out_path.write_text(json.dumps(entry, indent=2))
    print(f"\nManifest entry written to: {out_path}")
    print("Merge this into your hosted manifest.json.")


if __name__ == "__main__":
    main()
