#!/usr/bin/env python3
"""
Majesty 2 (GEM 3 Engine) — Task 4 & Task 5 Deep-Dive Analyzer:
1. Disassembles Task 4 targets (LoadFromNode, m2TimeBetweenQuantCommands, DummyFunction, scene.dll serialization).
2. Extracts all xSet / eAbstract@xSet console variables (CVars) across all engine DLLs and Majesty2.exe.
3. Audits x87 FPU vs SSE/SSE2 instruction ratios across geometry.dll, scene.dll, render.dll, pf.dll, Majesty2.exe,
   and disassembles hot math functions in geometry.dll.
4. Extracts and analyzes developer.con, level_of_detail.set, grass.set, m2GameConst.con from resource.pak.
"""

import json
import os
import struct
import zipfile
from collections import Counter, defaultdict

import capstone
import pefile

GAME_DIR = "$HOME/Documents/Majesty2_Archive/Majesty 2 Collection"
WORK_DIR = "./scratch/majesty2-re"
SDK_JSON = os.path.join(WORK_DIR, "sdk/exports_by_dll.json")

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True


def disasm_va_range(pe, start_va, end_va):
    base = pe.OPTIONAL_HEADER.ImageBase
    rva = start_va - base
    size = end_va - start_va
    data = pe.get_data(rva, size)
    lines = []
    for insn in md.disasm(data, start_va):
        lines.append(f"0x{insn.address:08X}: {insn.mnemonic:8s} {insn.op_str}")
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


def analyze_task4():
    print("=== [1] Analyzing Task 4 Disassembly Targets ===")
    results = {}

    # 1. Majesty2.exe LoadFromNode & mission.sav
    pe_m2 = pefile.PE(os.path.join(GAME_DIR, "Majesty2.exe"), fast_load=True)
    results["Majesty2_LoadFromNode"] = disasm_va_range(pe_m2, 0x00437210, 0x004373D0)
    results["Majesty2_mission_sav_1"] = disasm_va_range(pe_m2, 0x00412050, 0x00412130)
    results["Majesty2_mission_sav_2"] = disasm_va_range(pe_m2, 0x004E04B0, 0x004E0580)
    results["Majesty2_xIoProfiler_init"] = disasm_va_range(pe_m2, 0x00405A40, 0x00405B10)

    # 2. Multiplayer.dll m2TimeBetweenQuantCommands & m2TimeBetweenQuantReports
    pe_mp = pefile.PE(os.path.join(GAME_DIR, "Multiplayer.dll"), fast_load=True)
    results["Multiplayer_QuantVars"] = disasm_va_range(pe_mp, 0x1006D950, 0x1006DB20)

    # Find references to the global addresses storing m2TimeBetweenQuantCommands & Reports
    # Let's scan .text of Multiplayer.dll for references to 0x1006EB84 or the globals initialized there
    mp_base = pe_mp.OPTIONAL_HEADER.ImageBase
    for s in pe_mp.sections:
        if b".text" in s.Name:
            text_data = s.get_data()
            text_va = mp_base + s.VirtualAddress
            break

    # 3. ps.dll DummyFunction
    pe_ps = pefile.PE(os.path.join(GAME_DIR, "ps.dll"), fast_load=True)
    ps_base = pe_ps.OPTIONAL_HEADER.ImageBase
    results["ps_DummyFunction"] = disasm_va_range(pe_ps, ps_base + 0x000065A0, ps_base + 0x00006600)

    # 4. io.dll xIoProfiler
    pe_io = pefile.PE(os.path.join(GAME_DIR, "io.dll"), fast_load=True)
    io_base = pe_io.OPTIONAL_HEADER.ImageBase
    # Find xIoProfiler exports in io.dll
    pe_io.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    io_profiler_exports = {}
    for exp in pe_io.DIRECTORY_ENTRY_EXPORT.symbols:
        name = exp.name.decode("ascii", errors="ignore") if exp.name else ""
        if "Profiler" in name or "Logger" in name:
            va = io_base + exp.address
            io_profiler_exports[name] = {
                "rva": f"0x{exp.address:08X}",
                "va": f"0x{va:08X}",
                "disasm": disasm_va_range(pe_io, va, va + 0x70),
            }
    results["io_Profiler_exports"] = io_profiler_exports

    return results


def extract_cvars():
    print("=== [2] Extracting xSet / Console Variables Across Engine Binaries ===")
    target_bins = [
        "Majesty2.exe",
        "M2Editor.exe",
        "render.dll",
        "scene.dll",
        "ui.dll",
        "app.dll",
        "geometry.dll",
        "pf.dll",
        "io.dll",
        "audio.dll",
        "features.dll",
        "Multiplayer.dll",
        "video.dll",
        "base.dll",
        "device.dll",
        "localize.dll",
        "ps.dll",
        "render/r_d3d9.dll",
        "render/r_shader_d3d9.dll",
        "render/r_ shadow_d3d9.dll",
        "scene/s_environment.dll",
        "scene/s_fx.dll",
        "scene/s_grass.dll",
        "scene/s_landscape.dll",
        "scene/s_light.dll",
        "scene/s_physics.dll",
        "scene/s_postprocess.dll",
        "scene/s_shadow_map.dll",
        "scene/s_skin.dll",
        "scene/s_sky.dll",
        "scene/s_water.dll",
        "scene/s_wind.dll",
    ]

    cvars_by_bin = {}
    all_unique_cvars = {}

    for bin_name in target_bins:
        path = os.path.join(GAME_DIR, bin_name)
        if not os.path.exists(path):
            continue
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(
            directories=[
                pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
            ]
        )
        base = pe.OPTIONAL_HEADER.ImageBase

        # Find IAT slot for ??0eAbstract@xSet@@QAE@PBDPBVtype_info@@@Z or ??0eVar@xSet@@...
        xset_iat_vas = set()
        if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
            for entry in pe.DIRECTORY_ENTRY_IMPORT:
                for imp in entry.imports:
                    if imp.name and b"xSet" in imp.name:
                        xset_iat_vas.add(imp.address)

        # Also if this IS base.dll, find the export VA of ??0eAbstract@xSet@@QAE@PBDPBVtype_info@@@Z
        xset_direct_vas = set()
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if exp.name and b"eAbstract@xSet" in exp.name and b"??0" in exp.name:
                    xset_direct_vas.add(base + exp.address)

        text_sec = None
        for s in pe.sections:
            if b".text" in s.Name:
                text_sec = s
                break
        if not text_sec:
            continue

        text_data = text_sec.get_data()
        text_va = base + text_sec.VirtualAddress

        found_cvars = []
        seen_names = set()

        # Scan for calls to xSet constructor:
        # Pattern 1: FF 15 <IAT_VA> (call dword ptr [iat_va])
        for iat_va in xset_iat_vas:
            pat = b"\xff\x15" + struct.pack("<I", iat_va)
            pos = 0
            while True:
                idx = text_data.find(pat, pos)
                if idx == -1:
                    break
                call_va = text_va + idx
                # Disassemble 32 bytes before call_va to find pushed string literal and type_info or default value
                window_start = max(0, idx - 48)
                insns = list(md.disasm(text_data[window_start : idx + 24], text_va + window_start))
                pushes = []
                movs = []
                for ins in insns:
                    if ins.address < call_va and ins.mnemonic == "push":
                        pushes.append(ins.op_str)
                    elif ins.mnemonic == "mov":
                        movs.append((ins.address, ins.op_str))
                # Check pushed addresses for C-string and RTTI TypeDescriptor
                cvar_name = None
                cvar_type = None
                global_addr = None
                for p in reversed(pushes):
                    if p.startswith("0x"):
                        val = int(p, 16)
                        s_val = read_cstr(pe, val)
                        if s_val and not cvar_name and len(s_val) >= 2:
                            cvar_name = s_val
                        else:
                            # Check if val is a TypeDescriptor (points to vtable, +8 is mangled type name ".?AV..." or ".H", ".M", "._N")
                            td_name = read_cstr(pe, val + 8)
                            if td_name and td_name.startswith("."):
                                cvar_type = td_name
                # Also check mov ecx, 0x... before call for the global CVar object address
                for addr_m, op_s in reversed(movs):
                    if addr_m < call_va and op_s.startswith("ecx, 0x"):
                        global_addr = op_s.split(", ")[1]
                        break

                if cvar_name:
                    type_map = {
                        ".H": "int",
                        ".I": "uint",
                        ".M": "float",
                        ".N": "double",
                        "._N": "bool",
                        ".?AV?$eString@D@xBase@@": "string",
                        ".?AVeVec2@xGeom@@": "vec2",
                        ".?AVeVec3@xGeom@@": "vec3",
                    }
                    readable_type = type_map.get(cvar_type, cvar_type or "unknown")
                    entry = {
                        "name": cvar_name,
                        "type": readable_type,
                        "raw_type": cvar_type,
                        "call_va": f"0x{call_va:08X}",
                        "global_va": global_addr,
                    }
                    if cvar_name not in seen_names:
                        seen_names.add(cvar_name)
                        found_cvars.append(entry)
                        all_unique_cvars[cvar_name] = {**entry, "binary": bin_name}
                pos = idx + 1

        # Also scan for direct E8 rel32 calls if inside base.dll
        for target_va in xset_direct_vas:
            for idx in range(len(text_data) - 5):
                if text_data[idx] == 0xE8:
                    rel = struct.unpack_from("<i", text_data, idx + 1)[0]
                    call_va = text_va + idx
                    if call_va + 5 + rel == target_va:
                        window_start = max(0, idx - 48)
                        insns = list(md.disasm(text_data[window_start : idx + 24], text_va + window_start))
                        pushes = [ins.op_str for ins in insns if ins.address < call_va and ins.mnemonic == "push"]
                        cvar_name = None
                        cvar_type = None
                        for p in reversed(pushes):
                            if p.startswith("0x"):
                                val = int(p, 16)
                                s_val = read_cstr(pe, val)
                                if s_val and not cvar_name and len(s_val) >= 2:
                                    cvar_name = s_val
                                else:
                                    td_name = read_cstr(pe, val + 8)
                                    if td_name and td_name.startswith("."):
                                        cvar_type = td_name
                        if cvar_name and cvar_name not in seen_names:
                            seen_names.add(cvar_name)
                            entry = {
                                "name": cvar_name,
                                "type": cvar_type or "unknown",
                                "call_va": f"0x{call_va:08X}",
                            }
                            found_cvars.append(entry)
                            all_unique_cvars[cvar_name] = {**entry, "binary": bin_name}

        cvars_by_bin[bin_name] = sorted(found_cvars, key=lambda x: x["name"])
        print(f"  {bin_name:24s}: {len(found_cvars):4d} CVars extracted")

    return {"by_binary": cvars_by_bin, "total_unique": len(all_unique_cvars)}


def audit_x87_vs_sse():
    print("=== [3] Auditing x87 FPU vs SSE/SSE2 Instruction Ratios ===")
    bins = [
        "geometry.dll",
        "scene.dll",
        "render.dll",
        "pf.dll",
        "Majesty2.exe",
        "scene/s_grass.dll",
        "scene/s_landscape.dll",
        "scene/s_skin.dll",
        "scene/s_water.dll",
        "scene/s_physics.dll",
        "render/r_d3d9.dll",
    ]
    x87_prefixes = (
        "fld", "fst", "fadd", "fsub", "fmul", "fdiv", "fchs", "fabs",
        "fsqrt", "fsin", "fcos", "fsincos", "fpatan", "fptan", "fcom",
        "fucom", "fnstsw", "fild", "fist", "fxch", "fscale", "fprem"
    )
    sse_prefixes = (
        "addss", "subss", "mulss", "divss", "sqrtss", "rcpss", "rsqrtss", "maxss", "minss",
        "addps", "subps", "mulps", "divps", "sqrtps", "rcpps", "rsqrtps", "maxps", "minps",
        "movss", "movaps", "movups", "movhlps", "movlhps", "shufps", "unpcklps", "unpckhps",
        "comiss", "ucomiss", "cmpps", "cmpss", "cvtsi2ss", "cvtss2si", "cvttps2dq", "cvtdq2ps",
        "addsd", "subsd", "mulsd", "divsd", "movsd", "movapd", "movupd", "cvtsd2ss", "cvtss2sd",
        "xorps", "andps", "orps", "dpps"
    )

    stats = {}
    for b in bins:
        path = os.path.join(GAME_DIR, b)
        if not os.path.exists(path):
            continue
        pe = pefile.PE(path, fast_load=True)
        base = pe.OPTIONAL_HEADER.ImageBase
        text_sec = next(s for s in pe.sections if b".text" in s.Name)
        data = text_sec.get_data()
        va = base + text_sec.VirtualAddress

        x87_counts = Counter()
        sse_counts = Counter()
        total_insns = 0

        md_fast = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for insn in md_fast.disasm(data, va):
            total_insns += 1
            m = insn.mnemonic
            if m.startswith(x87_prefixes):
                x87_counts[m] += 1
            elif m.startswith(sse_prefixes):
                sse_counts[m] += 1

        tot_x87 = sum(x87_counts.values())
        tot_sse = sum(sse_counts.values())
        ratio = tot_x87 / max(1, tot_sse)
        stats[b] = {
            "total_instructions": total_insns,
            "x87_total": tot_x87,
            "sse_total": tot_sse,
            "x87_to_sse_ratio": round(ratio, 2),
            "top_x87": dict(x87_counts.most_common(12)),
            "top_sse": dict(sse_counts.most_common(10)),
        }
        print(f"  {b:22s}: x87={tot_x87:6d}, SSE={tot_sse:5d}, Ratio={ratio:6.1f}x (Total insns={total_insns})")

    # Now inspect geometry.dll exported functions to find the prime SSE2/ARM64 hook candidates
    pe_geom = pefile.PE(os.path.join(GAME_DIR, "geometry.dll"), fast_load=True)
    pe_geom.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    geom_base = pe_geom.OPTIONAL_HEADER.ImageBase

    with open(SDK_JSON, "r") as f:
        sdk_data = json.load(f)
    geom_exports = sdk_data["binaries"]["geometry.dll"]["exports"]

    # Sort exports by RVA to estimate function sizes
    sorted_exps = sorted(geom_exports, key=lambda x: int(x["rva"], 16))
    hot_functions = []
    for i, exp in enumerate(sorted_exps):
        rva = int(exp["rva"], 16)
        next_rva = int(sorted_exps[i + 1]["rva"], 16) if i + 1 < len(sorted_exps) else rva + 0x100
        size = min(max(16, next_rva - rva), 0x200)
        func_va = geom_base + rva
        code = pe_geom.get_data(rva, size)
        insns = []
        x87_c = 0
        for ins in md.disasm(code, func_va):
            insns.append(f"0x{ins.address:08X}: {ins.mnemonic:8s} {ins.op_str}")
            if ins.mnemonic.startswith(x87_prefixes):
                x87_c += 1
            if ins.mnemonic in ("ret", "retn"):
                break
        mangled = exp.get("mangled", "")
        if x87_c >= 6 or any(k in mangled for k in ("Matrix34", "Matrix33", "Quat", "Vec3", "BoundBox")):
            hot_functions.append({
                "ordinal": exp["ordinal"],
                "rva": exp["rva"],
                "va": f"0x{func_va:08X}",
                "mangled": mangled,
                "demangled": exp["demangled"],
                "x87_count": x87_c,
                "disasm": insns[:35],
            })

    hot_functions.sort(key=lambda x: x["x87_count"], reverse=True)
    stats["geometry_hot_functions"] = hot_functions[:30]
    return stats


def inspect_configs():
    print("=== [4] Inspecting Performance-Critical Config Files in resource.pak ===")
    pak_path = os.path.join(GAME_DIR, "resource/resource.pak")
    targets = [
        "entity/developer.con",
        "set/level_of_detail.set",
        "set/grass.set",
        "entity/m2GameConst.con",
        "set/water.set",
        "set/environment.set",
    ]
    extracted = {}
    with zipfile.ZipFile(pak_path, "r") as zf:
        namelist = zf.namelist()
        for t in targets:
            matches = [n for n in namelist if n.lower().endswith(t.lower()) or os.path.basename(n).lower() == os.path.basename(t).lower()]
            for m in matches:
                raw = zf.read(m)
                extracted[m] = raw.decode("cp1251", errors="replace")
                print(f"  Found {m} ({len(raw)} bytes)")
    return extracted


def main():
    t4 = analyze_task4()
    cvars = extract_cvars()
    fpu_stats = audit_x87_vs_sse()
    configs = inspect_configs()

    with open(os.path.join(WORK_DIR, "sdk/engine_cvars.json"), "w", encoding="utf-8") as f:
        json.dump(cvars, f, indent=2, ensure_ascii=False)

    report = {
        "task4_disasm": t4,
        "fpu_vs_sse": fpu_stats,
        "configs": configs,
    }
    with open(os.path.join(WORK_DIR, "sdk/performance_and_task4_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)

    print("Saved sdk/engine_cvars.json and sdk/performance_and_task4_report.json")


if __name__ == "__main__":
    main()
