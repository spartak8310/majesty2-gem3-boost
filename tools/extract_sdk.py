#!/usr/bin/env python3
"""
GEM 3 Engine (Majesty 2) Full C++ SDK & RTTI/Vtable Extractor
=============================================================
Parses all PE binaries (.exe, .dll, .asi, .flt) in Majesty 2 Collection:
  1. Export Tables (with full recursive-descent MSVC C++ demangler)
  2. Import Tables (dependency graph & imported symbols per module)
  3. MSVC RTTI TypeDescriptors (.?AV / .?AU), ClassHierarchyDescriptors (inheritance),
     CompleteObjectLocators, and VTables (virtual method addresses & counts)
  4. Generates:
     - sdk/exports_by_dll.json
     - sdk/GEM3_ENGINE_API.md
     - sdk/include/gem3_plugin_sdk.h
"""

import json
import os
import re
import struct
from collections import defaultdict
import pefile


# ============================================================================
# Recursive-Descent MSVC C++ Mangled Name Demangler
# ============================================================================

SPECIAL_NAMES = {
    "0": "{ctor}",
    "1": "{dtor}",
    "2": "operator new",
    "3": "operator delete",
    "4": "operator=",
    "5": "operator>>",
    "6": "operator<<",
    "7": "operator!",
    "8": "operator==",
    "9": "operator!=",
    "A": "operator[]",
    "B": "operator {cast}",
    "C": "operator->",
    "D": "operator*",
    "E": "operator++",
    "F": "operator--",
    "G": "operator-",
    "H": "operator+",
    "I": "operator&",
    "J": "operator->*",
    "K": "operator/",
    "L": "operator%",
    "M": "operator<",
    "N": "operator<=",
    "O": "operator>",
    "P": "operator>=",
    "Q": "operator,",
    "R": "operator()",
    "S": "operator~",
    "T": "operator^",
    "U": "operator|",
    "V": "operator&&",
    "W": "operator||",
    "X": "operator*=",
    "Y": "operator+=",
    "Z": "operator-=",
    "_0": "operator/=",
    "_1": "operator%=",
    "_2": "operator>>=",
    "_3": "operator<<=",
    "_4": "operator&=",
    "_5": "operator|=",
    "_6": "operator^=",
    "_7": "`vftable'",
    "_8": "`vbtable'",
    "_9": "`vcall'",
    "_A": "`typeof'",
    "_B": "`local static guard'",
    "_D": "`vbase destructor'",
    "_E": "`vector deleting destructor'",
    "_F": "`default constructor closure'",
    "_G": "`scalar deleting destructor'",
    "_H": "`vector constructor iterator'",
    "_I": "`vector destructor iterator'",
    "_J": "`vector vbase constructor iterator'",
    "_K": "`virtual displacement map'",
    "_L": "`eh vector constructor iterator'",
    "_M": "`eh vector destructor iterator'",
    "_N": "`eh vector vbase constructor iterator'",
    "_O": "`copy constructor closure'",
    "_S": "`local vftable'",
    "_T": "`local vftable constructor closure'",
    "_U": "operator new[]",
    "_V": "operator delete[]",
}

PRIMITIVE_TYPES = {
    "X": "void",
    "D": "char",
    "C": "signed char",
    "E": "unsigned char",
    "F": "short",
    "G": "unsigned short",
    "H": "int",
    "I": "unsigned int",
    "J": "long",
    "K": "unsigned long",
    "M": "float",
    "N": "double",
    "O": "long double",
    "Z": "...",
    "_N": "bool",
    "_J": "__int64",
    "_K": "unsigned __int64",
    "_W": "wchar_t",
}

CALL_CONVS = {
    "A": "__cdecl",
    "B": "__cdecl",
    "C": "__pascal",
    "D": "__pascal",
    "E": "__thiscall",
    "F": "__thiscall",
    "G": "__stdcall",
    "H": "__stdcall",
    "I": "__fastcall",
    "J": "__fastcall",
    "K": "",
}


class MSVCDemangler:
    def __init__(self, s: str):
        self.s = s
        self.pos = 0
        self.name_backrefs = []
        self.type_backrefs = []

    def peek(self, n=1):
        return self.s[self.pos : self.pos + n]

    def get(self, n=1):
        res = self.s[self.pos : self.pos + n]
        self.pos += n
        return res

    def match(self, prefix: str) -> bool:
        if self.s.startswith(prefix, self.pos):
            self.pos += len(prefix)
            return True
        return False

    def read_until(self, ch="@"):
        idx = self.s.find(ch, self.pos)
        if idx == -1:
            res = self.s[self.pos :]
            self.pos = len(self.s)
            return res
        res = self.s[self.pos : idx]
        self.pos = idx + len(ch)
        return res

    def read_number(self) -> int:
        neg = self.match("?")
        if self.peek() and self.peek() in "0123456789":
            val = int(self.get()) + 1
            return -val if neg else val
        val = 0
        while self.pos < len(self.s) and self.s[self.pos] != "@":
            c = self.get()
            if "A" <= c <= "P":
                val = (val << 4) + (ord(c) - ord("A"))
            else:
                break
        self.match("@")
        return -val if neg else val

    def read_template_name(self) -> str:
        # Starts after '?$'
        saved_names = list(self.name_backrefs)
        saved_types = list(self.type_backrefs)
        self.name_backrefs = []
        self.type_backrefs = []

        tname = self.read_until("@")
        if tname:
            self.name_backrefs.append(tname)
        args = []
        while self.pos < len(self.s) and not self.match("@"):
            if self.match("$0"):
                args.append(str(self.read_number()))
            elif self.match("$1"):
                args.append("&" + self.read_qualified_name())
            elif self.match("$"):
                self.get()
                args.append(str(self.read_number()))
            else:
                t = self.read_type(record_backref=True)
                args.append(t)
        self.name_backrefs = saved_names
        self.type_backrefs = saved_types
        res = f"{tname}<{', '.join(args)}>"
        # Clean nested template spacing and std::string
        res = res.replace(
            "basic_string<char, std::char_traits<char>, std::allocator<char>>",
            "string",
        )
        return res

    def read_unqualified_name(self, is_first=False) -> str:
        if self.match("?$"):
            res = self.read_template_name()
            if len(self.name_backrefs) < 10:
                self.name_backrefs.append(res)
            return res
        if self.match("?"):
            code = self.get()
            if code == "_":
                code += self.get()
            if code == "$" and self.pos < len(self.s):
                return self.read_template_name()
            return SPECIAL_NAMES.get(code, f"op_{code}")
        c = self.peek()
        if c and c in "0123456789":
            idx = int(self.get())
            if idx < len(self.name_backrefs):
                return self.name_backrefs[idx]
            return f"#{idx}"
        frag = self.read_until("@")
        if frag and len(self.name_backrefs) < 10:
            self.name_backrefs.append(frag)
        return frag

    def read_scope_list(self) -> list:
        scopes = []
        while self.pos < len(self.s) and not self.match("@"):
            if self.pos >= len(self.s):
                break
            part = self.read_unqualified_name()
            if part:
                scopes.append(part)
        scopes.reverse()
        return scopes

    def read_qualified_name(self) -> str:
        name = self.read_unqualified_name(is_first=True)
        scopes = self.read_scope_list()
        if name == "{ctor}" and scopes:
            name = scopes[-1].split("<")[0]
        elif name == "{dtor}" and scopes:
            name = "~" + scopes[-1].split("<")[0]
        if scopes:
            return "::".join(scopes + [name])
        return name

    def read_cv(self) -> str:
        if self.pos >= len(self.s):
            return ""
        c = self.get()
        if c in ("B", "D", "F", "J"):
            return "const"
        if c in ("C", "D", "G", "K"):
            return "volatile"
        return ""

    def read_type(self, record_backref=True) -> str:
        if self.pos >= len(self.s):
            return "void"
        c = self.peek()
        if c in "0123456789":
            idx = int(self.get())
            if idx < len(self.type_backrefs):
                return self.type_backrefs[idx]
            return f"T{idx}"

        if self.match("_"):
            c2 = "_" + self.get()
            return PRIMITIVE_TYPES.get(c2, "unknown")

        c = self.get()
        if c in PRIMITIVE_TYPES:
            return PRIMITIVE_TYPES[c]

        res = "unknown"
        if c in ("P", "Q", "R", "S"):  # Pointer
            cv_ptr = " const" if c in ("Q", "S") else ""
            if self.match("6"):  # Function pointer
                cc = CALL_CONVS.get(self.get(), "")
                ret = self.read_type(record_backref=True)
                params = self.read_param_list()
                self.match("Z")
                res = f"{ret} ({cc} *)({', '.join(params)})"
            elif self.match("8"):  # Member function pointer
                cls_scopes = self.read_scope_list()
                cls_name = "::".join(cls_scopes)
                cv_mem = self.read_cv()
                cc = CALL_CONVS.get(self.get(), "")
                ret = self.read_type(record_backref=True)
                params = self.read_param_list()
                self.match("Z")
                res = f"{ret} ({cc} {cls_name}::*)({', '.join(params)}) {cv_mem}".strip()
            else:
                cv_target = self.read_cv()
                inner = self.read_type(record_backref=False)
                res = f"{ cv_target + ' ' if cv_target else '' }{inner}*{cv_ptr}"
        elif c in ("A", "B"):  # Reference
            cv_target = self.read_cv()
            inner = self.read_type(record_backref=False)
            res = f"{ cv_target + ' ' if cv_target else '' }{inner}&"
        elif c in ("U", "V", "T"):  # struct, class, union
            scopes = self.read_scope_list()
            res = "::".join(scopes)
        elif c == "W":  # enum
            self.get()  # underlying type code (usually '4')
            scopes = self.read_scope_list()
            res = "enum " + "::".join(scopes)
        elif c == "?":  # Modified type
            cv = self.read_cv()
            inner = self.read_type(record_backref=False)
            res = f"{cv} {inner}".strip()
        elif c == "$":
            if self.match("$C"):
                cv = self.read_cv()
                inner = self.read_type(record_backref=False)
                res = f"{cv} {inner}".strip()
            elif self.match("$V"):
                res = ""

        if record_backref and len(self.type_backrefs) < 10 and res not in PRIMITIVE_TYPES.values():
            self.type_backrefs.append(res)
        return res

    def read_param_list(self) -> list:
        params = []
        if self.match("X"):
            return []
        while self.pos < len(self.s) and not self.match("@"):
            if self.peek() == "Z":
                break
            p = self.read_type(record_backref=True)
            params.append(p)
        return params


def demangle_msvc(mangled: str) -> dict:
    """
    Demangles an MSVC symbol or RTTI TypeDescriptor name and extracts structured metadata.
    """
    if not mangled:
        return {"mangled": "", "demangled": "", "kind": "unknown", "namespace": "", "class": "", "member": ""}

    # RTTI Type Descriptor (.?AV...@@ or .?AU...@@)
    if mangled.startswith(".?A"):
        kind_char = mangled[3:4]
        tag = "class" if kind_char == "V" else ("struct" if kind_char == "U" else "type")
        d = MSVCDemangler(mangled[4:])
        scopes = d.read_scope_list()
        full_name = "::".join(scopes)
        ns = "::".join(scopes[:-1]) if len(scopes) > 1 else ""
        cls = scopes[-1] if scopes else full_name
        return {
            "mangled": mangled,
            "demangled": f"{tag} {full_name}",
            "kind": f"rtti_{tag}",
            "namespace": ns,
            "class": cls,
            "member": "",
            "full_type": full_name,
        }

    if not mangled.startswith("?"):
        return {
            "mangled": mangled,
            "demangled": mangled,
            "kind": "c_export",
            "namespace": "global",
            "class": "",
            "member": mangled,
        }

    try:
        d = MSVCDemangler(mangled[1:])
        raw_first = d.read_unqualified_name(is_first=True)
        scopes = d.read_scope_list()

        member_name = raw_first
        if raw_first == "{ctor}" and scopes:
            member_name = scopes[-1].split("<")[0]
        elif raw_first == "{dtor}" and scopes:
            member_name = "~" + scopes[-1].split("<")[0]

        qual_name = "::".join(scopes + [member_name]) if scopes else member_name

        # Determine namespace & class
        ns = ""
        cls = ""
        if len(scopes) >= 2:
            ns = scopes[0]
            cls = "::".join(scopes[1:])
        elif len(scopes) == 1:
            # Heuristic: in GEM 3, namespaces start with lowercase 'x' (xIo, xScene, xRender, xUi, xSet, xScript, etc.)
            # while classes start with 'e' or uppercase (eStream, eEntity, Matrix34, etc.)
            if scopes[0].startswith("x") and len(scopes[0]) > 1 and scopes[0][1].isupper():
                ns = scopes[0]
                cls = ""
            elif scopes[0] in ("Game", "FileSystem", "Localization", "std", "ags"):
                ns = scopes[0]
                cls = ""
            else:
                ns = "global"
                cls = scopes[0]
        else:
            ns = "global"

        if d.pos >= len(d.s):
            return {
                "mangled": mangled,
                "demangled": qual_name,
                "kind": "symbol",
                "namespace": ns,
                "class": cls,
                "member": member_name,
            }

        access_code = d.get()
        # Data / vtable symbols
        if access_code in ("0", "1", "2", "3", "4"):
            t = d.read_type(record_backref=False)
            cv = d.read_cv()
            return {
                "mangled": mangled,
                "demangled": f"{cv + ' ' if cv else ''}{t} {qual_name}",
                "kind": "data",
                "namespace": ns,
                "class": cls,
                "member": member_name,
                "return_type": t,
            }
        if access_code in ("6", "7"):
            return {
                "mangled": mangled,
                "demangled": f"const {qual_name}",
                "kind": "vtable",
                "namespace": ns,
                "class": cls,
                "member": member_name,
            }

        # Function / Method symbols
        access_map = {
            "A": "private:",
            "B": "private:",
            "C": "private: static",
            "D": "private: static",
            "E": "private: virtual",
            "F": "private: virtual",
            "I": "protected:",
            "J": "protected:",
            "K": "protected: static",
            "L": "protected: static",
            "M": "protected: virtual",
            "N": "protected: virtual",
            "Q": "public:",
            "R": "public:",
            "S": "public: static",
            "T": "public: static",
            "U": "public: virtual",
            "V": "public: virtual",
            "Y": "",
            "Z": "",
        }
        access_str = access_map.get(access_code, "")
        is_member = access_code not in ("C", "D", "K", "L", "S", "T", "Y", "Z")
        this_cv = d.read_cv() if is_member else ""
        cc = CALL_CONVS.get(d.get(), "")

        has_ret = raw_first not in ("{ctor}", "{dtor}")
        if raw_first == "operator {cast}":
            ret_type = d.read_type(record_backref=True)
            member_name = f"operator {ret_type}"
            qual_name = "::".join(scopes + [member_name]) if scopes else member_name
            ret_str = ""
        elif has_ret:
            if d.match("@"):
                ret_type = "void"
            else:
                ret_type = d.read_type(record_backref=True)
            ret_str = ret_type + " "
        else:
            ret_type = ""
            ret_str = ""

        params = d.read_param_list()
        param_str = ", ".join(params)
        prefix = f"{access_str} " if access_str else ""
        suffix = f" {this_cv}" if this_cv else ""
        demangled = f"{prefix}{ret_str}{cc} {qual_name}({param_str}){suffix}".strip()
        demangled = re.sub(r"\s+", " ", demangled)

        kind = "method" if cls else "function"
        if raw_first == "{ctor}":
            kind = "constructor"
        elif raw_first == "{dtor}":
            kind = "destructor"
        elif "virtual" in access_str:
            kind = "virtual_method"

        return {
            "mangled": mangled,
            "demangled": demangled,
            "kind": kind,
            "access": access_str,
            "calling_convention": cc,
            "return_type": ret_type,
            "params": params,
            "this_cv": this_cv,
            "namespace": ns,
            "class": cls,
            "member": member_name,
        }
    except Exception:
        return {
            "mangled": mangled,
            "demangled": mangled,
            "kind": "unparsed",
            "namespace": "global",
            "class": "",
            "member": mangled,
        }


# ============================================================================
# RTTI & VTable Scanner for 32-bit MSVC PE Binaries
# ============================================================================

def extract_rtti_and_vtables(pe: pefile.PE, data: bytes) -> dict:
    base = pe.OPTIONAL_HEADER.ImageBase
    img_len = len(data)

    # Find text section range to validate virtual function pointers
    code_ranges = []
    for sec in pe.sections:
        if sec.Characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            s_start = base + sec.VirtualAddress
            s_end = s_start + max(sec.Misc_VirtualSize, sec.SizeOfRawData)
            code_ranges.append((s_start, s_end))

    def is_code_va(va: int) -> bool:
        return any(lo <= va < hi for lo, hi in code_ranges)

    # 1. Locate all TypeDescriptors (.?AV...@@, .?AU...@@)
    # In 32-bit MSVC, TypeDescriptor layout:
    #   +0x00: pVFTable (points to type_info::`vftable')
    #   +0x04: spare (0)
    #   +0x08: char name[] (".?AV...")
    type_descriptors = {}  # td_va -> demangled info
    for m in re.finditer(rb"\.\?A[VUW][A-Za-z0-9_@?$]+@@\x00", data):
        name_rva = m.start()
        if name_rva < 8:
            continue
        td_rva = name_rva - 8
        td_va = base + td_rva
        raw_name = m.group(0)[:-1].decode("ascii", errors="ignore")
        info = demangle_msvc(raw_name)
        type_descriptors[td_va] = info

    # 2. Search for RTTICompleteObjectLocator (COL) referencing each TypeDescriptor
    # COL layout (20 bytes):
    #   +0x00: signature (0)
    #   +0x04: offset (vtable offset in object)
    #   +0x08: cdOffset
    #   +0x0C: pTypeDescriptor (td_va)
    #   +0x10: pClassDescriptor (chd_va)
    classes = {}
    for td_va, td_info in type_descriptors.items():
        cls_key = td_info.get("full_type", td_info["demangled"])
        classes[cls_key] = {
            "rtti_mangled": td_info["mangled"],
            "demangled": td_info["demangled"],
            "namespace": td_info["namespace"],
            "class": td_info["class"],
            "td_va": hex(td_va),
            "base_classes": [],
            "vtables": [],
        }

    # Build O(N) index of all 4-byte aligned DWORDs in non-executable sections
    dword_positions = defaultdict(list)
    for sec in pe.sections:
        if not (sec.Characteristics & 0x20000000):  # Non-code sections (.rdata, .data)
            s_rva = sec.VirtualAddress
            s_size = min(max(sec.Misc_VirtualSize, sec.SizeOfRawData), img_len - s_rva)
            if s_size >= 4:
                count = s_size // 4
                vals = struct.unpack_from(f"<{count}I", data, s_rva)
                for i, v in enumerate(vals):
                    if base <= v < base + img_len:
                        dword_positions[v].append(s_rva + i * 4)

    for td_va, td_info in type_descriptors.items():
        cls_key = td_info.get("full_type", td_info["demangled"])
        for idx in dword_positions.get(td_va, ()):
            if idx < 12 or idx + 8 > img_len:
                continue
            col_rva = idx - 12
            sig, v_offset, cd_offset, p_td, p_chd = struct.unpack_from("<5I", data, col_rva)
            if sig != 0:
                continue
            chd_rva = p_chd - base
            if not (0 < chd_rva < img_len - 16):
                continue
            chd_sig, chd_attr, num_bases, p_base_array = struct.unpack_from("<4I", data, chd_rva)
            if chd_sig != 0 or num_bases > 64:
                continue

            # Parse base classes
            base_array_rva = p_base_array - base
            if 0 < base_array_rva < img_len - num_bases * 4 and not classes[cls_key]["base_classes"]:
                bases = []
                for b_idx in range(1, num_bases):  # index 0 is self
                    bcd_va = struct.unpack_from("<I", data, base_array_rva + b_idx * 4)[0]
                    bcd_rva = bcd_va - base
                    if 0 < bcd_rva < img_len - 24:
                        b_td_va = struct.unpack_from("<I", data, bcd_rva)[0]
                        if b_td_va in type_descriptors:
                            b_name = type_descriptors[b_td_va].get("full_type", type_descriptors[b_td_va]["demangled"])
                            if b_name not in bases and b_name != cls_key:
                                bases.append(b_name)
                classes[cls_key]["base_classes"] = bases

            # Find vtables pointing to col_va ([vtable_rva - 4] == col_va)
            col_va = base + col_rva
            for v_idx in dword_positions.get(col_va, ()):
                vt_rva = v_idx + 4
                if vt_rva + 4 > img_len:
                    continue
                first_fn = struct.unpack_from("<I", data, vt_rva)[0]
                if not is_code_va(first_fn):
                    continue
                methods = []
                cur = vt_rva
                while cur + 4 <= img_len and len(methods) < 256:
                    fn_va = struct.unpack_from("<I", data, cur)[0]
                    if is_code_va(fn_va):
                        methods.append(hex(fn_va))
                        cur += 4
                    else:
                        break
                classes[cls_key]["vtables"].append({
                    "vtable_va": hex(base + vt_rva),
                    "vtable_rva": hex(vt_rva),
                    "offset": v_offset,
                    "method_count": len(methods),
                    "methods": methods[:32],
                })

    return classes


def analyze_binary(filepath: str) -> dict:
    filename = os.path.basename(filepath)
    pe = pefile.PE(filepath, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    is_dotnet = False
    if len(pe.OPTIONAL_HEADER.DATA_DIRECTORY) > 14:
        clr_dir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[14]
        if clr_dir.VirtualAddress != 0 and clr_dir.Size != 0:
            is_dotnet = True

    exports = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            mangled = exp.name.decode("ascii", errors="ignore") if exp.name else f"ordinal_{exp.ordinal}"
            info = demangle_msvc(mangled)
            info["ordinal"] = exp.ordinal
            info["rva"] = hex(exp.address)
            info["va"] = hex(base + exp.address)
            exports.append(info)

    imports = defaultdict(list)
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for entry in pe.DIRECTORY_ENTRY_IMPORT:
            dll_name = entry.dll.decode("ascii", errors="ignore")
            for imp in entry.imports:
                mangled = imp.name.decode("ascii", errors="ignore") if imp.name else f"ordinal_{imp.ordinal}"
                info = demangle_msvc(mangled)
                info["iat_va"] = hex(imp.address) if imp.address else None
                imports[dll_name].append(info)

    rtti_classes = {}
    if not is_dotnet:
        try:
            mapped = pe.get_memory_mapped_image()
            rtti_classes = extract_rtti_and_vtables(pe, mapped)
        except Exception:
            pass

    return {
        "binary": filename,
        "size": os.path.getsize(filepath),
        "image_base": hex(base),
        "entry_point_rva": hex(pe.OPTIONAL_HEADER.AddressOfEntryPoint),
        "is_dotnet": is_dotnet,
        "export_count": len(exports),
        "imported_dll_count": len(imports),
        "imported_symbol_count": sum(len(v) for v in imports.values()),
        "rtti_class_count": len(rtti_classes),
        "exports": exports,
        "imports": dict(imports),
        "rtti_classes": rtti_classes,
    }


def main():
    game_dir = "$HOME/Documents/Majesty2_Archive/Majesty 2 Collection"
    sdk_dir = "./scratch/majesty2-re/sdk"
    os.makedirs(os.path.join(sdk_dir, "include"), exist_ok=True)

    # All 37 GEM 3 + game/middleware PE binaries (plus .asi/.flt if present)
    all_files = sorted(
        f for f in os.listdir(game_dir)
        if f.lower().endswith((".dll", ".exe", ".asi", ".flt"))
    )

    all_results = {}
    namespace_index = defaultdict(lambda: defaultdict(list))
    total_exports = 0
    total_imports = 0
    total_rtti = 0

    for fname in all_files:
        fpath = os.path.join(game_dir, fname)
        res = analyze_binary(fpath)
        all_results[fname] = res
        total_exports += res["export_count"]
        total_imports += res["imported_symbol_count"]
        total_rtti += res["rtti_class_count"]
        print(
            f"[+] {fname:26s} | Base={res['image_base']} | "
            f"Exports={res['export_count']:4d} | Imports={res['imported_symbol_count']:4d} | "
            f"RTTI={res['rtti_class_count']:4d}"
        )
        for exp in res["exports"]:
            ns = exp.get("namespace") or "global"
            cls = exp.get("class") or "(free functions)"
            namespace_index[ns][cls].append({
                "dll": fname,
                "rva": exp["rva"],
                "va": exp["va"],
                "mangled": exp["mangled"],
                "demangled": exp["demangled"],
                "kind": exp["kind"],
            })

    print(
        f"\n[=] TOTAL across {len(all_files)} binaries: "
        f"{total_exports} Exports | {total_imports} Imports | {total_rtti} RTTI Classes/VTables"
    )

    # Save sdk/exports_by_dll.json
    json_path = os.path.join(sdk_dir, "exports_by_dll.json")
    with open(json_path, "w", encoding="utf-8") as fp:
        json.dump(
            {
                "summary": {
                    "binary_count": len(all_files),
                    "total_exports": total_exports,
                    "total_imports": total_imports,
                    "total_rtti_classes": total_rtti,
                },
                "binaries": all_results,
            },
            fp,
            indent=2,
        )
    print(f"[+] Wrote {json_path}")

    # Generate sdk/GEM3_ENGINE_API.md
    md_path = os.path.join(sdk_dir, "GEM3_ENGINE_API.md")
    with open(md_path, "w", encoding="utf-8") as md:
        md.write("# GEM 3 Engine (Majesty 2) Complete C++ SDK & RTTI Reference\n\n")
        md.write(
            f"- **Binaries Analyzed**: {len(all_files)} PE modules (`.exe`, `.dll`, `.asi`, `.flt`)\n"
            f"- **Total Exported C++ Symbols**: `{total_exports}`\n"
            f"- **Total Imported Symbols**: `{total_imports}`\n"
            f"- **Total Reconstructed RTTI Classes & VTables**: `{total_rtti}`\n\n"
        )

        md.write("## 1. Module Overview Table\n\n")
        md.write("| Binary | Size (B) | ImageBase | Exports | Imports | RTTI Classes | Role |\n")
        md.write("|---|---:|---|---:|---:|---:|---|\n")
        role_hints = {
            "Majesty2.exe": "Main game executable (`Game::`, `Interface::`, `FileSystem::Crypt::`, AI, Logic)",
            "M2Editor.exe": "Map & Mission Editor (`xEditor::`, terrain/quest/trigger tools)",
            "scene.dll": "3D Scene graph, Entities (`xEntity`, `xScene`, `xActor`, `xObstacle`, `xLand`)",
            "render.dll": "Direct3D9 Renderer (`xRender`, shaders, shadows, grass, post-processing)",
            "ui.dll": "UI Widget Framework (`xUi`, windows, controls, layouts)",
            "features.dll": "Engine Feature Modules (cinematics, weather, camera controllers)",
            "app.dll": "Application Lifecycle (`xApp`, windowing, input, console loop)",
            "base.dll": "Core Primitives (`xCrypt::eMD5`, `xSet` variables, memory, strings, math)",
            "gembase.dll": "GEM3 Shared Types (`xMod`, localization, profiles)",
            "io.dll": "Virtual File System (`xIo`, `xRepository`, `xSdl`, `.pak`/ZIP streams, async cache)",
            "net.dll": "Low-level Networking (`xNet`, packet streams, sockets)",
            "Multiplayer.dll": "GameSpy/LAN Lobby & Session Manager (`Game::Multiplayer`)",
            "script.dll": "Script Engine (`xScript`, state machines, conditions, actions)",
            "pf.dll": "Particle & Pathfinding / Flow subsystem (`xPf`)",
            "ps.dll": "Particle System emitters (`xPs`)",
            "geometry.dll": "3D Math & Collision Primitives (`Matrix33`, `Matrix34`, `Vec3`, `Quat`, `BoundBox`)",
            "dda.dll": "Data-Driven Attributes & Serialization (`xDda`, `eIntrusiveRegistrator`)",
            "traceable.dll": "Raycasting & Spatial Tracing (`xTrace`)",
            "audio.dll": "Miles Sound System Wrapper (`xAudio`, `xSound`)",
            "video.dll": "Ogg Theora Video Playback (`xVideo`)",
            "advanced.dll": "CRC32 (`eCRC`), compression & utility helpers",
            "editor.dll": "Editor Core Dock/Panel Framework (`xEditor`)",
        }
        for fname, res in all_results.items():
            role = role_hints.get(fname, "Plugin / Middleware / Helper")
            md.write(
                f"| `{fname}` | {res['size']:,} | `{res['image_base']}` | "
                f"{res['export_count']} | {res['imported_symbol_count']} | "
                f"{res['rtti_class_count']} | {role} |\n"
            )

        md.write("\n## 2. Engine Namespaces & Exported C++ API\n\n")
        priority_ns = [
            "xIo", "xSet", "xConsole", "xDda", "xScript", "xScene", "xEntity", "xActor",
            "xRender", "xUi", "xNet", "Game", "xApp", "xAudio", "xVideo", "xMod",
            "xCrypt", "xSdl", "xEditor", "global"
        ]
        all_ns = sorted(namespace_index.keys(), key=lambda k: (priority_ns.index(k) if k in priority_ns else 999, k))

        for ns in all_ns:
            classes_dict = namespace_index[ns]
            total_in_ns = sum(len(v) for v in classes_dict.values())
            md.write(f"### Namespace `{ns}` ({total_in_ns} exports)\n\n")
            for cls_name, syms in sorted(classes_dict.items()):
                md.write(f"#### `{ns}::{cls_name}` ({len(syms)} symbols)\n\n")
                md.write("```cpp\n")
                for s in syms[:60]:
                    md.write(f"// [{s['dll']} @ RVA {s['rva']} | {s['mangled']}]\n")
                    md.write(f"{s['demangled']};\n")
                if len(syms) > 60:
                    md.write(f"// ... (+{len(syms) - 60} more symbols insdk/exports_by_dll.json)\n")
                md.write("```\n\n")

        md.write("## 3. Key Reconstructed RTTI Class Hierarchies & VTables\n\n")
        for bname in ["Majesty2.exe", "scene.dll", "render.dll", "io.dll", "ui.dll", "Multiplayer.dll", "geometry.dll", "pf.dll"]:
            if bname not in all_results:
                continue
            rtti = all_results[bname]["rtti_classes"]
            md.write(f"### `{bname}` ({len(rtti)} RTTI classes)\n\n")
            md.write("| Class / Struct | Base Classes | VTable VA | Virtual Methods |\n")
            md.write("|---|---|---|---:|\n")
            shown = 0
            for cname, cinfo in sorted(rtti.items()):
                if not cinfo["vtables"] and not cinfo["base_classes"]:
                    continue
                bases_str = ", ".join(f"`{b}`" for b in cinfo["base_classes"][:4])
                if len(cinfo["base_classes"]) > 4:
                    bases_str += f" (+{len(cinfo['base_classes'])-4})"
                vt_va = cinfo["vtables"][0]["vtable_va"] if cinfo["vtables"] else "-"
                vt_cnt = cinfo["vtables"][0]["method_count"] if cinfo["vtables"] else 0
                md.write(f"| `{cname}` | {bases_str} | `{vt_va}` | {vt_cnt} |\n")
                shown += 1
                if shown >= 80:
                    break
            md.write("\n")

    print(f"[+] Wrote {md_path}")


if __name__ == "__main__":
    main()
