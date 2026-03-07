#!/usr/bin/env python3
"""
Package firmware with OTA header for validated updates.

Usage:
    # Unsigned package (v1 header, 64 bytes)
    python3 package_firmware.py firmware.bin --version 1.0.0 --board touch-lcd-349 -o firmware.ota

    # Signed package (v2 header, 128 bytes) — requires ECDSA P-256 private key
    python3 package_firmware.py firmware.bin --sign keys/ota_signing_key.pem --version 1.0.0 -o firmware.ota

    # Verify an existing .ota file
    python3 package_firmware.py firmware.ota --verify

    # Verify signature against public key
    python3 package_firmware.py firmware.ota --verify --pubkey keys/ota_public_key.pem
"""

import os
import sys
import struct
import binascii
import argparse
import hashlib
import time


MAGIC = 0x4154304F  # 'OT0A'
HEADER_VER_UNSIGNED = 1
HEADER_VER_SIGNED = 2
HEADER_SIZE = 64
SIGNATURE_SIZE = 64
FLAG_SIGNED = 0x01
FLAG_ENCRYPTED = 0x02


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def encrypt_firmware(fw_data: bytes, key: bytes) -> tuple:
    """Encrypt firmware with AES-256-CTR. Returns (iv + ciphertext, iv)."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    iv = os.urandom(16)
    cipher = Cipher(algorithms.AES(key), modes.CTR(iv))
    encryptor = cipher.encryptor()
    ciphertext = encryptor.update(fw_data) + encryptor.finalize()
    return iv + ciphertext, iv


def load_encryption_key(key_path: str) -> bytes:
    """Load 32-byte AES-256 encryption key from file (hex or raw)."""
    with open(key_path, 'rb') as f:
        data = f.read().strip()
    # Try hex first
    try:
        key = bytes.fromhex(data.decode('ascii'))
        if len(key) == 32:
            return key
    except (ValueError, UnicodeDecodeError):
        pass
    # Raw binary
    if len(data) == 32:
        return data
    raise ValueError(f"Encryption key must be 32 bytes (got {len(data)})")


def package(fw_path: str, version: str, board: str, output: str,
            sign_key_path: str = None, encrypt_key_path: str = None):
    with open(fw_path, 'rb') as f:
        fw_data = f.read()

    is_encrypted = encrypt_key_path is not None
    is_signed = sign_key_path is not None

    # Encrypt firmware if requested (encrypt-then-sign)
    # CRC32 and signature cover the encrypted payload (IV + ciphertext)
    payload = fw_data
    if is_encrypted:
        enc_key = load_encryption_key(encrypt_key_path)
        payload, iv = encrypt_firmware(fw_data, enc_key)
        print(f"  Encrypted: AES-256-CTR, IV={iv.hex()[:16]}...")

    fw_crc = crc32(payload)

    # Build 64-byte header
    hdr_ver = HEADER_VER_SIGNED if is_signed else HEADER_VER_UNSIGNED
    flags = (FLAG_SIGNED if is_signed else 0) | (FLAG_ENCRYPTED if is_encrypted else 0)
    ver_bytes = version.encode()[:24].ljust(24, b'\x00')
    board_bytes = board.encode()[:16].ljust(16, b'\x00')
    # Reserved bytes: first 4 = build timestamp (uint32 little-endian), last 4 = zero
    build_ts = int(time.time())
    reserved = struct.pack('<I', build_ts) + b'\x00' * 4

    header = struct.pack('<IHHII', MAGIC, hdr_ver, flags, len(payload), fw_crc)
    header += ver_bytes
    header += board_bytes
    header += reserved
    assert len(header) == HEADER_SIZE

    signature = b''
    if is_signed:
        from cryptography.hazmat.primitives.asymmetric import ec, utils
        from cryptography.hazmat.primitives import hashes, serialization

        with open(sign_key_path, 'rb') as f:
            private_key = serialization.load_pem_private_key(f.read(), password=None)

        # Sign: SHA-256(payload) — covers encrypted data if encrypted
        der_sig = private_key.sign(payload, ec.ECDSA(hashes.SHA256()))

        # Decode DER signature to raw r,s (32 bytes each)
        r, s = utils.decode_dss_signature(der_sig)
        signature = r.to_bytes(32, 'big') + s.to_bytes(32, 'big')
        assert len(signature) == SIGNATURE_SIZE

    with open(output, 'wb') as f:
        f.write(header)
        if is_signed:
            f.write(signature)
        f.write(payload)

    total = len(header) + len(signature) + len(payload)
    print(f"Packaged: {output}")
    print(f"  Version: {version}")
    print(f"  Board: {board}")
    print(f"  Firmware: {len(fw_data)} bytes" + (f" -> {len(payload)} bytes encrypted" if is_encrypted else ""))
    print(f"  CRC32: 0x{fw_crc:08X}")
    print(f"  Signed: {'yes (ECDSA P-256)' if is_signed else 'no'}")
    print(f"  Encrypted: {'yes (AES-256-CTR)' if is_encrypted else 'no'}")
    print(f"  Build time: {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(build_ts))}")
    print(f"  Header: {len(header) + len(signature)} bytes (v{hdr_ver})")
    print(f"  Total: {total} bytes ({total / 1024:.1f} KB)")


def verify(ota_path: str, pubkey_path: str = None):
    with open(ota_path, 'rb') as f:
        all_data = f.read()

    if len(all_data) < HEADER_SIZE:
        print("ERROR: File too small for OTA header")
        return False

    header_data = all_data[:HEADER_SIZE]
    magic, hdr_ver, flags, fw_size, fw_crc = struct.unpack('<IHHII', header_data[:16])
    version = header_data[16:40].split(b'\x00')[0].decode()
    board = header_data[40:56].split(b'\x00')[0].decode()

    is_signed = (hdr_ver == HEADER_VER_SIGNED) and (flags & FLAG_SIGNED)
    total_header = HEADER_SIZE + (SIGNATURE_SIZE if is_signed else 0)

    print(f"OTA Header:")
    print(f"  Magic: 0x{magic:08X} {'OK' if magic == MAGIC else 'BAD'}")
    print(f"  Header version: {hdr_ver}")
    is_encrypted = bool(flags & FLAG_ENCRYPTED)
    print(f"  Flags: 0x{flags:04X} (signed={'yes' if is_signed else 'no'}, encrypted={'yes' if is_encrypted else 'no'})")
    print(f"  Version: {version}")
    print(f"  Board: {board}")
    # Parse build timestamp from reserved bytes
    reserved_data = header_data[56:64]
    build_ts = struct.unpack('<I', reserved_data[:4])[0]
    if build_ts > 1600000000:  # Sanity check: after 2020
        print(f"  Build time: {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(build_ts))}")
    print(f"  Firmware size: {fw_size} bytes")
    print(f"  Firmware CRC32: 0x{fw_crc:08X}")

    if magic != MAGIC:
        print("ERROR: Invalid magic")
        return False

    fw_data = all_data[total_header:]
    if len(fw_data) != fw_size:
        print(f"ERROR: Size mismatch (header says {fw_size}, file has {len(fw_data)})")
        return False

    actual_crc = crc32(fw_data)
    if actual_crc != (fw_crc & 0xFFFFFFFF):
        print(f"ERROR: CRC mismatch (header 0x{fw_crc:08X}, actual 0x{actual_crc:08X})")
        return False

    print("  CRC32: VERIFIED")

    if is_signed:
        sig_data = all_data[HEADER_SIZE:HEADER_SIZE + SIGNATURE_SIZE]
        r = int.from_bytes(sig_data[:32], 'big')
        s = int.from_bytes(sig_data[32:64], 'big')
        print(f"  Signature: r=0x{r:064x}")
        print(f"             s=0x{s:064x}")

        if pubkey_path:
            from cryptography.hazmat.primitives.asymmetric import ec, utils
            from cryptography.hazmat.primitives import hashes, serialization

            with open(pubkey_path, 'rb') as f:
                pub_data = f.read()

            # Support: public PEM, private PEM (extract public), C header (.h)
            if b'BEGIN PUBLIC KEY' in pub_data:
                public_key = serialization.load_pem_public_key(pub_data)
            elif b'BEGIN' in pub_data and b'PRIVATE' in pub_data:
                # Private key PEM — extract public key from it
                private_key = serialization.load_pem_private_key(pub_data, password=None)
                public_key = private_key.public_key()
            else:
                # C header file — extract hex bytes
                import re
                hex_bytes = re.findall(r'0x([0-9a-fA-F]{2})', pub_data.decode())
                if not hex_bytes:
                    print("ERROR: No key bytes found in pubkey file")
                    return False
                raw_key = bytes(int(h, 16) for h in hex_bytes)
                public_key = ec.EllipticCurvePublicKey.from_encoded_point(
                    ec.SECP256R1(), raw_key
                )

            der_sig = utils.encode_dss_signature(r, s)
            sign_data = fw_data  # Signature covers firmware data only
            try:
                public_key.verify(der_sig, sign_data, ec.ECDSA(hashes.SHA256()))
                print("  Signature: VERIFIED")
            except Exception as e:
                print(f"  Signature: INVALID ({e})")
                return False
        else:
            print("  Signature: present (use --pubkey to verify)")

    print("VERIFIED: Header and CRC OK" + (" + signature valid" if is_signed and pubkey_path else ""))
    return True


def main():
    parser = argparse.ArgumentParser(description='OTA firmware packager')
    parser.add_argument('firmware', help='Input firmware.bin (or .ota for --verify)')
    parser.add_argument('-v', '--version', default='0.0.0', help='Firmware version')
    parser.add_argument('-b', '--board', default='any', help='Target board')
    parser.add_argument('-o', '--output', help='Output .ota file')
    parser.add_argument('--sign', metavar='KEY', help='Sign with ECDSA P-256 private key (.pem)')
    parser.add_argument('--encrypt', metavar='KEY', help='Encrypt with AES-256-CTR key file (32 bytes hex or raw)')
    parser.add_argument('--verify', action='store_true', help='Verify an .ota file')
    parser.add_argument('--pubkey', help='Public key for signature verification (.pem or .h)')
    args = parser.parse_args()

    if args.verify:
        ok = verify(args.firmware, args.pubkey)
        sys.exit(0 if ok else 1)
    else:
        output = args.output or args.firmware.replace('.bin', '.ota')
        package(args.firmware, args.version, args.board, output, args.sign, args.encrypt)


if __name__ == '__main__':
    main()
