#!/usr/bin/env python3
"""m2_export_parity.py -- exact export-table parity check for Maj2 proxy DLLs.

Compares the SET of exported names AND their ordinals of a DLL against a
reference DLL.  Any difference in either direction, any stdcall-decorated
name (``Foo@N``), any duplicate name, or any ordinal mismatch => exit 1.

Motivation (t_v4_a1b_capture_export): the A1 capture DLL exported
``Direct3DCreate9@4`` instead of ``Direct3DCreate9``; Wine failed with
"Unimplemented function d3d9.dll.Direct3DCreate9".  The old verifier only
checked "name present", not "name matches exactly" -- this tool closes
that gap.

Usage:
    tools/m2_export_parity.py <dll> [--ref build/v4/d3d9_safe.dll]

Pure stdlib (no pefile dependency): parses the PE32/PE32+ optional header
data directory 0 (export table) directly.

Exit codes: 0 = parity, 1 = any discrepancy, 2 = input/parse error.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys

DECORATED_RE = re.compile(r"@\d+$")


class PEParseError(Exception):
    pass


def _rva_to_offset(rva: int, sections, hdr_end: int) -> int | None:
    """Map an RVA to a file offset using the section table."""
    for vaddr, vsize, rawptr, rawsize in sections:
        span = max(vsize, rawsize)
        if vaddr <= rva < vaddr + span:
            if rva - vaddr >= rawsize:
                return None  # virtual-only tail (e.g. .bss)
            return rawptr + (rva - vaddr)
    if rva < hdr_end:  # headers themselves are mapped at their RVA
        return rva
    return None


def parse_exports(path: str) -> list[tuple[str, int]]:
    """Return [(name, ordinal), ...] from the export name table, in file order."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        raise PEParseError(f"{path}: not an MZ image")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise PEParseError(f"{path}: bad PE signature")
    coff = e_lfanew + 4
    (machine, nsec, _ts, _ptrsym, _nsym, opt_size, _chars) = struct.unpack_from(
        "<HHIIIHH", data, coff
    )
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic not in (0x10B, 0x208):  # PE32 / PE32+
        raise PEParseError(f"{path}: unsupported optional-header magic 0x{magic:X}")
    pe32plus = magic == 0x208
    num_dd = struct.unpack_from("<I", data, opt + (108 if pe32plus else 92))[0]
    dd0 = opt + (112 if pe32plus else 96)  # data directory 0 = export table
    if num_dd < 1:
        raise PEParseError(f"{path}: no data directories")
    export_rva, _export_size = struct.unpack_from("<II", data, dd0)
    if export_rva == 0:
        return []  # no export table

    sec_off = opt + opt_size
    sections = []
    for i in range(nsec):
        (_name, vsize, vaddr, rawsize, rawptr) = struct.unpack_from(
            "<8sIIII", data, sec_off + 40 * i
        )
        sections.append((vaddr, vsize, rawptr, rawsize))
    hdr_end = sections[0][0] if sections else 0x1000

    eo = _rva_to_offset(export_rva, sections, hdr_end)
    if eo is None:
        raise PEParseError(f"{path}: cannot map export RVA 0x{export_rva:X}")
    (_chars, _ts, _mj, _mn, _name_rva, base, neat, nnames,
     _addrtbl_rva, nameptr_rva, ordtbl_rva) = struct.unpack_from(
        "<IIHHIIIIIII", data, eo
    )
    exports: list[tuple[str, int]] = []
    for i in range(nnames):
        n_off = _rva_to_offset(nameptr_rva + 4 * i, sections, hdr_end)
        o_off = _rva_to_offset(ordtbl_rva + 2 * i, sections, hdr_end)
        if n_off is None or o_off is None:
            raise PEParseError(f"{path}: name/ordinal table outside raw sections")
        name_rva_i = struct.unpack_from("<I", data, n_off)[0]
        ord_idx = struct.unpack_from("<H", data, o_off)[0]
        noff = _rva_to_offset(name_rva_i, sections, hdr_end)
        if noff is None:
            raise PEParseError(f"{path}: export name RVA 0x{name_rva_i:X} unmapped")
        end = data.index(b"\x00", noff)
        name = data[noff:end].decode("ascii", "replace")
        if ord_idx >= neat:
            raise PEParseError(f"{path}: name '{name}' ord index {ord_idx} >= EAT {neat}")
        exports.append((name, base + ord_idx))
    return exports


def check_parity(dll: str, ref: str, quiet: bool = False) -> int:
    """Print a report; return exit code (0 parity / 1 mismatch / 2 error)."""
    try:
        d_exports = parse_exports(dll)
        r_exports = parse_exports(ref)
    except (PEParseError, OSError, struct.error) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2

    problems: list[str] = []
    d_map: dict[str, int] = {}
    for name, ordinal in d_exports:
        if name in d_map:
            problems.append(f"duplicate export name '{name}' in {dll}")
        d_map[name] = ordinal
    r_map: dict[str, int] = {}
    for name, ordinal in r_exports:
        if name in r_map:
            problems.append(f"duplicate export name '{name}' in reference")
        r_map[name] = ordinal

    for name in sorted(d_map):
        if DECORATED_RE.search(name):
            problems.append(f"decorated export '{name}' in {dll} (stdcall @N leaked)")
    for name in sorted(r_map):
        if DECORATED_RE.search(name):
            problems.append(f"decorated export '{name}' in REFERENCE {ref}")

    only_dll = sorted(set(d_map) - set(r_map))
    only_ref = sorted(set(r_map) - set(d_map))
    ord_mism = sorted(
        (n, r_map[n], d_map[n]) for n in set(d_map) & set(r_map) if d_map[n] != r_map[n]
    )

    for n in only_dll:
        problems.append(f"export only in {dll}: '{n}' (ord {d_map[n]})")
    for n in only_ref:
        problems.append(f"export missing from {dll}: '{n}' (ref ord {r_map[n]})")
    for n, ro, do_ in ord_mism:
        problems.append(f"ordinal mismatch '{n}': ref {ro} vs dll {do_}")

    if not quiet:
        print(f"dll : {dll}  ({len(d_map)} named exports)")
        print(f"ref : {ref}  ({len(r_map)} named exports)")
    if problems:
        if not quiet:
            print(f"PARITY: MISMATCH ({len(problems)} problem(s))")
            for p in problems:
                print(f"  - {p}")
        else:
            print(f"PARITY: MISMATCH ({len(problems)} problem(s)): " + "; ".join(problems[:5]))
        return 1
    print("PARITY: OK" + ("" if quiet else f" -- names and ordinals identical ({len(d_map)})"))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dll", help="DLL to check")
    ap.add_argument(
        "--ref",
        default="build/v4/d3d9_safe.dll",
        help="reference DLL (default: build/v4/d3d9_safe.dll)",
    )
    ap.add_argument("-q", "--quiet", action="store_true", help="one-line output")
    args = ap.parse_args()
    return check_parity(args.dll, args.ref, quiet=args.quiet)


if __name__ == "__main__":
    sys.exit(main())
