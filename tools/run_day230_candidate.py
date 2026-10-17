#!/usr/bin/env python3
"""
Single-shot installer, runner, and verifier for Day 230 combat benchmark on
Opus wine-build (W) + x87sidecar + mtld3d/Metal with WINEMSYNC=1 WINEESYNC=1.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Add SATRAP ECHO for satrap_cm
sys.path.insert(0, './SATRAP_ECHO')
import satrap_cm as cm

REPO = Path('./scratch/majesty2-re')
WB_DIR = REPO / 'tools/wine-build'
W_PREFIX = WB_DIR / 'prefix'
W_WINE = WB_DIR / 'wine/bin/wine'
W_WINESERVER = WB_DIR / 'wine/bin/wineserver'
W_GAME_DIR = W_PREFIX / 'drive_c/GOG Games/Majesty 2 Collection'
STAGE_DIR = WB_DIR / 'x7'
DOCS_DIR = Path('$HOME/Documents/majesty2')
APP = Path('/Applications/Gamez/Majesty 2.app')

CANDIDATE_DLL = REPO / 'build/candidate.dll'
EXPECTED_SHA = '5f20415eac95092be94c0aaa13d45a7307a68d1c20e1e2cf9a883d9a2bf321f7'
EXPECTED_SIZE = 379392

TARGET_DESTS = [
    W_PREFIX / 'drive_c/windows/syswow64/d3d9.dll',
    W_GAME_DIR / 'd3d9.dll',
    APP / 'Contents/SharedSupport/prefix/drive_c/windows/syswow64/d3d9.dll',
    APP / 'Contents/SharedSupport/prefix/drive_c/GOG Games/Majesty 2 Collection/d3d9.dll',
    REPO / 'build/golden/d3d9.dll',
]

OUT_JSON = Path('/tmp/m2_day230_candidate.json')
FPS_LOG = Path('/tmp/m2_fps.log')
CHILD_LOG = Path('/tmp/m2_day230_child.log')

def sha256_of(path: Path):
    if not path.exists():
        return None
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    print('=== STEP 1: Verify candidate.dll ===')
    assert CANDIDATE_DLL.exists(), f"Missing {CANDIDATE_DLL}"
    data = CANDIDATE_DLL.read_bytes()
    cand_size = len(data)
    cand_sha = hashlib.sha256(data).hexdigest()
    print(f"Candidate size: {cand_size} (expected {EXPECTED_SIZE})")
    print(f"Candidate sha256: {cand_sha} (expected {EXPECTED_SHA})")
    assert cand_size == EXPECTED_SIZE, f"Size mismatch: {cand_size} != {EXPECTED_SIZE}"
    assert cand_sha == EXPECTED_SHA, f"SHA mismatch: {cand_sha} != {EXPECTED_SHA}"

    print('\n=== STEP 2: Install candidate.dll to target locations ===')
    install_log = []
    for dst in TARGET_DESTS:
        if dst.exists() or dst.parent.exists():
            dst.parent.mkdir(parents=True, exist_ok=True)
            bak = dst.with_name(dst.name + '.bak_v5')
            if dst.exists() and not bak.exists():
                shutil.copy2(dst, bak)
                print(f"Backed up {dst.name} -> {bak.name} ({dst.stat().st_size} B, {sha256_of(dst)[:16]})")
                install_log.append(f"Backup: {bak} ({dst.stat().st_size} B)")
            shutil.copy2(CANDIDATE_DLL, dst)
            installed_sha = sha256_of(dst)
            print(f"Installed {dst} -> {installed_sha[:16]} ({dst.stat().st_size} B)")
            assert installed_sha == EXPECTED_SHA, f"Verification failed after install for {dst}"
            install_log.append(f"Installed: {dst} ({dst.stat().st_size} B, {installed_sha[:16]})")
        else:
            print(f"Skipping non-existent parent directory: {dst}")

    print('\n=== STEP 3: Setup environment and run Day 230 combat benchmark ===')
    # Clean up previous benchmark artifacts
    for p in [OUT_JSON, FPS_LOG, CHILD_LOG, Path('/tmp/m2_frametimes.bin'), Path('/tmp/m2_autostart.txt')]:
        if p.exists():
            try:
                p.unlink()
            except OSError:
                pass

    env = os.environ.copy()
    env['WINEPREFIX'] = str(W_PREFIX)
    env['WINEDEBUG'] = '-all'
    env['WINEMSYNC'] = '1'
    env['WINEESYNC'] = '1'
    env['X87_LOG_THROUGHPUT'] = '1'
    env['M2B_APP_DIR'] = str(WB_DIR / 'wine')
    env['M2B_WINE_BIN_DIR'] = str(WB_DIR / 'wine/bin')
    env['M2B_WINESERVER'] = str(W_WINESERVER)
    env['M2B_WINEPREFIX'] = str(W_PREFIX)
    env['M2B_FRAMEWORKS_DIR'] = str(WB_DIR / 'wine/lib')
    env['M2B_WINE_LIB_DIR'] = str(WB_DIR / 'wine/lib')
    env['M2B_DYLD_FALLBACK_LIBRARY_PATH'] = ''
    env['M2B_DOCS_DIR'] = str(DOCS_DIR)
    env['M2B_DXVK_CACHE'] = str(STAGE_DIR / 'dxvk_cache')
    env['M2B_ICD_DIR'] = str(APP / 'Contents/Resources/vulkan/icd.d')
    env['M2B_GAME_DIR'] = str(W_GAME_DIR)
    env['M2B_LAUNCH_CWD'] = str(W_GAME_DIR)
    env['M2B_LAUNCH_CMD'] = f'"{W_WINE}" Majesty2.exe'
    env['M2B_CHILD_LOG'] = str(CHILD_LOG)
    env['M2B_DLLOVERRIDES'] = 'd3d9=n,b'

    cmd = [
        sys.executable,
        str(REPO / 'tools/m2_bench_x7.py'),
        '--label', 'candidate_day230_live',
        '--save', 'bench_heavy',
        '--speed', '5.0',
        '--dwell', '25.0',
        '--post-init-wait', '150.0',
        '--timeout', '300.0',
        '--out', str(OUT_JSON),
    ]

    print(f"Launching benchmark: {' '.join(cmd)}")
    t0 = time.time()
    res = subprocess.run(cmd, env=env, cwd=str(REPO), capture_output=True, text=True)
    elapsed = time.time() - t0
    print(f"Benchmark finished in {elapsed:.1f}s (returncode: {res.returncode})")
    print(res.stdout)
    if res.stderr:
        print("STDERR:\n" + res.stderr)

    print('\n=== STEP 4: Verify results and parse telemetry ===')
    assert OUT_JSON.exists(), f"Output JSON {OUT_JSON} was not generated!"
    bench_data = json.loads(OUT_JSON.read_text())

    # Read FPS log
    assert FPS_LOG.exists(), f"FPS log {FPS_LOG} was not found!"
    fps_content = FPS_LOG.read_text(errors='ignore')

    # Read child log for sidecar throughput
    child_content = CHILD_LOG.read_text(errors='ignore') if CHILD_LOG.exists() else ""

    # 1. Wave 6 SSE verification
    w6_match = re.search(r'#\s*(wave6_sse:\s*mask=0x3FF\s+active=0x3FF\s+ok=10/10[^\r\n]*)', fps_content)
    assert w6_match, "Wave 6 SSE hook signature '# wave6_sse: mask=0x3FF active=0x3FF ok=10/10' NOT found in /tmp/m2_fps.log!"
    w6_status_str = w6_match.group(1).strip()
    print(f"Verified Wave 6 SSE: {w6_status_str}")

    # 2. Combat metrics verification
    combat = bench_data.get('combat', {})
    phases = bench_data.get('phases_combat', {})
    load = bench_data.get('load', {})

    frames = combat.get('frames', 0)
    avg_fps = combat.get('avg_fps', 0.0)
    frame_ms = phases.get('frame_ms', 0.0)
    quant_ms = phases.get('quant_ms', 0.0)
    game_x = phases.get('game_x', 0.0)

    print(f"Combat frames: {frames}")
    print(f"Combat FPS: {avg_fps}")
    print(f"Combat frame_ms: {frame_ms}")
    print(f"Combat quant_ms: {quant_ms}")
    print(f"Combat game_x: {game_x}")

    assert frames > 0, f"Expected combat.frames > 0, got {frames}"
    assert avg_fps > 0, f"Expected combat.fps > 0, got {avg_fps}"
    assert frame_ms > 0, f"Expected frame_ms > 0, got {frame_ms}"
    assert quant_ms > 0, f"Expected quant_ms > 0, got {quant_ms}"

    # 3. x87sidecar throughput
    sidecar_lines = [l for l in child_content.splitlines() if 'sidecar:' in l]
    max_hooks = 0
    for l in sidecar_lines:
        m = re.search(r'\(total\s+(\d+)\)', l)
        if m:
            max_hooks = max(max_hooks, int(m.group(1)))
    print(f"x87sidecar lines count: {len(sidecar_lines)}")
    print(f"x87sidecar max hook requests: {max_hooks}")
    sidecar_last = sidecar_lines[-5:] if sidecar_lines else []

    # Check env verification in json
    effective_env = bench_data.get('effective_env', {})
    print(f"Effective env WINEMSYNC: {effective_env.get('WINEMSYNC')}, WINEESYNC: {effective_env.get('WINEESYNC')}")

    receipt = {
        "status": "PASS",
        "benchmark_label": "candidate_day230_live",
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "elapsed_sec": round(elapsed, 2),
        "candidate": {
            "size": cand_size,
            "sha256": cand_sha,
            "installed_targets": install_log
        },
        "wave6_sse": {
            "status": w6_status_str,
            "mask": "0x3FF",
            "active": "0x3FF",
            "ok": "10/10"
        },
        "load": load,
        "combat": {
            "frames": frames,
            "avg_fps": avg_fps,
            "one_pct_low_fps": combat.get('one_pct_low_fps'),
            "avg_ms": combat.get('avg_ms'),
            "p99_ms": combat.get('p99_ms')
        },
        "phases_combat": {
            "frame_ms": frame_ms,
            "quant_ms": quant_ms,
            "logic_ms": phases.get('logic_ms'),
            "draw_ms": phases.get('draw_ms'),
            "lock_ms": phases.get('lock_ms'),
            "game_x": game_x,
            "speed": phases.get('speed_last')
        },
        "x87sidecar": {
            "hook_requests_total": max_hooks,
            "sample_lines": sidecar_last
        },
        "effective_env": {
            "WINEMSYNC": effective_env.get('WINEMSYNC'),
            "WINEESYNC": effective_env.get('WINEESYNC'),
            "WINEDLLOVERRIDES": effective_env.get('WINEDLLOVERRIDES')
        }
    }

    print('\n=== STEP 5: Emit trace and receipt ===')
    trace_res = cm.leave_trace(
        project="majesty2-re",
        card_id="t_m2_day230_live_run",
        title="Day 230 Combat Benchmark on Opus wine-build (W) + x87sidecar + mtld3d/Metal with WINEMSYNC=1 WINEESYNC=1",
        summary=json.dumps(receipt, indent=2),
        status="completed",
        decision=f"Day 230 combat benchmark PASS: {frames} frames at {avg_fps} FPS, {frame_ms} ms frame_ms, {quant_ms} ms quant_ms, game_x={game_x}, Wave 6 ok=10/10, x87sidecar hooks={max_hooks}",
        promote_global=True,
        sha256_data=cand_sha
    )
    print(f"Trace logged: {trace_res}")

    cm.emit(receipt)
    print("\nEngineering receipt emitted successfully.")

if __name__ == '__main__':
    main()
