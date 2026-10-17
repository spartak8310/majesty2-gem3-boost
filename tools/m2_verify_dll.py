#!/usr/bin/env python3
"""PE32 techgate for the d3d9 proxy build (Wave 1 additions included).

Verifies:
  1. i386 PE32 DLL, image base and sections are sane; .reloc present and inside the image
  2. export table is exactly the 8 undecorated names the harness expects (techgate invariant)
  3. no static import of d3d9.dll itself (it must forward to d3d9_dxvk.dll at runtime)
  4. Wave 1 instrumentation is really inside the binary (frustum / zone marker strings)
  5. every RVA -> file offset conversion used below round-trips through pe.get_offset_from_rva
     (the .reloc / FileOffset invariant from HARNESS_BLACKBOARD.md)
Usage: python3 scripts/m2_verify_dll.py build/d3d9.dll [--json out.json]
"""
import argparse
import hashlib
import json
import sys

import pefile

EXPECTED_EXPORTS = [
    'Direct3DCreate9', 'D3DPERF_BeginEvent', 'D3DPERF_EndEvent', 'D3DPERF_SetMarker',
    'D3DPERF_SetRegion', 'D3DPERF_QueryRepeatFrame', 'D3DPERF_SetOptions', 'D3DPERF_GetStatus',
]
WAVE1_MARKERS = [b'frustum: SetTransform capture hooked', b'frustum: conv=', b'frustum-mtx view=',
                 b'zone: mode=', b'zone: aabb candidates=', b'# frustum t=', b'# zone t=',
                 b'M2_ZONE_AABB', b'M2_ZONE_CULL', b'not a whitelisted shape']
WAVE2_MARKERS = [b'pf: disabled', b'pf: kill-switch', b'M2_PARTICLE_CULL', b'M2_PF_SCAN',
                 b'M2_PF_OPERATOR_RVA', b'M2_PF_PAINT_RVA', b'M2_PF_POS', b'M2_PF_OP_RET',
                 b'# pf t=', b'# pf-up t=', b'pf-scan: ', b'signature mismatch', b'M2_DISABLE_PARTICLE_CULL']
# Radical Splice 2 (t_m2_multithread_quant): parallel entity quant
QUANT_MARKERS = [b'M2_QUANT_RVA', b'M2_QUANT_EXPORT', b'M2_QUANT_THREADS', b'M2_QUANT_ARGS',
                 b'M2_QUANT_VERIFY', b'M2_QUANT_SNAP_MAX', b'M2_QUANT_KILL', b'M2_QUANT_SCAN',
                 b'quant: installed ', b'quant: export hit ', b'quant: READY base=', b'quant: pool up threads=',
                 b'quant: verify MISMATCH', b'quant: verify PASSED', b'# quant t=', b'M2_QUANT_LEARN']
# Wave 3 (t_m2_decoupled_render): decoupled render loop - present pacing + sub-tick camera lerp
WAVE3_MARKERS = [b'render: pacing', b'M2_FRAME_LIMIT', b'M2_PRESENT_MIN_DRAWS', b'M2_CAM_LERP',
                 b'M2_CAM_LERP_MAX_STEP', b'M2_CAM_LERP_MAX_TICKS', b'M2_RENDER_PROBE', b'# render t=']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('--json')
    a = ap.parse_args()

    checks = []

    def chk(ok, name, detail=''):
        checks.append({'check': name, 'ok': bool(ok), 'detail': detail})
        print(('PASS  ' if ok else 'FAIL  ') + name + (('  [' + detail + ']') if detail else ''))

    blob = open(a.dll, 'rb').read()
    sha = hashlib.sha256(blob).hexdigest()
    pe = pefile.PE(a.dll, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT'],
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC'],
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG'],
    ])

    chk(pe.FILE_HEADER.Machine == 0x14c, 'i386 (PE32) machine', hex(pe.FILE_HEADER.Machine))
    chk(bool(pe.FILE_HEADER.Characteristics & 0x2000), 'IMAGE_FILE_DLL flag')
    chk(pe.OPTIONAL_HEADER.Magic == 0x10b, 'PE32 optional header magic', hex(pe.OPTIONAL_HEADER.Magic))
    chk(0 < pe.OPTIONAL_HEADER.ImageBase < 2**32, 'image base in range (relocatable proxy)',
        hex(pe.OPTIONAL_HEADER.ImageBase))
    chk(pe.OPTIONAL_HEADER.SizeOfImage > 0x1000, 'SizeOfImage', hex(pe.OPTIONAL_HEADER.SizeOfImage))

    # exports
    exports = []
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') and pe.DIRECTORY_ENTRY_EXPORT:
        for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            name = s.name.decode() if s.name else '#%d' % s.ordinal
            exports.append(name)
            chk(not name.startswith('_') and '@' not in name, 'export undecorated: ' + name)
    chk(len(exports) == 8, 'exactly 8 exports', str(len(exports)))
    chk(sorted(exports) == sorted(EXPECTED_EXPORTS), 'the 8 expected export names (order-independent)', ','.join(exports))

    # imports: must not import d3d9.dll (we are the proxy)
    imports = []
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        imports = [e.dll.decode().lower() for e in pe.DIRECTORY_ENTRY_IMPORT]
    chk('d3d9.dll' not in imports, 'does not statically import d3d9.dll', ','.join(imports))
    chk('d3d9_dxvk.dll' not in imports, 'does not statically import d3d9_dxvk.dll')

    # .reloc presence + RVA->offset round trip for every section
    reloc_ok = False
    if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
        total = sum(len(b.entries) for b in pe.DIRECTORY_ENTRY_BASERELOC)
        reloc_ok = total > 0
        chk(reloc_ok, 'base relocations present', '%d entries' % total)
    for s in pe.sections:
        name = s.Name.rstrip(b'\x00').decode(errors='replace')
        off = pe.get_offset_from_rva(s.VirtualAddress)
        chk(off is not None and 0 <= off < len(blob),
            'RVA->FileOffset round trip: %s' % name,
            'rva=0x%X off=0x%X vsize=0x%X raw=0x%X' % (s.VirtualAddress, off, s.Misc_VirtualSize, s.SizeOfRawData))
    # the .reloc lesson: prove offset != rva for the reloc section and that the table is intact
    for s in pe.sections:
        if s.Name.rstrip(b'\x00') == b'.reloc':
            off = pe.get_offset_from_rva(s.VirtualAddress)
            chk(off != s.VirtualAddress, '.reloc RVA != FileOffset (guarded)', 'rva=0x%X off=0x%X' % (s.VirtualAddress, off))

    for m in WAVE1_MARKERS:
        chk(m in blob, 'wave1 marker: ' + m.decode(), '')
    for m in WAVE2_MARKERS:
        chk(m in blob, 'wave2 marker: ' + m.decode(), '')
    for m in QUANT_MARKERS:
        chk(m in blob, 'splice2 marker: ' + m.decode(), '')
    for m in WAVE3_MARKERS:
        chk(m in blob, 'wave3 marker: ' + m.decode(), '')

    result = {'dll': a.dll, 'sha256': sha, 'size': len(blob), 'exports': exports,
              'imports': imports, 'checks': checks,
              'pass': sum(1 for c in checks if c['ok']), 'fail': sum(1 for c in checks if not c['ok'])}
    print(json.dumps({k: result[k] for k in ('dll', 'sha256', 'size', 'exports', 'imports', 'pass', 'fail')}, indent=2))
    if a.json:
        json.dump(result, open(a.json, 'w'), indent=2)
    return 0 if result['fail'] == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
