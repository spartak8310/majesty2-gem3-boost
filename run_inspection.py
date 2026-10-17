import os
import sys
import glob
import json
from pathlib import Path

# Add SATRAP ECHO path
sys.path.insert(0, './SATRAP_ECHO')
try:
    import satrap_cm as cm
except ImportError:
    cm = None

report = {
    "step1_src_xbone": {},
    "step2_re_decompiled": {},
    "step3_geometry_dll": {}
}

# -------------------------------------------------------------
# 1. Inspect src/xbone_parallel.h and find definitions in src/
# -------------------------------------------------------------
src_dir = Path("src")
xbone_h = src_dir / "xbone_parallel.h"
if xbone_h.exists():
    content = xbone_h.read_text(errors="ignore")
    report["step1_src_xbone"]["xbone_parallel_h"] = {
        "exists": True,
        "line_count": len(content.splitlines()),
        "content_snippet": "\n".join(content.splitlines()[:50])
    }
else:
    # check if found elsewhere
    all_xbone_files = list(Path(".").glob("**/xbone*"))
    report["step1_src_xbone"]["xbone_parallel_h"] = {
        "exists": False,
        "related_files_found": [str(p) for p in all_xbone_files]
    }

src_matches = []
search_dirs = [src_dir, Path(".")] if not src_dir.exists() else [src_dir]
for sdir in search_dirs:
    if sdir.exists():
        for root, _, files in os.walk(sdir):
            for f in files:
                if f.endswith(('.h', '.cpp', '.c', '.hpp', '.inc', '.asm', '.md', '.txt')):
                    fpath = Path(root) / f
                    try:
                        lines = fpath.read_text(errors="ignore").splitlines()
                        for idx, line in enumerate(lines):
                            if "InstallQuantDistributorHook" in line or "xBone" in line or "xbone" in line.lower():
                                src_matches.append({
                                    "file": str(fpath),
                                    "line": idx + 1,
                                    "text": line.strip()
                                })
                    except Exception:
                        pass

report["step1_src_xbone"]["matches"] = src_matches[:50]

# -------------------------------------------------------------
# 2. Check re/ and decompiled/ for existing decompilation/analysis
# -------------------------------------------------------------
re_decompiled_summary = {}
targets = ["xbone", "features", "geometry", "scene", "pather"]

for folder_name in ["re", "decompiled"]:
    folder = Path(folder_name)
    if folder.exists():
        files_found = []
        for root, _, files in os.walk(folder):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), folder)
                files_found.append(rel)
        
        target_hits = []
        for rel in files_found:
            rel_low = rel.lower()
            for t in targets:
                if t in rel_low:
                    target_hits.append({
                        "file": str(folder / rel),
                        "target": t,
                        "size": (folder / rel).stat().st_size
                    })
                    break
        
        # also search text content if small files
        content_hits = []
        for rel in files_found:
            fpath = folder / rel
            if fpath.stat().st_size < 500000 and fpath.suffix in ('.txt', '.md', '.c', '.cpp', '.h', '.asm', '.json', '.yaml', '.py'):
                try:
                    txt = fpath.read_text(errors="ignore")
                    for t in targets + ["l1", "l2", "l3"]:
                        if t in txt.lower():
                            content_hits.append({"file": str(fpath), "keyword": t})
                            break
                except Exception:
                    pass

        re_decompiled_summary[folder_name] = {
            "exists": True,
            "total_files": len(files_found),
            "target_hits": target_hits,
            "content_hits": content_hits[:30],
            "file_sample": files_found[:30]
        }
    else:
        re_decompiled_summary[folder_name] = {"exists": False}

report["step2_re_decompiled"] = re_decompiled_summary

# -------------------------------------------------------------
# 3. Inspect geometry.dll via pefile + capstone
# -------------------------------------------------------------
dll_path = Path("/Applications/Gamez/Majesty 2.app/Contents/SharedSupport/prefix/drive_c/GOG Games/Majesty 2 Collection/geometry.dll")
if not dll_path.exists():
    candidates = list(Path("/Applications/Gamez").glob("**/geometry.dll"))
    if candidates:
        dll_path = candidates[0]

report["step3_geometry_dll"]["dll_path"] = str(dll_path)
report["step3_geometry_dll"]["exists"] = dll_path.exists()

if dll_path.exists():
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64

    pe = pefile.PE(str(dll_path))
    is_64 = (pe.FILE_HEADER.Machine == 0x8664)
    mode = CS_MODE_64 if is_64 else CS_MODE_32
    md = Cs(CS_ARCH_X86, mode)

    # Base relocations
    reloc_rvas = set()
    if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
        for base_reloc in pe.DIRECTORY_ENTRY_BASERELOC:
            for entry in getattr(base_reloc, 'entries', []):
                # type 0 is padding (IMAGE_REL_BASED_ABSOLUTE)
                if getattr(entry, 'type', 0) != 0 and getattr(entry, 'rva', None) is not None:
                    reloc_rvas.add(entry.rva)

    exports_list = []
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            name = exp.name.decode('utf-8', 'ignore') if exp.name else f"Ordinal_{exp.ordinal}"
            rva = exp.address
            is_forwarder = exp.forwarder is not None
            forwarder_str = exp.forwarder.decode('utf-8', 'ignore') if is_forwarder else None
            
            file_offset = pe.get_offset_from_rva(rva) if not is_forwarder else None
            
            # Check if first 6 bytes [rva, rva+6) overlap with relocations
            # A 32-bit relocation at R spans [R, R+4). It overlaps with [rva, rva+6) if max(rva, R) < min(rva+6, R+4)
            # i.e., R < rva + 6 and R + 4 > rva
            relocs_in_first_6 = [
                r for r in reloc_rvas
                if (r < rva + 6 and r + 4 > rva)
            ]
            first_6_free = (len(relocs_in_first_6) == 0)

            # Disassemble up to 256 bytes or ret
            x87_count = 0
            total_insns = 0
            x87_mnems = []
            
            if not is_forwarder and file_offset is not None:
                try:
                    code = pe.get_data(rva, 256)
                    for insn in md.disasm(code, rva):
                        total_insns += 1
                        mnem = insn.mnemonic.lower()
                        # x87 instructions: start with 'f' or 'wait'/'fwait'
                        if (mnem.startswith('f') and mnem not in ('far',)) or mnem in ('wait', 'fwait'):
                            x87_count += 1
                            x87_mnems.append(mnem)
                        if mnem in ('ret', 'retn', 'retf', 'iret'):
                            break
                        if total_insns >= 64:
                            break
                except Exception as e:
                    pass

            exports_list.append({
                "ordinal": exp.ordinal,
                "name": name,
                "rva": hex(rva),
                "file_offset": hex(file_offset) if file_offset is not None else None,
                "is_forwarder": is_forwarder,
                "forwarder": forwarder_str,
                "first_6_bytes_free_of_reloc": first_6_free,
                "relocs_in_first_6": [hex(r) for r in relocs_in_first_6],
                "inspected_insn_count": total_insns,
                "x87_instruction_count": x87_count,
                "x87_mnemonics": x87_mnems
            })

    report["step3_geometry_dll"]["machine"] = "x86_64" if is_64 else "x86_32"
    report["step3_geometry_dll"]["image_base"] = hex(pe.OPTIONAL_HEADER.ImageBase)
    report["step3_geometry_dll"]["total_relocs"] = len(reloc_rvas)
    report["step3_geometry_dll"]["total_exports"] = len(exports_list)
    report["step3_geometry_dll"]["exports"] = exports_list

print("=== RECEIPT_START ===")
print(json.dumps(report, indent=2))
print("=== RECEIPT_END ===")

if cm:
    cm.emit(report)
