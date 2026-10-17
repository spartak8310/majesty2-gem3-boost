#!/usr/bin/env python3
"""
Majesty 2 (GEM 3 Engine) Closed/Encrypted .PAK Unpacker & Decryptor
===================================================================
Reverse-engineered from:
  - io.dll:
      ?PurgeEmpty@xFile@xSystem@xIo@@YAXABV?$eName@W4eFileNameTag@xIo@@@3@@Z (RVA 0x00014E80)
      ?MountZip@xRepository@xIo@@YAX... (RVA 0x00014850)
      minizip zopen_file callback (VA 0x10006100)
  - Majesty2.exe:
      FileSystem::Crypt::StreamFactory (VA 0x00731090, vtable 0x008C1DD0)
      FileSystem::Crypt::PakSetKeyCrypters::SetKey (VA 0x007312F0, vtable 0x008C1DC4)
      FileSystem::Secure::PakExeDecryptPassword::GetPassword (VA 0x0072A0C0, vtable 0x008C1DF0)
      FileSystem::ModuleCRC singleton (VA 0x0072B5E0, seed = 0x4CE52E73)
      FileSystem::Crypt::FileRijndaelStream (VA 0x00731410, Underflow VA 0x00732980)
      FileSystem::Crypt::IDataCrypter<Rijndael>::Decrypt (VA 0x00732360)

Algorithm Summary:
  1. Key Lookup:
     md5_digest = MD5(ascii_lowercase(basename(pak_path)) + uint32_le(file_size))
     Matched against 32 obfuscated slots at VA 0x0072A441 (each 0x82 bytes of shellcode),
     where both the 16-byte MD5 digest and 16-byte AES-128 key are XOR-masked with
     the 32-bit ModuleCRC constant 0x4CE52E73.
  2. Block Decryption (16-byte independent blocks):
     For each 16-byte block C[0..15]:
       a) Pre-whitening (forward neighbor XOR):
          for i in 0..14: C[i] ^= C[i+1]
       b) Standard AES-128-ECB block decrypt:
          P = AES_128_ECB_Decrypt(key, C)
       c) Post-whitening (backward neighbor XOR):
          for i in 15..1: P[i] ^= P[i-1]
     The resulting stream is a standard PKZIP archive (with zero padding to 16-byte alignment).
"""

import argparse
import hashlib
import io
import json
import os
import struct
import sys
import zipfile
from pathlib import Path
from Crypto.Cipher import AES

MODULE_CRC_MASK = 0x4CE52E73

# All 32 raw slots extracted from Majesty2.exe @ VA 0x0072A441..0x0072B480
# Each entry is (part1_hex, part2_hex) where:
#   MD5_digest  = part1 ^ (0x4CE52E73 * 4)
#   AES_128_key = part2 ^ (0x4CE52E73 * 4)
RAW_SLOTS = [
    ("274caadbf390a42e7816faf58c110d05", "ee779687194a5710a692ad737bb20376"),
    ("5ad2bd9b4050299ab36fb944244ff43a", "42f7a03b7c92590631f02becfae577ea"),
    ("6c58111fef49f64d0719775137066245", "2fc09f8e7b6801d5a29ed5e933806555"),
    ("ca6485bf952a1e4f89ef89cd00e7886a", "f6775f4ffce2b19feb4183c0d13c51df"),
    ("1da0fd5634a2a4f5bcca40abcdfc087b", "94472a94ea0381c7615d1ffef6640d9b"),
    ("203c2ebd5d9f150d1c60f4fa392b060f", "506e489abc5376a59ac70bfcc675442d"),
    ("f83021c2f9066ba6bc7bee671d3ae491", "b8b14a6fe80adc0eca32153a3ad5337e"),
    ("af968af26510a91b1c693f5fb14d894d", "bf27832fa424e9127a3037c21df7a8cb"),
    ("0709fc7881f77f343c04b94d15dce84e", "a7ea0684505ccffb2a1bc17f12d6169c"),
    ("daca4ca7f81d908ec7f678bccac32284", "f9633934426940520c0d885c00428cb7"),
    ("3ff5be98a2345cef60a3f62f97a16684", "219c84b97001f35e2e935016fe41e861"),
    ("dccb1d8237f35208f4da5f8bef2c7f97", "c454af77ce7faa382a99eea6b44eb54c"),
    ("5d4317ff7c3d8b53c762e0e9197436f7", "f802bbf1371694d2aefaf787b0eb6eb8"),
    ("f983fe35a344034fca77c4212b40f26c", "78c96b140508ca62c62a181ca2016e30"),
    ("ac0c1dd2f3002e45a0814d0fe24d8330", "6a4df9b80975bc24cb128423d940efc1"),
    ("0bd0d1a2faa5976ffc1aa75447aeea47", "e6e82ca2e9cb0de19ef38aa2f1da8f6e"),
    ("d45e3028f3fac842b4957998f3db7db8", "4c37d1ec1ad631417aa41883484dc33e"),
    ("6464f00bc7a0e22818a432327b9e950e", "d28c97a7d479adcbef3927ce12834a8b"),
    ("4c6c1cb372018582eb99d34e068f394a", "1f5692eb547b83778cb305f1aefcd049"),
    ("8aadedb6c7b191cd2361c00bf1856515", "ba35a6aba92b832d7f151b03d9073070"),
    ("ab7b98eaa61583b590a215d96d12bfe4", "6087eb1a0012ec6dfc1fdadb9211a7f8"),
    ("7780b799c8566201a86a0a99496f60e9", "874a7372fb4bcdd994bf385343300b49"),
    ("ef493cfec108bced671c16c75db8ea09", "dee3ddfc9e5753732f225b4a38b1179c"),
    ("7a70d3ffeabd208ee11c415b7c7869d5", "d7f8fac33c92792bcf10c109b3e3e987"),
    ("a3f8cb9d3bac9c8ff4df043eab3c4135", "fb314cc242e8e4ef9a4e8489e00f671b"),
    ("6ab4a133cbf79448b37431473828f282", "4523a68c0f18ddf96475fdcac99d314a"),
    ("af0a062e7553552f88bccce9ab9e91b9", "7cf7c8a2e0794935ad065bc648b1f323"),
    ("061eece69a6aec13b29e5b0b11177d79", "af9c17e78848f86ed92e7ada85b133bf"),
    ("8e2c799b6a7f4c2a75e3046e7128d5a3", "89a24f1ea12245a20bd683a9e19432bb"),
    ("8a79ef2d24f4744be888e181675fc3a7", "1ded36d09ab8e5366fd845581524e0a5"),
    ("c051071df488542c6342642694053e9d", "801f034c3534dc174b4f82c6b7904422"),
    ("5b66eb06a4bcdb5a3ec1ec4ff2b063e0", "e3f69bbb2a28da42cc475a5943ad5c9a"),
]


def xor_mask_16(data16: bytes, mask32: int = MODULE_CRC_MASK) -> bytes:
    w0, w1, w2, w3 = struct.unpack("<4I", data16)
    return struct.pack("<4I", w0 ^ mask32, w1 ^ mask32, w2 ^ mask32, w3 ^ mask32)


def get_key_table():
    table = {}
    for idx, (p1_hex, p2_hex) in enumerate(RAW_SLOTS):
        md5_expected = xor_mask_16(bytes.fromhex(p1_hex))
        aes_key = xor_mask_16(bytes.fromhex(p2_hex))
        table[md5_expected] = (idx, aes_key)
    return table


def derive_pak_key(pak_path: str, file_size: int = None):
    if file_size is None:
        file_size = os.path.getsize(pak_path)
    basename_lower = os.path.basename(pak_path).lower().encode("ascii")
    md5_digest = hashlib.md5(basename_lower + struct.pack("<I", file_size)).digest()
    table = get_key_table()
    if md5_digest in table:
        slot_idx, key = table[md5_digest]
        return slot_idx, md5_digest, key
    return None, md5_digest, None


def decrypt_pak_bytes(data: bytes, key: bytes) -> bytes:
    """
    Fast decryption of Majesty 2 closed .pak archive bytes.
    Implements:
      1. Pre-whitening:  for i in 0..14: block[i] ^= block[i+1]
      2. AES-128-ECB:    block = AES_Decrypt(key, block)
      3. Post-whitening: for i in 15..1: block[i] ^= block[i-1]
    """
    if len(data) % 16 != 0:
        raise ValueError(f"Encrypted .pak size ({len(data)}) is not a multiple of 16 bytes")

    n = len(data)
    # Step 1: Forward neighbor XOR within each 16-byte block
    # Every byte at index (k % 16 != 15) is XORed with byte at k + 1
    pre = bytearray(data)
    for r in range(15):
        # Slice assignment across all blocks at once in CPython!
        pre[r:n:16] = bytes(a ^ b for a, b in zip(data[r:n:16], data[r + 1 : n : 16]))

    # Step 2: Standard AES-128-ECB decryption (C extension in PyCryptodome)
    cipher = AES.new(key, AES.MODE_ECB)
    mid = cipher.decrypt(bytes(pre))

    # Step 3: Backward neighbor XOR within each 16-byte block
    # Every byte at index (k % 16 != 0) is XORed with mid[k - 1]
    post = bytearray(mid)
    for r in range(1, 16):
        post[r:n:16] = bytes(a ^ b for a, b in zip(mid[r:n:16], mid[r - 1 : n : 16]))

    return bytes(post)


def encrypt_pak_bytes(plain_data: bytes, key: bytes) -> bytes:
    """
    Inverse operation: encrypts a standard ZIP buffer into a Majesty 2 closed .pak.
    """
    if len(plain_data) % 16 != 0:
        pad_len = 16 - (len(plain_data) % 16)
        plain_data = plain_data + (b"\x00" * pad_len)

    n = len(plain_data)
    # Inverse of Step 3 (cascading forward XOR within each 16-byte block)
    pre = bytearray(plain_data)
    for r in range(1, 16):
        pre[r:n:16] = bytes(a ^ b for a, b in zip(pre[r:n:16], pre[r - 1 : n : 16]))

    # Inverse of Step 2 (AES-128-ECB encrypt)
    cipher = AES.new(key, AES.MODE_ECB)
    mid = cipher.encrypt(bytes(pre))

    # Inverse of Step 1 (cascading backward XOR within each 16-byte block)
    post = bytearray(mid)
    for r in range(14, -1, -1):
        post[r:n:16] = bytes(a ^ b for a, b in zip(post[r:n:16], post[r + 1 : n : 16]))

    return bytes(post)


def unpack_pak(pak_path: str, out_dir: str, save_zip: bool = True) -> dict:
    with open(pak_path, "rb") as f:
        raw = f.read()

    is_plain_zip = raw[:2] == b"PK"
    slot_idx, md5_digest, key = derive_pak_key(pak_path, len(raw))

    info = {
        "file": pak_path,
        "basename": os.path.basename(pak_path),
        "size": len(raw),
        "is_encrypted": not is_plain_zip,
        "md5_input_digest": md5_digest.hex(),
        "slot_index": slot_idx,
        "aes_key": key.hex() if key else None,
        "extracted_files": 0,
    }

    if is_plain_zip:
        print(f"[*] {os.path.basename(pak_path)}: Plain PKZIP archive (slot={slot_idx}, key={info['aes_key']})")
        zip_bytes = raw
    else:
        if key is None:
            raise RuntimeError(f"No matching slot found in PakExeDecryptPassword table for {pak_path} (size={len(raw)})")
        print(
            f"[+] {os.path.basename(pak_path)}: Encrypted .PAK -> Slot #{slot_idx:02d} | "
            f"MD5={md5_digest.hex()} | AES-128 Key={key.hex()}"
        )
        zip_bytes = decrypt_pak_bytes(raw, key)
        if zip_bytes[:4] != b"PK\x03\x04":
            raise RuntimeError(f"Decryption failed for {pak_path}: magic is {zip_bytes[:4]!r}")

    os.makedirs(out_dir, exist_ok=True)
    if save_zip and not is_plain_zip:
        zip_out = os.path.join(out_dir, os.path.basename(pak_path) + ".decrypted.zip")
        with open(zip_out, "wb") as fz:
            fz.write(zip_bytes)
        info["decrypted_zip_path"] = zip_out

    extract_target = os.path.join(out_dir, os.path.splitext(os.path.basename(pak_path))[0])
    os.makedirs(extract_target, exist_ok=True)

    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        namelist = zf.namelist()
        info["extracted_files"] = len(namelist)
        info["sample_files"] = namelist[:25]
        zf.extractall(extract_target)

    print(f"    -> Unpacked {info['extracted_files']} entries to {extract_target}")
    return info


def main():
    parser = argparse.ArgumentParser(description="Decrypt and unpack Majesty 2 closed .pak archives")
    parser.add_argument(
        "--game-dir",
        default="$HOME/Documents/Majesty2_Archive/Majesty 2 Collection",
        help="Path to Majesty 2 Collection directory",
    )
    parser.add_argument(
        "--out-dir",
        default="./scratch/majesty2-re/unpacked",
        help="Output directory for decrypted .zip and extracted files",
    )
    parser.add_argument(
        "--all-encrypted",
        action="store_true",
        help="Also unpack encrypted text/sound localization .pak files",
    )
    parser.add_argument("paks", nargs="*", help="Specific .pak files to unpack")
    args = parser.parse_args()

    if args.paks:
        targets = args.paks
    else:
        targets = [
            os.path.join(args.game_dir, "resource.std/resource.std.pak"),
            os.path.join(args.game_dir, "resource.editor/resource.editor.pak"),
            os.path.join(args.game_dir, "localization.expansion_1/texts/texts.expansion_1.pak"),
        ]
        if args.all_encrypted:
            targets.extend([
                os.path.join(args.game_dir, "localization.expansion_2/texts/texts.expansion_2.pak"),
                os.path.join(args.game_dir, "localization.expansion_3/texts/texts.expansion_3.pak"),
                os.path.join(args.game_dir, "localization.expansion_2/sound/3d/3d.expansion_2.pak"),
            ])

    results = []
    for t in targets:
        if os.path.exists(t):
            res = unpack_pak(t, args.out_dir)
            results.append(res)
        else:
            print(f"[!] File not found: {t}", file=sys.stderr)

    # Also dump all 32 slots decoded table
    slots_dump = []
    for idx, (p1_hex, p2_hex) in enumerate(RAW_SLOTS):
        slots_dump.append({
            "slot": idx,
            "raw_part1": p1_hex,
            "raw_part2": p2_hex,
            "expected_md5": xor_mask_16(bytes.fromhex(p1_hex)).hex(),
            "aes_128_key": xor_mask_16(bytes.fromhex(p2_hex)).hex(),
        })

    report_path = os.path.join(args.out_dir, "pak_decryption_report.json")
    with open(report_path, "w", encoding="utf-8") as fp:
        json.dump({"module_crc_mask": hex(MODULE_CRC_MASK), "unpacked": results, "all_32_slots": slots_dump}, fp, indent=2)
    print(f"[+] Saved decryption report and 32-slot key table to {report_path}")


if __name__ == "__main__":
    main()
