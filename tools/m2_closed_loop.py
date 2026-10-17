#!/usr/bin/env python3
"""
SATRAP MetaClaw — Two-Tier Closed-Loop Autonomous Evolution Harness for Majesty 2
================================================================================
Architecture:
  - Role of Architect: Set high-level target trajectory & bounds.
  - Role of MetaClaw: Autonomous Hypothesis Generation -> Code -> Tier 1 Workbench (Clore) -> Tier 2 Wine Bench (Mac) -> Evaluator -> Retain/Revert.
  - Tier 1 (Clore #84966): Differential test (100k vectors, 10M tests) in RAM tmpfs in ~50ms.
  - Tier 2 (Mac Wine 11): 20s combat dwell on Day 230 only if Tier 1 PASSES with speedup.
"""

import atexit
import hashlib
import json
import os
import signal
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

REPO = Path("./scratch/majesty2-re")
SYSWOW = Path("/Applications/Gamez/Majesty 2.app/Contents/SharedSupport/prefix/drive_c/windows/syswow64")
TARGET_DLL = SYSWOW / "d3d9.dll"
PROXY_CPP = REPO / "src" / "d3d9_proxy.cpp"
BEST_CPP = REPO / "src" / "d3d9_proxy.cpp.best"
BEST_DLL = SYSWOW / "d3d9.dll.best"
# --- MetaClaw v4: staging + golden restore (experiments never stay in the live install) ---
GAME_DLL = SYSWOW.parents[1] / "GOG Games" / "Majesty 2 Collection" / "d3d9.dll"
LIVE_DLLS = [TARGET_DLL, GAME_DLL]                     # every place the game can load our proxy from
STAGE_DLL = REPO / "build" / "candidate.dll"            # compile output; NEVER the live install
GOLDEN_DLL = REPO / "build" / "golden" / "d3d9.dll"     # known-good build; live install is restored to it after every run
AUTOSTART_FILE = Path("/tmp/m2_autostart.txt")          # stale trigger hijacks normal launches into bench_heavy
BEST_JSON = Path("/tmp/m2_metaclaw_best_v4.json")  # v4: new file so old polluted baselines are ignored
LOG_FILE = Path("/tmp/m2_closed_loop.log")
BLACKBOARD = REPO / "HARNESS_BLACKBOARD.md"
WORKBENCH_CPP = REPO / "src" / "m2_workbench.cpp"

CLORE_EXEC = "$HOME/.local/bin/cc-clore-exec"
DELEGATE = "$HOME/.local/bin/cc-hybrid-delegate"
BENCH_PY = REPO / "tools" / "m2_bench.py"
PYTHON_VENV = REPO / ".venv" / "bin" / "python3"


def log(msg: str):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    formatted = f"[{timestamp}] {msg}"
    print(formatted, flush=True)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(formatted + "\n")


def run_cmd(cmd: str, timeout: int = 240) -> tuple[int, str]:
    try:
        res = subprocess.run(
            cmd, shell=True, text=True, capture_output=True, timeout=timeout
        )
        out = (res.stdout or "") + (res.stderr or "")
        return res.returncode, out.strip()
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def run_clore_workbench() -> tuple[bool, str]:
    """Tier 1: Run isolated micro-workbench on Clore #84966 in RAM tmpfs (~0.5s)."""
    if not WORKBENCH_CPP.exists():
        return True, "No workbench file, skipping Tier 1"
    
    cmd = (
        f'{CLORE_EXEC} --job m2-workbench '
        f'--sync-in "{WORKBENCH_CPP}" '
        f'"g++ -O3 -msse2 -march=native m2_workbench.cpp -o bench && cp bench /tmp/bench && chmod +x /tmp/bench && /tmp/bench"'
    )
    rc, out = run_cmd(cmd, timeout=30)
    if rc != 0:
        return False, f"Workbench execution failed (rc={rc}): {out[:300]}"
    try:
        start_idx = out.find("{")
        end_idx = out.rfind("}") + 1
        if start_idx != -1 and end_idx != -1:
            data = json.loads(out[start_idx:end_idx])
            if data.get("status") == "PASS" and data.get("mismatches", 1) == 0:
                return True, f"Tier 1 PASS (mismatches=0, sse2={data.get('sse2_ms', 0):.2f}ms)"
    except Exception as e:
        return False, f"Failed to parse workbench json: {e}, raw output: {out[:200]}"
    return False, f"Tier 1 FAILED or mismatches > 0: {out[:300]}"


def compile_via_clore() -> bool:
    src_dir = REPO / "src"
    STAGE_DLL.parent.mkdir(parents=True, exist_ok=True)
    STAGE_DLL.unlink(missing_ok=True)
    cmd = (
        f'{CLORE_EXEC} --job m2-loop-build '
        f'--sync-in "{src_dir}" '
        f'--sync-out d3d9.dll:"{STAGE_DLL}" '
        f'"i686-w64-mingw32-g++ -shared -std=c++17 -O3 -msse2 -mfpmath=sse -s -static-libgcc -static-libstdc++ -Wl,--kill-at d3d9_proxy.cpp -o d3d9.dll -lgdi32 -luser32"'
    )
    rc, out = run_cmd(cmd, timeout=45)
    if rc != 0:
        log(f"Clore Compilation FAILED:\n{out[:500]}")
        return False
    if not (STAGE_DLL.exists() and STAGE_DLL.stat().st_size > 50000):
        log("Compile produced no usable staged DLL.")
        return False
    if b"Direct3DCreate9" not in STAGE_DLL.read_bytes():
        log("Staged DLL does not contain Direct3DCreate9 (missing d3d9.def / --kill-at?). Rejecting.")
        return False
    return True


def _run_benchmark_inner(label: str, out_json: Path) -> dict:
    cmd = (
        f'M2_PROFILE=1 WINEMSYNC=0 WINEESYNC=0 DXVK_ASYNC=1 MTL_HUD_ENABLED=0 '
        f'"{PYTHON_VENV}" "{BENCH_PY}" --label {label} --save bench_heavy --dwell 20 --speed 5 --out "{out_json}"'
    )
    out_json.unlink(missing_ok=True)  # v4: never read a stale json left by an earlier run with the same name
    rc, out = run_cmd(cmd, timeout=240)
    if out_json.exists():
        try:
            with open(out_json, "r") as f:
                return json.load(f)
        except Exception as e:
            log(f"Failed to parse benchmark json: {e}")
    return {"status": "FAIL", "error": out[-500:]}



WIN_THRESHOLD = 1.10   # v4: candidate must beat the measured baseline by >=10% in TWO independent runs (run-to-run noise is ~5-10%)

V4_RULES = """

=== MetaClaw v4: ПРАВИЛА ЭТОЙ ИТЕРАЦИИ (обязательны) ===
1. Замер идёт через staging: твоя сборка попадает в живую установку игры только на время замера, затем автоматически откатывается на golden. Не копируй DLL в игру сам.
2. Итерация засчитывается только если ты реально изменил src/d3d9_proxy.cpp. Файл не изменён (или равен .best) -> итерация пустая, замер не запускается.
3. Не пиши в HARNESS_BLACKBOARD.md: запись о победе делает только сам цикл после двух независимых замеров (порог +10%).
4. Не утверждай, что M2Arena ухудшил FPS: прошлые замеры с arena были перепутаны с отключением хуков Hm/Mat (они же дают артефакт ландшафта). Для arena нужен чистый A/B одним флагом.
5. Цель выбирай по Amdahl: назови долю функции в профиле (профиль /tmp/m2_x86_profile_5x.txt устарел, пересними) и оценку потолка 1/(1-доля*(1-1/ускорение)).
6. Одна гипотеза - одна правка. Последняя строка ответа: CHANGED: <функция/хук>; EXPECTED: <метрика и величина>.
"""


def sha256_file(path) -> str:
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except FileNotFoundError:
        return ""


def deploy_live(src_dll: Path):
    """Copy a DLL into BOTH live locations (prefix syswow64 + game dir) for the duration of one benchmark."""
    for t in LIVE_DLLS:
        if t.parent.exists():
            shutil.copy2(src_dll, t)


def restore_live_install():
    """Put the golden DLL back and remove the stale autostart trigger. Idempotent; safe in finally/atexit/signal."""
    try:
        if GOLDEN_DLL.exists():
            deploy_live(GOLDEN_DLL)
        AUTOSTART_FILE.unlink(missing_ok=True)
    except Exception as e:  # never let cleanup mask the real error
        log(f"restore_live_install error: {e}")


def extract_metrics(results: dict) -> tuple:
    combat = results.get("combat", {}) or {}
    phases = results.get("phases_combat", {}) or {}
    fps = combat.get("avg_fps", results.get("avg_fps", 0.0)) or 0.0
    quant = phases.get("quant_ms", results.get("quant_ms", 0.0)) or 0.0  # missing quant -> 0 -> fails the quant>20 guard
    return float(fps), float(quant)


def extract_load(results: dict) -> float:
    """Engine seconds for 'LEVEL LOADED' (bench_heavy loads ~80 s today). 0.0 if the level never finished loading."""
    load = results.get("load", {}) or {}
    if not load.get("level_loaded"):
        return 0.0
    return float(load.get("level_load_engine_sec") or 0.0)


def beats_baseline(results: dict, base_fps: float, base_load: float) -> bool:
    """v4 win = (avg_fps +10%  OR  level-load time -15%) AND no >5% regression in the other metric.
    The level must really have loaded and the simulation must be running (quant_ms > 20)."""
    fps, quant = extract_metrics(results)
    load_s = extract_load(results)
    if results.get("status") != "PASS" or quant <= 20.0 or load_s <= 0.0:
        return False
    fps_win = fps >= base_fps * WIN_THRESHOLD
    load_win = base_load > 0 and load_s <= base_load * 0.85
    no_regress = fps >= base_fps * 0.95 and (base_load <= 0 or load_s <= base_load * 1.05)
    return (fps_win or load_win) and no_regress


def preflight():
    GOLDEN_DLL.parent.mkdir(parents=True, exist_ok=True)
    if not GOLDEN_DLL.exists():
        if TARGET_DLL.exists():
            shutil.copy2(TARGET_DLL, GOLDEN_DLL)
            (GOLDEN_DLL.parent / "SEEDED_UNVERIFIED.txt").write_text(
                f"Seeded {datetime.now().isoformat()} from live syswow64/d3d9.dll sha256={sha256_file(GOLDEN_DLL)}\n"
                "NOT verified for play. Card W0 must launch it (fresh match, 1x) and then cp a verified build over this file.\n",
                encoding="utf-8")
            log("WARNING: golden seeded from the live DLL (unverified for play). See build/golden/SEEDED_UNVERIFIED.txt")
        else:
            log("FATAL: no golden DLL and no live DLL.")
            sys.exit(2)
    AUTOSTART_FILE.unlink(missing_ok=True)
    atexit.register(restore_live_install)

    def _sig(signum, frame):
        restore_live_install()
        sys.exit(128 + signum)

    signal.signal(signal.SIGTERM, _sig)
    signal.signal(signal.SIGINT, _sig)


def run_benchmark(label: str, out_json: Path) -> dict:
    """Staging benchmark: STAGE_DLL is live ONLY for the duration of the run; golden is always restored."""
    try:
        if not STAGE_DLL.exists():
            return {"status": "FAIL", "error": "no staged candidate DLL"}
        deploy_live(STAGE_DLL)
        return _run_benchmark_inner(label, out_json)
    finally:
        restore_live_install()


def main():
    log("=== SATRAP MetaClaw Two-Tier Closed-Loop Started ===")

    # Baseline checkpoint
    if not BEST_CPP.exists() and PROXY_CPP.exists():
        shutil.copy2(PROXY_CPP, BEST_CPP)
    preflight()
    if not BEST_DLL.exists() and GOLDEN_DLL.exists():
        shutil.copy2(GOLDEN_DLL, BEST_DLL)

    best_fps = 0.0
    best_quant = 0.0
    best_load = 0.0
    if BEST_JSON.exists():
        try:
            with open(BEST_JSON) as f:
                d = json.load(f)
                best_fps = d.get("avg_fps", best_fps)
                best_quant = d.get("quant_ms", best_quant)
                best_load = d.get("load_s", best_load)
                log(f"Loaded existing baseline: avg_fps={best_fps:.2f}, quant_ms={best_quant:.2f}ms")
        except Exception:
            pass

    if best_fps <= 0.0 or best_load <= 0.0:
        log("No v4 baseline yet: measuring the GOLDEN build (no agent, no edits)...")
        shutil.copy2(GOLDEN_DLL, STAGE_DLL)
        base_json = Path("/tmp/m2_v4_baseline.json")
        base_res = run_benchmark("v4_baseline", base_json)
        b_fps, b_quant = extract_metrics(base_res)
        b_load = extract_load(base_res)
        if base_res.get("status") != "PASS" or b_fps <= 0.0 or b_quant <= 20.0 or b_load <= 0.0:
            log(f"FATAL: golden baseline did not produce a valid measurement (status={base_res.get('status')}, fps={b_fps}, quant={b_quant}, level_load_s={b_load}). Fix the launch first (card W0).")
            return 2
        best_fps, best_quant, best_load = b_fps, b_quant, b_load
        BEST_JSON.write_text(json.dumps({"avg_fps": best_fps, "quant_ms": best_quant, "load_s": best_load, "source": "golden-baseline"}))
        log(f"Measured golden baseline: avg_fps={best_fps:.2f}, quant_ms={best_quant:.2f}ms, level_load={best_load:.1f}s")

    target_fps = 20.0
    iteration = 1
    max_iterations = 30
    noop_streak = 0

    while iteration <= max_iterations and best_fps < target_fps:
        log(f"\n>>> ITERATION {iteration}/{max_iterations} (Target >= {target_fps} FPS, Current Best = {best_fps:.2f} FPS) <<<")

        prompt_text = f"""Ты — автономный архитектор движка GEM 3 в замкнутом цикле MetaClaw.
Твоя цель: поднять FPS в Majesty 2 (Day 230 / save167 / 300+ юнитов, 5x speed) с текущих {best_fps:.2f} FPS до {target_fps} FPS!
Текущее лучшее время симуляции quant_ms = {best_quant:.2f}ms.

ВНИМАНИЕ: НИКОГДА не включай обратно старые хуки Fast_Heightmap_* (0x05EF60, 0x0B3E90, 0x0B46D0) и Fast_Mat3x4_* / Fast_Mat4x4_* — они вызывают артефакт искажения ландшафта (гигантский коричневый треугольник)!
Текущая база уже содержит M2Arena (lock-free thread-local allocator) и XBoneParallel (многопоточный расчет анимаций на 8 P-ядрах).

Ты свободен автономно формулировать и проверять любые инженерные гипотезы:
1. Spatial Frustum Culling (отсечение матриц, анимаций и поиска целей для сущностей вне поля зрения камеры в d3d9_proxy.cpp).
2. Particle Flow Throttling (отсечение генерации частиц pf.dll для невидимых эмиттеров).
3. Decoupled Render / 120 FPS Present (отвязка Present() от симуляции, сглаживание камеры).
4. Pruning шины eEventRouter::Process (ранний выход для спама событий между неактивными сущностями).
5. Тонкая настройка размера чанка (chunkSize) и количества потоков в XBoneParallel::GetEngine().Init().

Твоя задача на эту итерацию:
1. Изучи текущий {PROXY_CPP} и блэкборд {BLACKBOARD}.
2. Выбери ОДНУ конкретную гипотезу, которая ещё не реализована или может быть улучшена.
3. Внедри изменения прямо в {PROXY_CPP}.
4. Убедись, что код компилируется и не содержит синтаксических ошибок.
Харнесс сам проверит код на Clore и запустит тест! Отчитайся о гипотезе."""

        prompt_text += V4_RULES + f"\nИзмеренный baseline: avg_fps={best_fps:.2f}, quant_ms={best_quant:.2f}ms.\n"
        src_hash_before = sha256_file(PROXY_CPP)
        prompt_file = Path(f"/tmp/m2_prompt_iter_{iteration}.txt")
        prompt_file.write_text(prompt_text, encoding="utf-8")

        # Invoke Haiku 5.5 via stdin with low effort (fast 3s turn)
        log("Invoking Claude Haiku 5.5 to formulate and apply optimization hypothesis...")
        report_file = Path(f"/tmp/m2_iteration_{iteration}_report.md")
        delegate_cmd = (
            f'cat "{prompt_file}" | {DELEGATE} --dir "{REPO}" --dir "/tmp" --dir "/Applications/Gamez/Majesty 2.app" '
            f'--model haiku --effort low --max-turns 15 '
            f'--out "{report_file}"'
        )
        d_rc, d_out = run_cmd(delegate_cmd, timeout=300)
        log(f"Haiku 5.5 finished turn (rc={d_rc}).")
        src_hash_after = sha256_file(PROXY_CPP)
        if src_hash_after == src_hash_before or src_hash_after == sha256_file(BEST_CPP):
            noop_streak += 1
            log(f"NO-OP: agent changed nothing (rc={d_rc}, streak={noop_streak}). Skipping compile+bench - an unchanged build is not a measurement.")
            if noop_streak >= 5:
                log("5 consecutive no-ops: the agent path is broken. Stopping the loop.")
                break
            iteration += 1
            continue
        noop_streak = 0

        # Step 2: Compile via Clore
        log("Compiling patched d3d9_proxy.cpp via Clore 32T RAM tmpfs...")
        if not compile_via_clore():
            log("Compilation failed. Rolling back to .best...")
            shutil.copy2(BEST_CPP, PROXY_CPP)
            restore_live_install()
            iteration += 1
            continue

        # Step 3: Tier 1 Workbench on Clore (0.5s)
        log("Running Tier 1 Clore RAM Workbench...")
        wb_pass, wb_msg = run_clore_workbench()
        log(f"Tier 1 Workbench: {wb_msg}")
        if not wb_pass:
            log("Tier 1 rejected patch. Rolling back without running Mac game...")
            shutil.copy2(BEST_CPP, PROXY_CPP)
            restore_live_install()
            iteration += 1
            continue

        # Step 4: Tier 2 Mac In-Game Benchmark (Day 230, 20s dwell, 240s timeout)
        iter_json = Path(f"/tmp/m2_iter_{iteration}.json")
        log("Tier 1 passed! Running Tier 2 deterministic benchmark on Mac (Day 230, speed 5)...")
        results = run_benchmark(f"iter_{iteration}", iter_json)

        status = results.get("status", "FAIL")
        new_fps, new_quant = extract_metrics(results)
        new_load = extract_load(results)

        log(f"Tier 2 Verdict: status={status}, avg_fps={new_fps:.2f} (best={best_fps:.2f}), quant_ms={new_quant:.2f}ms (best={best_quant:.2f}ms)")

        # Evaluator Techgate: strictly compare with best, guard against paused simulations (quant < 20ms)
        confirmed = False
        log(f"Tier 2 load: level_load={new_load:.1f}s (best={best_load:.1f}s)")
        if beats_baseline(results, best_fps, best_load):
            log(f"Candidate looks like a win (fps {(new_fps / best_fps - 1) * 100:+.1f}%, load {new_load:.1f}s vs {best_load:.1f}s). Confirming with an independent second run...")
            confirm_json = Path(f"/tmp/m2_iter_{iteration}_confirm.json")
            cres = run_benchmark(f"iter_{iteration}_confirm", confirm_json)
            c_fps, c_quant = extract_metrics(cres)
            log(f"Confirm run: status={cres.get('status')}, avg_fps={c_fps:.2f}, quant_ms={c_quant:.2f}ms")
            if beats_baseline(cres, best_fps, best_load):
                confirmed = True
                new_fps = min(new_fps, c_fps)        # conservative: keep the worse of the two runs
                new_quant = max(new_quant, c_quant)
                new_load = max(new_load, extract_load(cres))
        if confirmed:
            gain = ((new_fps - best_fps) / best_fps) * 100
            log(f"SUCCESS: Hypothesis WON! FPS improved by {gain:+.1f}% ({best_fps:.2f} -> {new_fps:.2f} FPS). Retaining as new baseline!")
            best_fps = new_fps
            best_quant = new_quant
            best_load = new_load
            shutil.copy2(PROXY_CPP, BEST_CPP)
            shutil.copy2(STAGE_DLL, BEST_DLL)
            BEST_JSON.write_text(json.dumps({"avg_fps": new_fps, "quant_ms": new_quant, "load_s": new_load, "iteration": iteration, "cpp_sha256": sha256_file(PROXY_CPP), "runs": 2}))
            with open(BLACKBOARD, "a", encoding="utf-8") as bb:
                bb.write(f"\n- [LOOP-VERIFIED {datetime.now():%Y-%m-%d %H:%M}] iter {iteration}: avg_fps={new_fps:.2f} quant_ms={new_quant:.2f}ms level_load={new_load:.1f}s ({gain:+.1f}% fps vs measured baseline, 2 runs, cpp_sha={sha256_file(PROXY_CPP)[:10]})\n")
        else:
            log("REJECTED: In-game performance did not improve or benchmark failed. Rolling back...")
            shutil.copy2(BEST_CPP, PROXY_CPP)
            restore_live_install()

        iteration += 1
        time.sleep(2)

    log(f"=== Closed-Loop Complete: Final Best FPS = {best_fps:.2f} FPS ===")


if __name__ == "__main__":
    sys.exit(main() or 0)
