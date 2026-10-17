#!/usr/bin/env python3
"""Decode the d3d9 proxy's in-process x86 guest profile into per-function self / inclusive tables.

Inputs (written by ProfilerThread in src/d3d9_proxy.cpp):
  /tmp/m2_x86_profile.txt  header lines '# module <name> base=XXXXXXXX end=XXXXXXXX' (runtime bases)
  /tmp/m2_x86_samples.bin  ProfSample[]: eip, esp, phase, nRets, ebpRet[4], rets[32]  (160 bytes each)

Function boundaries: MSVC pads functions with int3 (0xCC); a function starts at the first non-CC byte after a CC run
(at least 1 byte, 16-byte aligned start preferred), or at the nearest export, whichever is closer. Names come from
the module's exports (demangled to Class::Method form); unnamed functions are reported as sub_<VA> near <export>.

usage: m2_x86_profile.py [profile.txt] [samples.bin] [--top N]
"""
import bisect
import glob
import os
import re
import struct
import sys
from collections import Counter

import pefile

GAME = "/Applications/Gamez/Majesty 2.app/Contents/SharedSupport/prefix/drive_c/GOG Games/Majesty 2 Collection"
PREFIX = "/Applications/Gamez/Majesty 2.app/Contents/SharedSupport/prefix/drive_c/windows"
WINE_LIB = "/Applications/Gamez/Majesty 2.app/Contents/SharedSupport/wine/lib/wine/i386-windows"
SAMPLE_FMT = "<4I4I32I"
SAMPLE_SIZE = struct.calcsize(SAMPLE_FMT)
PHASES = {0: "other", 1: "quant", 2: "present"}


def demangle(n):
    """?Quant@eScene@xScene@@QAEXXZ -> xScene::eScene::Quant (good enough for MSVC member/free functions)."""
    if not n.startswith("?"):
        return n
    body = n[1:]
    if body.startswith("?"):          # operators / ctors: keep mangled
        return n
    head = body.split("@@", 1)[0]
    parts = head.split("@")
    return "::".join(reversed(parts))


def find_file(name):
    low = name.lower()
    for d in (GAME, f"{PREFIX}/syswow64", WINE_LIB):
        for p in glob.glob(f"{d}/*"):
            if os.path.basename(p).lower() == low:
                return p
    hits = [p for p in glob.glob(f"{PREFIX}/winsxs/**/{name}", recursive=True)]
    return hits[0] if hits else None


class Image:
    def __init__(self, name, base, end):
        self.name, self.base, self.end = name, base, end
        self.pe = None
        self.exports = []
        path = find_file(name)
        if not path:
            return
        try:
            pe = pefile.PE(path, fast_load=True)
            pe.parse_data_directories(directories=[0])
        except Exception:
            return
        self.pe = pe
        self.ib = pe.OPTIONAL_HEADER.ImageBase
        self.data = pe.__data__
        ex = {}
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.name:
                    ex.setdefault(e.address, e.name.decode(errors="replace"))
        self.exports = sorted(ex.items())
        self.export_rvas = [a for a, _ in self.exports]

    def byte(self, rva):
        try:
            off = self.pe.get_offset_from_rva(rva)
            return self.data[off]
        except Exception:
            return None

    def func_start(self, rva):
        """Walk back to the byte after the nearest int3 padding run or aligned ret/ret-imm16 (max 64 KB)."""
        if not self.pe:
            return None
        r = rva
        lo = max(rva - 0x10000, 0x1000)
        while r > lo:
            b = self.byte(r - 1)
            if b is None:
                return None
            if b == 0xCC and self.byte(r - 2) == 0xCC:
                break
            if r % 16 == 0 and self.byte(r) != 0xCC:
                if b == 0xC3:  # ret immediately before an aligned start
                    break
                if self.byte(r - 3) == 0xC2 and self.byte(r - 1) == 0x00:  # ret imm16 before an aligned start
                    break
            r -= 1
        e = self.nearest_export(rva)
        if e and e[0] > r:
            r = e[0]
        return r

    def nearest_export(self, rva):
        i = bisect.bisect_right(self.export_rvas, rva) - 1
        return self.exports[i] if i >= 0 else None


def load(profile, samples):
    mods = []
    for l in open(profile, encoding="utf-8", errors="replace"):
        m = re.match(r"# module (\S+)\s+base=([0-9A-F]+) end=([0-9A-F]+)", l)
        if m:
            mods.append(Image(m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    raw = open(samples, "rb").read()
    rows = [struct.unpack_from(SAMPLE_FMT, raw, i) for i in range(0, len(raw) - SAMPLE_SIZE + 1, SAMPLE_SIZE)]
    return mods, rows


class Resolver:
    def __init__(self, mods):
        self.mods = sorted(mods, key=lambda m: m.base)
        self.cache = {}

    def mod(self, va):
        for m in self.mods:
            if m.base <= va < m.end:
                return m
        return None

    def func(self, va):
        """-> (key, label) for the function containing va."""
        if va in self.cache:
            return self.cache[va]
        m = self.mod(va)
        if not m:
            r = (va, f"{va:08X} <no module>")
        else:
            rva = va - m.base
            fs = m.func_start(rva)
            if fs is None:
                r = (va & ~0xFFF, f"{m.name}+{rva & ~0xFFF:06X} (no image on disk)")
            else:
                e = m.nearest_export(rva)
                disk_va = (m.ib if m.pe else m.base) + fs
                if e and e[0] == fs:
                    name = demangle(e[1])
                elif e:
                    name = f"sub_{disk_va:08X} (after {demangle(e[1])}+0x{fs - e[0]:X})"
                else:
                    name = f"sub_{disk_va:08X}"
                r = (m.base + fs, f"{m.name}!{name}  [rva {fs:06X}, va {disk_va:08X}]")
        self.cache[va] = r
        return r


def table(title, counter, labels, denom, top):
    print(f"\n== {title} ==")
    for i, (k, c) in enumerate(counter.most_common(top), 1):
        print(f"{i:3d} {c:6d} {100.0 * c / max(denom, 1):6.2f}%  {labels[k]}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 40
    if "--top" in sys.argv:
        args.remove(str(top))
    profile = args[0] if args else "/tmp/m2_x86_profile.txt"
    samples = args[1] if len(args) > 1 else "/tmp/m2_x86_samples.bin"
    mods, rows = load(profile, samples)
    res = Resolver(mods)
    n = len(rows)
    phase = Counter(r[2] for r in rows)
    print(f"samples={n}  " + "  ".join(f"{PHASES[p]}={100.0 * c / n:.1f}%" for p, c in sorted(phase.items())))

    labels = {}
    self_f, self_mod, incl_f = Counter(), Counter(), Counter()
    by_phase = {p: Counter() for p in PHASES}
    for r in rows:
        eip, _, ph, nrets = r[:4]
        ebp_rets = r[4:8]
        rets = r[8:8 + nrets]
        k, lab = res.func(eip)
        labels[k] = lab
        self_f[k] += 1
        by_phase[ph][k] += 1
        m = res.mod(eip)
        self_mod[m.name if m else "?"] += 1
        seen = {k}
        for a in list(rets) + [x for x in ebp_rets if x]:
            kk, ll = res.func(a)
            labels[kk] = ll
            if kk not in seen:
                seen.add(kk)
                incl_f[kk] += 1
        incl_f[k] += 1

    print("\n== self time by module ==")
    for name, c in self_mod.most_common():
        print(f"{c:6d} {100.0 * c / n:6.2f}%  {name}")
    table(f"top-{top} functions by SELF samples", self_f, labels, n, top)
    table(f"top-{top} functions by INCLUSIVE samples (EIP + stack-scan + EBP-chain callers)", incl_f, labels, n, top)
    for p in (1, 0):
        table(f"top-{min(top, 25)} SELF inside phase '{PHASES[p]}'", by_phase[p], labels, phase.get(p, 0), min(top, 25))


if __name__ == "__main__":
    main()
