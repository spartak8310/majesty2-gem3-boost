#!/usr/bin/env python3
"""
Extracts additional CVar details from base.dll and plugin DLLs,
plus exact disassembly of Matrix34, Matrix33, Quaternion, and Vec3 methods in geometry.dll,
and Multiplayer.dll usage of m2TimeBetweenQuantCommands / m2TimeBetweenQuantReports.
"""

import json
import os
import struct
from collections import Counter

import capstone
import pefile

GAME_DIR = "$HOME/Documents/Majesty2_Archive/Majesty 2 Collection"
WORK_DIR = "./scratch/majesty2-re"
SDK_JSON = os.path.join(WORK_DIR, "sdk/exports_by_dll.json")

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def disasm_func(pe, rva, max_bytes=256):
    base = pe.OPTIONAL_HEADER.ImageBase
    va = base + rva
    data = pe.get_data(rva, max_bytes)
    lines = []
    for ins in md.disasm(data, va):
        lines.append(f"0x{ins.address:08X}: {ins.mnemonic:8s} {ins.op_str}")
        if ins.mnemonic in ("ret", "retn"):
            break
    return lines


def read_cstr(pe, va, max_len=128):
    base = pe.OPTIONAL_HEADER.ImageBase
    rva = va - base
    if rva <= 0 or rva >= pe.OPTIONAL_HEADER.SizeOfImage:
        return None
    try:
        data = pe.get_data(rva, max_len)
        idx = data.find(b"\x00")
        if idx <= 0:
            return None
        s = data[:idx].decode("ascii")
        if all(32 <= ord(c) < 127 for c in s):
            return s
    except Exception:
        return None
    return None


def main():
    with open(SDK_JSON, "r") as f:
        sdk = json.load(f)

    print("All binaries in SDK:", list(sdk["binaries"].keys()))

    # 1. Scan all 44 binaries for xSet and xConsole CVars (including base.dll internal calls)
    pe_base = pefile.PE(os.path.join(GAME_DIR, "base.dll"), fast_load=True)
    base_ib = pe_base.OPTIONAL_HEADER.ImageBase
    # Find RVA of ??0eAbstract@xSet@@QAE@PBDPBVtype_info@@@Z and ??0eAtom@xConsole@@QAE@PBD@Z in base.dll
    target_vas = []
    for exp in sdk["binaries"]["base.dll"]["exports"]:
        if ("eAbstract@xSet" in exp["mangled"] or "eAtom@xConsole" in exp["mangled"]) and "??0" in exp["mangled"]:
            va = int(exp["va"], 16)
            target_vas.append((exp["mangled"], va))
            print("base.dll constructor:", exp["mangled"], hex(va))

    # Scan base.dll .text for E8 rel32 to target_vas or vtable assignments of eAbstract@xSet (0x10052...)
    text_sec = next(s for s in pe_base.sections if b".text" in s.Name)
    text_data = text_sec.get_data()
    text_va = base_ib + text_sec.VirtualAddress

    # Also find strings "con_debugs", "con_messages", "pro_log", "dbg_exceptions" in base.dll or other DLLs
    cvar_strings_to_locate = [
        b"con_debugs\x00",
        b"con_messages\x00",
        b"con_warnings\x00",
        b"con_errors\x00",
        b"pro_log\x00",
        b"dbg_exceptions\x00",
    ]
    located_special = {}
    for bin_name, bin_info in sdk["binaries"].items():
        bpath = os.path.join(GAME_DIR, bin_name)
        if not os.path.exists(bpath):
            continue
        raw = open(bpath, "rb").read()
        pe = pefile.PE(bpath, fast_load=True)
        ib = pe.OPTIONAL_HEADER.ImageBase
        for target_s in cvar_strings_to_locate:
            idx = raw.find(target_s)
            if idx != -1:
                rva = pe.get_rva_from_offset(idx)
                va = ib + rva
                # find push <va> in raw
                push_pat = b"\x68" + struct.pack("<I", va)
                p_idx = raw.find(push_pat)
                call_va = ib + pe.get_rva_from_offset(p_idx) if p_idx != -1 else 0
                sname = target_s[:-1].decode()
                located_special[sname] = {
                    "binary": bin_name,
                    "string_va": f"0x{va:08X}",
                    "init_va": f"0x{call_va:08X}",
                    "disasm": disasm_func(pe, pe.get_rva_from_offset(p_idx) - 16, 80) if p_idx != -1 else [],
                }
                print(f"Found {sname} in {bin_name} at string_va=0x{va:08X}, init_va=0x{call_va:08X}")

    # Let's check base.dll around init_va of con_debugs to extract ALL base.dll CVars!
    base_cvars = []
    if "con_debugs" in located_special:
        init_rva = int(located_special["con_debugs"]["init_va"], 16) - base_ib
        # Scan the static initializer region around init_rva in base.dll
        start_scan = max(0, init_rva - 0x4000)
        end_scan = min(pe_base.OPTIONAL_HEADER.SizeOfImage, init_rva + 0x4000)
        region = pe_base.get_data(start_scan, end_scan - start_scan)
        for ins in md.disasm(region, base_ib + start_scan):
            if ins.mnemonic == "push" and ins.op_str.startswith("0x"):
                val = int(ins.op_str, 16)
                s = read_cstr(pe_base, val)
                if s and ("_" in s or s.startswith(("con", "pro", "dbg", "mem", "log", "sys", "io"))):
                    base_cvars.append({"name": s, "push_va": f"0x{ins.address:08X}", "str_va": f"0x{val:08X}"})
    print(f"Found {len(base_cvars)} CVars in base.dll static initializers:", [c["name"] for c in base_cvars])

    # 2. Check all remaining engine binaries in sdk["binaries"] for x87/SSE counts
    plugin_fpu_stats = {}
    for bin_name, bin_info in sdk["binaries"].items():
        if bin_name in ("advanced.dll", "dda.dll", "editor.dll", "entity.ed.dll", "features.dll", "gembase.dll", "net.dll", "script.dll", "traceable.dll", "ui.dll", "audio.dll", "app.dll", "Multiplayer.dll"):
            bpath = os.path.join(GAME_DIR, bin_name)
            pe = pefile.PE(bpath, fast_load=True)
            ib = pe.OPTIONAL_HEADER.ImageBase
            tsec = next((s for s in pe.sections if b".text" in s.Name), None)
            if not tsec:
                continue
            tdata = tsec.get_data()
            tva = ib + tsec.VirtualAddress
            x87_c = 0
            sse_c = 0
            tot_c = 0
            for ins in md.disasm(tdata, tva):
                tot_c += 1
                if ins.mnemonic.startswith(("fld", "fst", "fadd", "fsub", "fmul", "fdiv", "fchs", "fabs", "fsqrt", "fsin", "fcos", "fsincos", "fpatan", "fcom", "fucom", "fnstsw", "fild", "fist", "fxch")):
                    x87_c += 1
                elif ins.mnemonic.startswith(("addss", "subss", "mulss", "divss", "addps", "subps", "mulps", "divps", "movss", "movaps", "movups", "shufps", "comiss", "ucomiss", "cvtsi2ss", "cvtss2si", "sqrtss", "rsqrtss", "xorps")):
                    sse_c += 1
            plugin_fpu_stats[bin_name] = {"total": tot_c, "x87": x87_c, "sse": sse_c}
            print(f"Binary {bin_name:24s}: x87={x87_c:5d}, SSE={sse_c:4d}, total={tot_c:6d}")

    # 3. Inspect Matrix33, Matrix34, Quaternion, Vec3 methods in geometry.dll
    pe_geom = pefile.PE(os.path.join(GAME_DIR, "geometry.dll"), fast_load=True)
    geom_math_funcs = []
    for exp in sdk["binaries"]["geometry.dll"]["exports"]:
        m = exp["mangled"]
        if any(cls in m for cls in ("Matrix33", "Matrix34", "Quaternion", "Vec3", "Xform")):
            rva = int(exp["rva"], 16)
            dis = disasm_func(pe_geom, rva, 320)
            geom_math_funcs.append({
                "ordinal": exp["ordinal"],
                "rva": exp["rva"],
                "va": exp["va"],
                "mangled": m,
                "demangled": exp["demangled"],
                "disasm": dis,
            })

    # 4. Find where m2TimeBetweenQuantCommands (0x100931a4) and m2TimeBetweenQuantReports (0x10093144) are used in Multiplayer.dll
    pe_mp = pefile.PE(os.path.join(GAME_DIR, "Multiplayer.dll"), fast_load=True)
    mp_ib = pe_mp.OPTIONAL_HEADER.ImageBase
    mp_tsec = next(s for s in pe_mp.sections if b".text" in s.Name)
    mp_tdata = mp_tsec.get_data()
    mp_tva = mp_ib + mp_tsec.VirtualAddress

    mp_xrefs = {}
    for label, addr in [
        ("m2TimeBetweenQuantReports_obj", 0x10093144),
        ("m2TimeBetweenQuantReports_val", 0x10093170),
        ("m2QuantSmoothFactor_obj", 0x10093174),
        ("m2QuantSmoothFactor_val", 0x100931A0),
        ("m2TimeBetweenQuantCommands_obj", 0x100931A4),
        ("m2TimeBetweenQuantCommands_val", 0x100931D0),
        ("m2MaxTimeDiff_obj", 0x100931D4),
        ("m2MaxTimeDiff_val", 0x10093200),
    ]:
        pat = struct.pack("<I", addr)
        pos = 0
        hits = []
        while True:
            idx = mp_tdata.find(pat, pos)
            if idx == -1:
                break
            hit_va = mp_tva + idx
            rva_start = max(mp_tsec.VirtualAddress, (hit_va - mp_ib) - 32)
            hits.append({
                "hit_va": f"0x{hit_va:08X}",
                "context": disasm_func(pe_mp, rva_start, 96),
            })
            pos = idx + 1
        mp_xrefs[label] = hits
        print(f"Multiplayer.dll XREFs for {label} (0x{addr:08X}): {len(hits)} hits")

    # 5. Also check scene.dll exports related to Save/Load/Sync/Clone
    pe_scene = pefile.PE(os.path.join(GAME_DIR, "scene.dll"), fast_load=True)
    scene_sync_funcs = []
    for exp in sdk["binaries"]["scene.dll"]["exports"]:
        m = exp["mangled"]
        if any(k in m for k in ("Sync", "Save", "Load", "Preload", "Spawn", "Delete", "Quant")):
            rva = int(exp["rva"], 16)
            scene_sync_funcs.append({
                "ordinal": exp["ordinal"],
                "rva": exp["rva"],
                "va": exp["va"],
                "mangled": m,
                "demangled": exp["demangled"],
                "disasm": disasm_func(pe_scene, rva, 128)[:20],
            })

    out = {
        "located_special_cvars": located_special,
        "base_cvars": base_cvars,
        "plugin_fpu_stats": plugin_fpu_stats,
        "geom_math_funcs": geom_math_funcs,
        "multiplayer_quant_xrefs": mp_xrefs,
        "scene_sync_funcs": scene_sync_funcs[:40],
    }
    with open(os.path.join(WORK_DIR, "sdk/extra_re_details.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
    print("Saved sdk/extra_re_details.json")


if __name__ == "__main__":
    main()
