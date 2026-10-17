#!/usr/bin/env python3
"""
Deterministic Event-Driven Benchmark Harness for Majesty 2 (macOS / Sikarugir / Wine 11.0)
==========================================================================================
Tracks all real engine startup & level-loading phases from M2_Debug.log:
  1. Wine -> Engine Entry (0.00s engine time)
  2. D3D9 Main Window Created
  3. Common Textures Loaded (~19.5s)
  4. guiMainPage + 28,104 Files Cached (~26.0s)
  5. GamePreloadManager (~16.8s) -> Application::Init() COMPLETE (~39.9s engine / ~45s wall)
  6. Optional: Level Load (BEGIN LOADING LEVEL -> END / Gameplay)
  7. Dwell in 3D Main Menu (or Level) for `--dwell` seconds AFTER readiness, sampling CPU/RSS,
     then perform guaranteed 3-step clean shutdown (wineserver -k -> pkill -9 -> lockfile cleanup).
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import time
from pathlib import Path

# ---------------------------------------------------------------- X7: opt-in path overrides
# Every constant below can be redirected with an M2B_* environment variable so the SAME
# harness drives a second wine (the X7 arm: prebuilt athei/wine-build + x87sidecar, in its own
# prefix) instead of the Sikarugir app bundle.  Unset (the normal case) reproduces the
# historical values byte for byte, so the Sikarugir arms and every existing caller are
# unaffected.  The values the X7 script exports are documented in re/X7_WINEBUILD_GAME.md.
def _env_path(name, default):
    """Path override: M2B_<name> wins when set and non-empty, else the historical constant."""
    v = os.environ.get(name)
    return Path(v) if v else default


APP_DIR = _env_path("M2B_APP_DIR", Path("/Applications/Gamez/Majesty 2.app"))
LAUNCHER = _env_path("M2B_LAUNCHER", APP_DIR / "Contents/MacOS/launcher")
WINE_BIN_DIR = _env_path("M2B_WINE_BIN_DIR", APP_DIR / "Contents/SharedSupport/wine/bin")
WINESERVER = _env_path("M2B_WINESERVER", WINE_BIN_DIR / "wineserver")
WINEPREFIX = _env_path("M2B_WINEPREFIX", APP_DIR / "Contents/SharedSupport/prefix")
FRAMEWORKS_DIR = _env_path("M2B_FRAMEWORKS_DIR", APP_DIR / "Contents/Frameworks")
WINE_LIB_DIR = _env_path("M2B_WINE_LIB_DIR", APP_DIR / "Contents/SharedSupport/wine/lib")

DOCS_DIR = _env_path("M2B_DOCS_DIR", Path("$HOME/Documents/majesty2"))
DEBUG_LOG = DOCS_DIR / "M2_Debug.log"
ERROR_LOG = DOCS_DIR / "M2_Error.log"
OPTIONS_FILE = DOCS_DIR / "profiles/0/m2_options.set"
DXVK_CACHE_DIR = _env_path("M2B_DXVK_CACHE", DOCS_DIR / "dxvk_cache")
ICD_DIR = _env_path("M2B_ICD_DIR", APP_DIR / "Contents/Resources/vulkan/icd.d")
ICD_FILES = {
    "kosmickrisp": ICD_DIR / "kosmickrisp_mesa_icd.json",
    "moltenvk": ICD_DIR / "MoltenVK_icd.json",
}

# Bench knobs set from the CLI (main) before a run.
BENCH_OPTS = {"hud": False, "icd": None, "clear_mt": False}


def game_env():
    """Env shared by wineserver -k and the game. WINEMSYNC/WINEESYNC must match between server and clients."""
    env = os.environ.copy()
    env["WINEPREFIX"] = str(WINEPREFIX)
    # An empty M2B_DYLD_FALLBACK_LIBRARY_PATH disables the fallback (wine-build is relocatable
    # and needs no DYLD_* at all); unset keeps the Sikarugir value.
    if "M2B_DYLD_FALLBACK_LIBRARY_PATH" in os.environ:
        env["DYLD_FALLBACK_LIBRARY_PATH"] = os.environ["M2B_DYLD_FALLBACK_LIBRARY_PATH"]
    else:
        env["DYLD_FALLBACK_LIBRARY_PATH"] = f"{FRAMEWORKS_DIR}:{WINE_LIB_DIR}"
    env["SikarugirAppWine11"] = "1"
    # WINEMSYNC and WINEESYNC removed: macOS Darwin has no Linux eventfd/futex, causes lock stalls.
    env["ROSETTA_ADVERTISE_AVX"] = "1"
    env["DXVK_ASYNC"] = "1"
    try:
        DXVK_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass  # unwritable cache location must not take the harness down (X7: path is configurable)
    env["DXVK_SHADER_CACHE_PATH"] = str(DXVK_CACHE_DIR)
    if BENCH_OPTS["hud"]:
        env["MTL_HUD_ENABLED"] = "1"
        env["DXVK_HUD"] = "fps,frametimes,gpuload"
    else:
        env["MTL_HUD_ENABLED"] = "0"
        env["DXVK_HUD"] = "0"
    icd = BENCH_OPTS["icd"]
    if icd:
        env["VK_DRIVER_FILES"] = str(ICD_FILES[icd])
        if icd == "moltenvk":
            env["MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS"] = "0"
            env["MVK_CONFIG_FAST_MATH_ENABLED"] = "1"
    if BENCH_OPTS["clear_mt"]:
        env["M2_CLEAR_MT"] = "1"  # proxy clears D3DCREATE_MULTITHREADED in CreateDevice
    return env


PROC_NAMES = ["wineserver", "wine", "wine64-preloader", "start.exe", "Majesty2.exe", "Sikarugir", "launcher"]


def kill_game_procs():
    """Kill by exact process name (never `pkill -f`: it would match our own parent's argv)."""
    subprocess.run(["killall", "-9", *PROC_NAMES], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def clean_processes_and_locks():
    """Safely terminate Wine/Sikarugir and remove stale lockfiles so Sikarugir never hits Secondary run."""
    env = game_env()
    try:
        subprocess.run(
            [str(WINESERVER), "-k"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )
    except Exception:
        pass
    time.sleep(0.8)
    kill_game_procs()
    patterns = [
        "/tmp/.wine-*",
        os.path.join(os.environ.get("TMPDIR", "/tmp"), ".wine-*"),
        "/var/folders/ps/2kh3df492116vkwp4byyf23w0000gp/T/xKWx*",
    ]
    for pattern in patterns:
        for p in glob.glob(pattern):
            shutil.rmtree(p, ignore_errors=True)


def ensure_windowed_mode():
    """Ensure {full_screen 0}, {auto_pause 0} and complete audio mute in m2_options.set."""
    if OPTIONS_FILE.exists():
        text = OPTIONS_FILE.read_text(encoding="utf-8", errors="ignore")
        new_text = re.sub(r"\{full_screen(?:\s+1)?\}", "{full_screen 0}", text)
        new_text = re.sub(r"\{auto_pause(?:\s+1)?\}", "{auto_pause 0}", new_text)
        new_text = re.sub(r"\{(master|main_music|ambience|message|log|sfx|environment|voice|interface)\s+[\d\.]+\}", r"{\1 0}", new_text)
        if new_text != text:
            OPTIONS_FILE.write_text(new_text, encoding="utf-8")


def sample_process_cpu():
    """Sample CPU% and RSS (MB) of Majesty2.exe + wine processes."""
    try:
        out = subprocess.check_output(
            ["ps", "-Ao", "pcpu,rss,comm"], text=True, stderr=subprocess.DEVNULL
        )
        total_cpu = 0.0
        max_rss_kb = 0
        count = 0
        for line in out.splitlines():
            if "Majesty2.exe" in line or "wine" in line:
                parts = line.strip().split(None, 2)
                if len(parts) >= 2:
                    try:
                        total_cpu += float(parts[0])
                        max_rss_kb = max(max_rss_kb, int(parts[1]))
                        count += 1
                    except ValueError:
                        pass
        return {
            "cpu_pct": round(total_cpu, 1),
            "rss_mb": round(max_rss_kb / 1024.0, 1),
            "procs": count,
        }
    except Exception:
        return {"cpu_pct": 0.0, "rss_mb": 0.0, "procs": 0}


def run_benchmark(
    label: str = "baseline",
    dwell_sec: float = 15.0,
    max_timeout: float = 120.0,
    out_json: str = "/tmp/m2_bench_latest.json",
):
    print(f"\n==================================================================")
    print(f"[m2_bench] Starting Benchmark Run: '{label}'")
    print(f"           Dwell after Menu Ready: {dwell_sec}s | Hard Timeout: {max_timeout}s")
    print(f"==================================================================")
    clean_processes_and_locks()
    ensure_windowed_mode()

    if DEBUG_LOG.exists():
        DEBUG_LOG.unlink()
    if ERROR_LOG.exists():
        ERROR_LOG.unlink()

    env = game_env()

    t0 = time.monotonic()
    milestones = {
        "wine_boot_wall_sec": None,
        "main_window_wall_sec": None,
        "common_textures_engine_sec": None,
        "common_textures_wall_sec": None,
        "cache_28k_files_engine_sec": None,
        "cache_28k_files_wall_sec": None,
        "game_preload_manager_engine_sec": None,
        "app_init_total_engine_sec": None,
        "interactive_menu_ready_wall_sec": None,
        "level_load_started": None,
    }
    cpu_samples = []
    ready_time = None

    try:
        proc = subprocess.Popen(
            [str(LAUNCHER)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        while True:
            elapsed = time.monotonic() - t0
            if elapsed >= max_timeout:
                print(f"  [+{elapsed:5.2f}s] [TIMEOUT] Max timeout ({max_timeout}s) reached.")
                break

            if DEBUG_LOG.exists():
                if milestones["wine_boot_wall_sec"] is None:
                    milestones["wine_boot_wall_sec"] = round(elapsed, 2)
                    print(f"  [+{elapsed:5.2f}s] 1/5 Wine -> Engine started (M2_Debug.log created)")

                content = DEBUG_LOG.read_text(encoding="utf-8", errors="ignore")

                if milestones["main_window_wall_sec"] is None and ">> main window started in" in content:
                    milestones["main_window_wall_sec"] = round(elapsed, 2)
                    print(f"  [+{elapsed:5.2f}s] 2/5 D3D9 Window initialized (Windowed Mode)")

                m_tex = re.search(r"([0-9.]+):.*>> Common textures started in ([0-9.]+) s", content)
                if m_tex and milestones["common_textures_wall_sec"] is None:
                    milestones["common_textures_engine_sec"] = float(m_tex.group(2))
                    milestones["common_textures_wall_sec"] = round(elapsed, 2)
                    print(f"  [+{elapsed:5.2f}s] 3/5 Common textures loaded ({m_tex.group(2)}s engine)")

                caches = re.findall(
                    r"([0-9.]+):.*\[xIoProfiler\]\[WriteLine\(\)\] CACHED (\d+) files \(([0-9.]+) Kb\) in ([0-9.]+) sec",
                    content,
                )
                if len(caches) >= 2 and milestones["cache_28k_files_wall_sec"] is None:
                    c_time = round(float(caches[0][3]) + float(caches[1][3]), 3)
                    milestones["cache_28k_files_engine_sec"] = c_time
                    milestones["cache_28k_files_wall_sec"] = round(elapsed, 2)
                    print(f"  [+{elapsed:5.2f}s] 4/5 28,104 pak files cached ({c_time}s engine, 471 MB)")

                m_pre = re.search(r"([0-9.]+):.*>> GamePreloadManager started in ([0-9.]+) s", content)
                if m_pre and milestones["game_preload_manager_engine_sec"] is None:
                    milestones["game_preload_manager_engine_sec"] = float(m_pre.group(2))

                m_app = re.search(r"([0-9.]+):.*>>>> Application::Init\(\) started in ([0-9.]+) s", content)
                if m_app and milestones["interactive_menu_ready_wall_sec"] is None:
                    milestones["app_init_total_engine_sec"] = float(m_app.group(2))
                    milestones["interactive_menu_ready_wall_sec"] = round(elapsed, 2)
                    ready_time = time.monotonic()
                    print(
                        f"  [+{elapsed:5.2f}s] 5/5 >>> INTERACTIVE 3D MAIN MENU READY! "
                        f"(Application::Init = {m_app.group(2)}s, GamePreloadManager = {milestones['game_preload_manager_engine_sec']}s) <<<"
                    )
                    print(f"  [+{elapsed:5.2f}s] Dwelling {dwell_sec}s in 3D Main Menu to sample CPU/Memory...")

                m_lvl = re.search(r"BEGIN LOADING LEVEL ([^\r\n]+)", content)
                if m_lvl and milestones["level_load_started"] is None:
                    milestones["level_load_started"] = m_lvl.group(1).strip()
                    print(f"  [+{elapsed:5.2f}s] [Level Load] Started loading level: {milestones['level_load_started']}")

            if ready_time is not None:
                dwell_elapsed = time.monotonic() - ready_time
                sample = sample_process_cpu()
                if sample["procs"] > 0:
                    cpu_samples.append(sample)
                if dwell_elapsed >= dwell_sec:
                    print(f"  [+{elapsed:5.2f}s] Dwell complete ({dwell_sec}s). Stopping cleanly...")
                    break

            time.sleep(0.5)

    finally:
        clean_processes_and_locks()

    avg_cpu = round(sum(s["cpu_pct"] for s in cpu_samples) / len(cpu_samples), 1) if cpu_samples else 0.0
    peak_rss = max((s["rss_mb"] for s in cpu_samples), default=0.0)

    rem = subprocess.run(["pgrep", "-x", "|".join(PROC_NAMES)], capture_output=True, text=True)
    leftover_pids = [p for p in rem.stdout.splitlines() if p.strip() and int(p.strip()) != os.getpid()]

    result = {
        "label": label,
        "status": "PASS" if milestones["interactive_menu_ready_wall_sec"] is not None and not leftover_pids else "INCOMPLETE",
        "milestones": milestones,
        "runtime_menu_metrics": {
            "dwell_sec": dwell_sec,
            "avg_cpu_pct": avg_cpu,
            "peak_rss_mb": peak_rss,
            "samples": len(cpu_samples),
        },
        "clean_exit_leftover_processes": len(leftover_pids),
    }

    Path(out_json).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("\n=== M2 BENCHMARK SUMMARY ===")
    print(json.dumps(result, indent=2))
    return result


SENSOR_DYLIB = "/tmp/wine_sensor.dylib"
FPS_LOG = Path("/tmp/m2_fps.log")
FT_BIN = Path("/tmp/m2_frametimes.bin")
GAME_DIR = _env_path("M2B_GAME_DIR", WINEPREFIX / "drive_c/GOG Games/Majesty 2 Collection")
STARTUP_FILE = GAME_DIR / "resource.mod/interface/cs/m2Startup.con"  # xConsole::ExecFile appends .con
STARTUP_BASE = 'exec "camera"\nexec "m2GameConst"\nexec "unitBars"\nexec "minimap"\nexec "cursor"\nexec "video"\nexec "credits"\nexec "shop"\nm2Version\nr_hideZNearClip 1\n'
WINE_BIN = WINE_BIN_DIR / "wine"
SAMPLE_OUT = "/tmp/m2_combat_sample.txt"
AUTOSTART_FILE = Path("/tmp/m2_autostart.txt")
X86_PROFILE = Path("/tmp/m2_x86_profile.txt")      # written by the proxy's in-process guest EIP profiler
X86_SAMPLES = Path("/tmp/m2_x86_samples.bin")


def sha256_of(path):
    """Hex digest of a file, or None if it does not exist."""
    if not path.exists():
        return None
    return hashlib.sha256(path.read_bytes()).hexdigest()


def frame_stats(us):
    """avg fps, avg/p99/max ms, 1% low fps (1000/p99) and 0.1% low from a list of frame times in microseconds."""
    if not us:
        return {"frames": 0}
    s = sorted(us)
    n = len(s)
    p99 = s[min(n - 1, int(n * 0.99))] / 1000.0
    p999 = s[min(n - 1, int(n * 0.999))] / 1000.0
    avg = sum(s) / n / 1000.0
    worst1 = s[int(n * 0.99):] or s[-1:]
    return {
        "frames": n,
        "avg_fps": round(1000.0 / avg, 2),
        "avg_ms": round(avg, 3),
        "p99_ms": round(p99, 3),
        "one_pct_low_fps": round(1000.0 / p99, 2) if p99 else 0,
        "one_pct_low_avg_fps": round(1000.0 / (sum(worst1) / len(worst1) / 1000.0), 2),
        "p999_ms": round(p999, 3),
        "max_ms": round(s[-1] / 1000.0, 3),
    }


ENV_PROBE_KEYS = ("WINEMSYNC", "WINEESYNC", "ROSETTA_ADVERTISE_AVX", "DXVK_ASYNC", "DXVK_SHADER_CACHE_PATH",
                  "DXVK_HUD", "MTL_HUD_ENABLED", "VK_DRIVER_FILES", "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS",
                  "MVK_CONFIG_FAST_MATH_ENABLED", "M2_CLEAR_MT", "WINEDLLOVERRIDES")
ENV_PROBE_PREFIXES = ("M2_",)  # every M2_* knob is probed in addition to the fixed keys above
# Processes that carry the game's env. The Majesty2.exe line itself usually shows NO env under
# Rosetta (E1 §5: effective_env was 12xNone in every run JSON), while wineserver / wine64-preloader
# lines do (manual sample in re/ab_msync/140_msync_ab.log). Tuple order = marker match
# AND merge priority: the game exe first, then its preloader, the server, plain wine.
GAME_PROC_MARKERS = ("Majesty2.exe", "wine64-preloader", "wineserver", "wine", "Sikarugir")


def parse_ps_env_line(line: str) -> dict:
    """Extract the fixed probe keys plus every M2_* assignment from a single `ps eww` line."""
    found = {}
    for k in ENV_PROBE_KEYS:
        m = re.search(r"(?:^|\s)" + k + r"=(\S*)", line)
        found[k] = m.group(1) if m else None
    for m in re.finditer(r"(?:^|\s)(" + "|".join(ENV_PROBE_PREFIXES) + r"[A-Z0-9_]+)=(\S*)", line):
        k, v = m.group(1), m.group(2)
        if found.get(k) is None:
            found[k] = v
    return found


def _proc_identity(line: str, marker: str = None) -> str:
    """"pid=NNN proc=NAME" from a `ps -Ao pid,command` line (best effort).

    NAME prefers the matched GAME_PROC_MARKERS entry -- whitespace tokenisation is
    unreliable because the app-bundle path itself contains a space ('Majesty 2.app').
    """
    parts = line.strip().split(None, 1)
    pid = parts[0] if parts else "?"
    if marker:
        name = marker
    else:
        toks = (parts[1] if len(parts) > 1 else "").split()
        name = os.path.basename(toks[0]) if toks else "?"
    return f"pid={pid} proc={name}"


def _line_marker(line: str, low: str):
    """LONGEST (then earliest) marker present in the line: argv[0] identity.

    The bundle path contains a 'wine' directory, so earliest-position alone would
    call every process 'wine'; longest-marker wins instead -- a wine64-preloader
    line (which also contains 'Majesty2.exe' in its args and 'wine' in its path)
    is identified as the preloader.
    """
    best = None
    for m in GAME_PROC_MARKERS:
        i = low.find(m.lower())
        if i >= 0 and (best is None or (-len(m), i) < best[0]):
            best = ((-len(m), i), m)
    return best[1] if best else None


def game_ps_lines(ps_text: str):
    """(marker, line) pairs from `ps eww -Ao pid,command` output belonging to THIS game prefix.

    A line qualifies when it names a wine/wineserver/Majesty2 process AND carries this
    prefix's footprint: the app-bundle path in argv (wine binaries, preloader, game exe)
    or WINEPREFIX=<our prefix> in the env block. Wine processes of other prefixes are
    excluded. Our own `ps eww` invocation and the header are skipped.
    """
    prefix_marks = (str(APP_DIR), str(WINEPREFIX))
    out = []
    for line in ps_text.splitlines():
        stripped = line.strip()
        if not stripped or "ps eww" in line or stripped.split()[0] == "PID":
            continue
        low = stripped.lower()
        marker = _line_marker(line, low)
        if marker and any(p in line for p in prefix_marks):
            out.append((marker, line))
    return out


def probe_game_env_detailed(ps_text=None):
    """Effective env of the running game merged over ALL process lines of this prefix.

    Returns {"effective_env": {...}, "env_source": [pid=.. proc=..], "env_warnings": [...]}
    or None when no game process of this prefix is running. Values are merged with
    priority Majesty2.exe > wine64-preloader/wine > wineserver; when two lines disagree
    on a key an env_warnings entry names both processes.
    """
    if ps_text is None:
        try:
            ps_text = subprocess.check_output(["ps", "eww", "-Ao", "pid,command"], text=True,
                                              stderr=subprocess.DEVNULL)
        except Exception:
            return None
    hits = []
    for marker, line in game_ps_lines(ps_text):
        found = parse_ps_env_line(line)
        if any(v is not None for v in found.values()):
            prio = (GAME_PROC_MARKERS.index(marker) if marker in GAME_PROC_MARKERS
                    else len(GAME_PROC_MARKERS))
            hits.append((prio, _proc_identity(line, marker), found))
    if not hits:
        return None
    hits.sort(key=lambda h: h[0])
    merged, key_src, sources, warnings = {}, {}, [], []
    for _prio, ident, found in hits:
        sources.append(ident)
        for k, v in found.items():
            if v is None:
                continue
            if k not in merged:
                merged[k] = v
                key_src[k] = ident
            elif merged[k] != v:
                warnings.append(f"{k}={v} on {ident} disagrees with {k}={merged[k]} on {key_src[k]}")
    env = {k: merged.get(k) for k in ENV_PROBE_KEYS}
    for k, v in merged.items():
        if k not in env:
            env[k] = v
    return {"effective_env": env, "env_source": sources, "env_warnings": warnings}


def probe_game_env():
    """Effective env of the running game (any wine/wineserver/Majesty2.exe line of this prefix)."""
    d = probe_game_env_detailed()
    return d["effective_env"] if d else None


# ---------------------------------------------------------------- A/B alternation mode (M1)

_AB_ENV_TOKEN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=\S+$")
_AB_LABEL_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


def make_ab_script(out_path, arms, pairs=3, dwell_sec=20.0, speed=5.0, save="bench_heavy",
                   bench_py=None, python_bin=None, out_dir=None, max_timeout=420.0,
                   post_init_wait=150.0):
    """Generate a shell script that benchmarks arms in strict alternation A,B,A,B,...

    Like .hands/inbox/150_pather_ab.sh: one m2_bench.py stress invocation per run,
    per-run JSON + a snapshot of /tmp/m2_fps.log, a RESULT line per run. Alternation
    cancels slow drift (thermal/cache/filesystem state) that a block order A...A B...B
    would fold into the arm difference.

    arms: sequence of (label, env_str, note); env_str is a space-separated list of
    VAR=VALUE tokens (validated: no spaces inside tokens, safely word-split by `env $envs`).
    Returns (Path(out_path), n_runs).
    """
    if len(arms) < 2:
        raise ValueError("need at least two arms, e.g. [('A', 'M2_PATHER_SSE=1', 'candidate'), ...]")
    for label, env_str, _note in arms:
        if not _AB_LABEL_RE.match(label):
            raise ValueError(f"arm label must be a single token: {label!r}")
        for tok in (env_str or "").split():
            if not _AB_ENV_TOKEN_RE.match(tok):
                raise ValueError(f"env token {tok!r} of arm {label!r} is not VAR=VALUE")
    labels = [a[0] for a in arms]
    if len(set(labels)) != len(labels):
        raise ValueError(f"arm labels must be unique: {labels}")

    bench_py = str(bench_py or Path(__file__).resolve())
    python_bin = python_bin or "python3"
    out_dir = out_dir or f"/tmp/m2_ab_{time.strftime('%Y%m%d_%H%M%S')}"
    n_runs = len(arms) * pairs

    lines = [
        "#!/bin/sh",
        "# Generated by tools/m2_bench.py make_ab_script() -- A/B alternation benchmark (M1).",
        "# Arms:",
    ]
    for label, env_str, note in arms:
        lines.append(f"#   {label} = {env_str or '<no extra env>'}  # {note}")
    order = " ".join(f"{a[0]}{i}" for i in range(1, pairs + 1) for a in arms)
    lines += [
        f"# Order: {order} (strict alternation, {pairs} full passes, {n_runs} runs).",
        "# M1b: --ab-report judges this directory with interleaved=True (separation rule: win",
        "# needs every candidate > every control, gap >= 5%, gain >= max(8%, 3x pooled spread)).",
        "set -eu",
        f'OUT_DIR="{out_dir}"',
        f'BENCH_PY="{bench_py}"',
        f'PY_BIN="{python_bin}"',
        'mkdir -p "$OUT_DIR"',
        "run_one() {",
        '  label="$1"; envs="$2"',
        '  echo "== $label env: $envs started $(date +%H:%M:%S)"',
        "  # shellcheck disable=SC2086  # envs is a validated VAR=VAL list; intentional word split",
        f'  env $envs "$PY_BIN" "$BENCH_PY" --label "$label" --save {save} --dwell {dwell_sec:g} '
        f'--speed {speed:g} --post-init-wait {post_init_wait:g} --timeout {max_timeout:g} '
        '--out "$OUT_DIR/$label.json"',
        '  cp /tmp/m2_fps.log "$OUT_DIR/$label.fps.log" 2>/dev/null || true',
        '  echo "RESULT $label json=$OUT_DIR/$label.json"',
        "}",
    ]
    for i in range(1, pairs + 1):
        for label, env_str, _note in arms:
            lines.append(f"run_one {label}{i} '{env_str or ''}'")
    lines += [
        'echo "ALL DONE $(date +%H:%M:%S)"',
        'echo "report: $PY_BIN $BENCH_PY --ab-report $OUT_DIR"',
    ]
    p = Path(out_path)
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    p.chmod(0o755)
    return p, n_runs


def _last_flag_line(text, prefix):
    """Value of the LAST line starting with prefix (proxy rewrites '# features:'/'# pather_sse:')."""
    last = None
    for l in text.splitlines():
        if l.startswith(prefix):
            last = l[len(prefix):].strip()
    return last


def ab_report(runs, control_arm="B", candidate_arm="A", interleaved=True):
    """Aggregate an alternation series into one JSON-serialisable report.

    runs: list of {"label": "A1", "arm": "A", "json": {run JSON}, "log": "fps log text, optional"}.
    Per run: fps, 1%-low, quant_ms, max frame ms, frames, '# features:'/'# pather_sse:' flags
    (from the run log, falling back to the run JSON's proxy_log), env warnings.
    Attaches verdict(control, candidate) from tools/m2_closed_loop.py: the candidate arm is
    judged ONLY against the control arm measured in the same session -- never a stored baseline.

    interleaved=True (the default: runs produced by make_ab_script() alternate A,B,A,B,...
    inside one session) selects the M1b separation rule -- every candidate above every
    control, gap >= 5%, gain >= max(8%, 3x pooled within-group spread); see verdict().
    Pass interleaved=False for block-ordered data -> the conservative 20% rule.
    """
    rows = []
    for r in runs:
        j = r.get("json") or {}
        combat = j.get("combat") or {}
        phases = j.get("phases_combat") or {}
        log_text = r.get("log")
        if log_text is None:
            log_text = "\n".join(j.get("proxy_log") or [])
        rows.append({
            "label": r.get("label"),
            "arm": r.get("arm"),
            "status": j.get("status"),
            "fps": combat.get("avg_fps"),
            "one_pct_low": combat.get("one_pct_low_avg_fps", combat.get("one_pct_low_fps")),
            "quant_ms": phases.get("quant_ms"),
            "max_ms": combat.get("max_ms"),
            "frames": combat.get("frames"),
            "features": _last_flag_line(log_text, "# features:"),
            "pather_sse": _last_flag_line(log_text, "# pather_sse:"),
            "env_warnings": j.get("env_warnings") or [],
        })
    summary = {}
    for arm in dict.fromkeys(r.get("arm") for r in runs):
        fps = [x["fps"] for x in rows if x["arm"] == arm and isinstance(x["fps"], (int, float))]
        q = [x["quant_ms"] for x in rows if x["arm"] == arm and isinstance(x["quant_ms"], (int, float))]
        summary[arm] = {
            "runs": sum(1 for x in rows if x["arm"] == arm),
            "measured": len(fps),
            "mean_fps": round(sum(fps) / len(fps), 3) if fps else None,
            "mean_quant_ms": round(sum(q) / len(q), 2) if q else None,
        }
    report = {"control_arm": control_arm, "candidate_arm": candidate_arm,
              "interleaved": interleaved, "runs": rows, "summary": summary, "verdict": None}
    ctrl = [x["fps"] for x in rows if x["arm"] == control_arm and isinstance(x["fps"], (int, float))]
    cand = [x["fps"] for x in rows if x["arm"] == candidate_arm and isinstance(x["fps"], (int, float))]
    if ctrl and cand:
        try:
            from m2_closed_loop import verdict as _verdict
            report["verdict"] = _verdict(ctrl, cand, interleaved=interleaved)
        except ImportError:
            report["verdict_import_error"] = "tools/m2_closed_loop.py not importable"
    return report


def format_ab_table(report):
    """One markdown table for an ab_report() dict."""
    lines = [
        "| run | arm | fps | 1%-low | quant_ms | max_ms | frames | features | pather_sse |",
        "|-----|-----|-----|--------|----------|--------|--------|----------|------------|",
    ]
    for r in report["runs"]:
        def fmt(v, spec):
            return format(v, spec) if isinstance(v, (int, float)) else "-"
        lines.append(
            f"| {r['label']} | {r['arm']} | {fmt(r['fps'], '.2f')} | {fmt(r['one_pct_low'], '.2f')} "
            f"| {fmt(r['quant_ms'], '.1f')} | {fmt(r['max_ms'], '.1f')} | {r['frames'] if r['frames'] is not None else '-'} "
            f"| {r['features'] or '-'} | {r['pather_sse'] or '-'} |"
        )
    return "\n".join(lines)


def format_verdict_line(v):
    """One line for a verdict() dict: label, deciding rule, gain, separation gap, spreads.

    Printed by --ab-report (M1b): a report must always show the gap, the spread(s) and the
    rule that accepted the verdict -- 'interleaved_separation' or 'conservative'.
    """
    if not v:
        return "verdict=None (no verdict attached)"
    parts = [f"verdict={v.get('verdict')}", f"rule={v.get('rule')}"]
    if v.get("gain_pct") is not None:
        parts.append(f"gain={v['gain_pct']:+.1f}%")
    if v.get("separation_pct") is not None:
        parts.append(f"separation={v['separation_pct']:+.2f}%")
    if v.get("pooled_spread_pct") is not None:
        parts.append(f"pooled_spread={v['pooled_spread_pct']:.2f}%")
    if v.get("interleaved_gain_threshold_pct") is not None:
        parts.append(f"interleaved_threshold=max(8%, 3x pooled)={v['interleaved_gain_threshold_pct']:.1f}%")
    parts.append(f"control_spread={v.get('control_spread_pct')}%")
    parts.append(f"conservative_threshold=max(20%, 3x ctrl spread)={v.get('threshold_pct')}%")
    parts.append(f"n_control={v.get('n_control')} n_candidate={v.get('n_candidate')}")
    return " ".join(parts)


def load_ab_runs(out_dir):
    """Load <label>.json (+ optional <label>.fps.log) files written by a make_ab_script() run.

    The arm is the label with the trailing run number stripped (A1 -> A, B3 -> B).
    """
    out_dir = Path(out_dir)
    runs = []
    for jf in sorted(out_dir.glob("*.json")):
        if jf.stem in ("AB_REPORT",):
            continue
        try:
            j = json.loads(jf.read_text(encoding="utf-8", errors="ignore"))
        except Exception:
            continue
        if not isinstance(j, dict) or "combat" not in j:
            continue
        logf = jf.with_name(jf.stem + ".fps.log")
        runs.append({
            "label": jf.stem,
            "arm": re.sub(r"\d+$", "", jf.stem),
            "json": j,
            "log": logf.read_text(encoding="utf-8", errors="ignore") if logf.exists() else None,
        })
    return runs


def read_fps_log_lines():
    if not FPS_LOG.exists():
        return []
    return FPS_LOG.read_text(encoding="utf-8", errors="ignore").splitlines()


def telemetry_stats(lines):
    """Aggregate '# telemetry ...' lines written once per second by the d3d9 proxy."""
    rows = []
    for l in lines:
        if not l.startswith("# telemetry "):
            continue
        kv = dict(x.split("=", 1) for x in l[len("# telemetry "):].split() if "=" in x)
        try:
            rows.append({k: float(v) for k, v in kv.items()})
        except ValueError:
            pass
    if not rows:
        return {"seconds": 0}
    frames = sum(r.get("frames", 0) for r in rows) or 1

    def per_frame(key):
        return round(sum(r.get(key, 0) for r in rows) / frames, 1)

    return {
        "seconds": len(rows),
        "frames": int(frames),
        "draws_per_frame": per_frame("draws"),
        "prims_per_frame": per_frame("prims"),
        "up_draws_per_frame": per_frame("updraws"),
        "allocs_per_frame": per_frame("allocs"),
        "allocs_exe_per_frame": per_frame("a_exe"),
        "allocs_scene_per_frame": per_frame("a_scene"),
        "allocs_render_per_frame": per_frame("a_render"),
        "max_draws_per_frame_sec_avg": max(round(r.get("draws", 0) / max(r.get("frames", 1), 1), 1) for r in rows),
    }


def phase_stats(lines):
    """Aggregate '# phases ...' lines (ms per frame inside Present / draws / locks / state / misc / create + logic)."""
    rows = []
    for l in lines:
        if not l.startswith("# phases "):
            continue
        try:
            rows.append({k: float(v) for k, v in (x.split("=", 1) for x in l[len("# phases "):].split() if "=" in x)})
        except ValueError:
            pass
    if not rows:
        return {"seconds": 0}
    frames = sum(r["frames"] for r in rows) or 1
    out = {"seconds": len(rows), "frames": int(frames)}
    for k in ("frame_ms", "present_ms", "draw_ms", "lock_ms", "state_ms", "misc_ms", "create_ms", "logic_ms",
              "lock_calls", "state_calls", "misc_calls", "create_calls", "quant_ms", "quants_pf", "qarg_ms", "game_x"):
        out[k] = round(sum(r.get(k, 0) * r["frames"] for r in rows) / frames, 2)  # frame-weighted per-frame mean
    out["foreign_calls"] = int(sum(r.get("foreign", 0) for r in rows))
    out["speed_last"] = rows[-1].get("speed")
    out["speed_scale_last"] = rows[-1].get("speed_scale")
    return out


def game_pid():
    r = subprocess.run(["pgrep", "-x", "Majesty2.exe"], capture_output=True, text=True)
    pids = [int(p) for p in r.stdout.split()]
    return pids[0] if pids else None


def loaded_icd(pid):
    """Which Vulkan driver dylib the game process really mapped (lsof), independent of the env we passed."""
    try:
        out = subprocess.run(["lsof", "-p", str(pid)], capture_output=True, text=True, timeout=20).stdout
    except Exception as e:
        return f"lsof failed: {e}"
    libs = sorted({l.split()[-1] for l in out.splitlines() if re.search(r"kosmickrisp|MoltenVK|libvulkan", l, re.I)})
    return libs


def read_frametimes():
    import array
    a = array.array("I")
    if FT_BIN.exists():
        b = FT_BIN.read_bytes()
        a.frombytes(b[: len(b) // 4 * 4])
    return list(a)


def dll_overrides():
    """WINEDLLOVERRIDES for a stress run: our native proxy must win over the builtin d3d9.

    M2B_DLLOVERRIDES is an opt-in override (diagnostics: e.g. 'd3d9=b' to run wine's own
    D3D9 without the proxy); unset keeps the historical value.
    """
    return os.environ.get("M2B_DLLOVERRIDES", "d3d9=n,b")


def stress_launch_spec():
    """How run_stress() starts the game.

    Default (M2B_LAUNCH_CMD unset): the Sikarugir app-bundle launcher, exactly as before.
    With M2B_LAUNCH_CMD set (the X7 wine-build arm): the command is shlex-split and run
    directly with cwd = M2B_LAUNCH_CWD (default: GAME_DIR); its console output is appended
    to M2B_CHILD_LOG when that is set (that file is where the x87sidecar's X87_LOG_THROUGHPUT
    "sidecar: N req/s (total M)" lines land, i.e. the HOOK_REQUESTS counter).
    Returns (argv, cwd, child_log_path or None).
    """
    cmd = os.environ.get("M2B_LAUNCH_CMD")
    if not cmd:
        return [str(LAUNCHER)], None, None
    argv = shlex.split(cmd)
    if not argv:
        raise ValueError("M2B_LAUNCH_CMD is set but splits to an empty argv")
    cwd = os.environ.get("M2B_LAUNCH_CWD") or str(GAME_DIR)
    return argv, cwd, os.environ.get("M2B_CHILD_LOG") or None


def run_stress(save="bench_heavy", dwell_sec=25.0, post_init_wait=55.0, max_timeout=240.0,
               out_json="/tmp/m2_stress_latest.json", label="stress", commands=None, speed=5.0,
               sample_out=SAMPLE_OUT, sample_delay=3.0, sample_sec=5):
    """Combat stress test: the d3d9 proxy runs `commands` (default: m2LoadGame save) after Application::Init(),
    then samples CPU/RAM + /usr/bin/sample stack profile for dwell_sec."""
    if not commands:
        if save.lower() in ("single_missions/pp03:pp03", "single_missions/pp03"):
            commands = ["LEVEL:Single_missions/PP03:pp03"]
        elif ":" in save:
            commands = [f"LEVEL:{save}"]
        elif save.lower().startswith("single_missions/"):
            m = save.split("/")[-1]
            commands = [f"LEVEL:{save}:{m}"]
        else:
            # a saves folder (profiles/0/saves[/campaigns]/<save>): load-game dialog -> evLoadSelectedGame
            commands = [f"SAVE:{save}"]
        if speed and speed > 0:
            commands.append(f"SPEED:{speed:g}")
    print(f"[m2_stress] commands={commands} dwell={dwell_sec}s post-init wait={post_init_wait}s timeout={max_timeout}s")
    clean_processes_and_locks()
    ensure_windowed_mode()
    startup_sha_before = sha256_of(STARTUP_FILE)
    for f in (DEBUG_LOG, ERROR_LOG, FPS_LOG, FT_BIN, X86_PROFILE, X86_SAMPLES):
        if f.exists():
            f.unlink()
    env = game_env()
    # Load syswow64\d3d9.dll (our proxy) as native; without this Wine uses the builtin d3d9 and the proxy never runs.
    env["WINEDLLOVERRIDES"] = dll_overrides()
    argv, cwd, child_log = stress_launch_spec()

    res = {"label": label, "save": save, "bench_opts": dict(BENCH_OPTS), "load": {}, "combat": {}, "status": "INCOMPLETE"}
    cpu_samples, entity_lines = [], []
    t0 = time.monotonic()
    t_init = t_loaded = t_measure = None
    ft_base = 0
    tel_base = 0
    created = []
    sampler = None
    child_fh = None
    try:
        STARTUP_FILE.parent.mkdir(parents=True, exist_ok=True)
        AUTOSTART_FILE.write_text("\n".join(commands) + "\n", encoding="utf-8")
        created.append(AUTOSTART_FILE)
        if child_log:
            # X7 wine-build arm: keep the child's console output (x87sidecar throughput lines).
            child_fh = open(child_log, "ab", buffering=0)
            subprocess.Popen(argv, env=env, cwd=cwd, stdout=child_fh, stderr=subprocess.STDOUT)
        else:
            subprocess.Popen(argv, env=env, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        while True:
            now = time.monotonic()
            el = now - t0
            if el >= max_timeout:
                print(f"  [+{el:6.1f}s] TIMEOUT")
                break
            content = DEBUG_LOG.read_text(encoding="utf-8", errors="ignore") if DEBUG_LOG.exists() else ""
            if t_init is None and re.search(r"Application::Init\(\) started in", content):
                t_init = now
                res["load"]["app_init_wall_sec"] = round(el, 2)
                print(f"  [+{el:6.1f}s] Application::Init done -> proxy runs autostart commands {commands}")
            m = re.search(r"LEVEL LOADED in ([0-9.]+)", content)
            m2 = re.search(r"GameStateLevel::SecondUpdate started in", content)
            if t_loaded is None and ((m and m2) or (t_init and now - t_init >= post_init_wait)):
                t_loaded = now
                res["load"]["level_loaded"] = bool(m)
                res["load"]["level_load_engine_sec"] = float(m.group(1)) if m else None
                res["load"]["level_loaded_wall_sec"] = round(el, 2)
                print(f"  [+{el:6.1f}s] level loaded={bool(m)} ({m.group(1) + 's' if m else 'fallback wait'}); 3s warm-up then {dwell_sec}s combat dwell")
            if t_loaded is not None:
                if t_measure is None and (now - t_loaded >= 3.0):
                    t_measure = now
                    ft_base = len(read_frametimes())
                    tel_base = len(read_fps_log_lines())
                    env_probe = probe_game_env_detailed()
                    res["effective_env"] = env_probe["effective_env"] if env_probe else None
                    res["env_source"] = env_probe["env_source"] if env_probe else None
                    res["env_warnings"] = env_probe["env_warnings"] if env_probe else None
                    for w in res["env_warnings"] or []:
                        print(f"  [env-warning] {w}")
                    print(f"  [+{el:6.1f}s] Starting clean {dwell_sec}s combat FPS window (ft_base={ft_base})")
                    pid = game_pid()
                    res["game_pid"] = pid
                    res["loaded_vulkan_icd"] = loaded_icd(pid) if pid else None
                    print(f"  [+{el:6.1f}s] Majesty2.exe pid={pid} vulkan libs={res['loaded_vulkan_icd']}")
                if (t_measure is not None and sampler is None and sample_out and now - t_measure >= sample_delay
                        and res.get("game_pid")):
                    sampler = subprocess.Popen(["/usr/bin/sample", str(res["game_pid"]), str(sample_sec), "-file", sample_out],
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    res["sample"] = {"file": sample_out, "start_in_window_sec": round(now - t_measure, 1),
                                     "sec": sample_sec}
                    print(f"  [+{el:6.1f}s] /usr/bin/sample {res['game_pid']} {sample_sec} -> {sample_out}")
                if t_measure is not None:
                    sm = sample_process_cpu()
                    if sm["procs"]:
                        cpu_samples.append(sm)
                    if now - t_measure >= dwell_sec and (sampler is None or sampler.poll() is not None):
                        break
            time.sleep(0.5)
        content = DEBUG_LOG.read_text(encoding="utf-8", errors="ignore") if DEBUG_LOG.exists() else ""
        entity_lines = [l.strip() for l in content.splitlines()
                        if re.search(r"entit|Save .* not found|m2LoadGame|statistic|LOADING LEVEL|LEVEL LOADED", l, re.I)][-25:]
        res["combat"] = frame_stats(read_frametimes()[ft_base:]) if t_measure else {"frames": 0}
        res["whole_run"] = frame_stats(read_frametimes())
        fps_lines = read_fps_log_lines()
        res["telemetry_combat"] = telemetry_stats(fps_lines[tel_base:]) if t_measure else {"seconds": 0}
        res["phases_combat"] = phase_stats(fps_lines[tel_base:]) if t_measure else {"seconds": 0}
        res["proxy_log"] = [l for l in fps_lines if l.startswith("# ") and not l.startswith(("# telemetry ", "# phases "))][:80]
        speed_lines = [l for l in fps_lines if "game speed" in l]
        res["game_speed_log"] = speed_lines[-5:]
        res["x86_profile"] = str(X86_PROFILE) if X86_PROFILE.exists() else None
    finally:
        if sampler is not None and sampler.poll() is None:
            try:
                sampler.wait(timeout=30)
            except subprocess.TimeoutExpired:
                sampler.kill()
        if child_fh is not None:
            try:
                child_fh.close()
            except Exception:
                pass
        for f in created:
            try:
                f.unlink()
            except FileNotFoundError:
                pass
        clean_processes_and_locks()
    res["cpu_ram"] = {
        "avg_cpu_pct": round(sum(x["cpu_pct"] for x in cpu_samples) / len(cpu_samples), 1) if cpu_samples else 0,
        "peak_rss_mb": max((x["rss_mb"] for x in cpu_samples), default=0),
        "samples": len(cpu_samples),
    }
    res["log_entity_lines"] = entity_lines
    res["errors_log_bytes"] = ERROR_LOG.stat().st_size if ERROR_LOG.exists() else 0
    # The bench must never touch resource.mod m2Startup.con: verify it byte-for-byte.
    res["startup_con_preserved"] = sha256_of(STARTUP_FILE) == startup_sha_before
    res["status"] = "PASS" if (res["load"].get("level_loaded") and res["combat"].get("frames", 0) > 0) else "INCOMPLETE"
    Path(out_json).write_text(json.dumps(res, indent=2), encoding="utf-8")
    print(json.dumps(res, indent=2, ensure_ascii=False))
    return res


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Majesty 2 Event-Driven Benchmark")
    parser.add_argument("--label", type=str, default="baseline", help="Benchmark run label")
    parser.add_argument("--dwell", type=float, default=15.0, help="Seconds to stay in 3D Main Menu AFTER Application::Init()")
    parser.add_argument("--timeout", type=float, default=120.0, help="Hard safety timeout in seconds")
    parser.add_argument("--out", type=str, default="/tmp/m2_bench_latest.json", help="Output JSON path")
    parser.add_argument("--save", type=str, default=None, help="Combat stress mode: save name auto-loaded via m2LoadGame (e.g. bench_heavy)")
    parser.add_argument("--cmd", action="append", default=None,
                        help="Combat stress mode: console command run by the proxy after init, repeatable "
                             "(e.g. --cmd 'm2LoadLevel single_missions/pp03:pp03'). Overrides --save.")
    parser.add_argument("--speed", type=float, default=5.0, help="Stress mode: game speed the proxy enforces after load (0 = off)")
    parser.add_argument("--post-init-wait", type=float, default=150.0, help="Stress mode: fallback seconds after Application::Init()")
    parser.add_argument("--hud", action="store_true", help="Enable MTL/DXVK HUDs (off by default for clean measurements)")
    parser.add_argument("--icd", choices=sorted(ICD_FILES), default=None,
                        help="Force Vulkan ICD via VK_DRIVER_FILES (moltenvk also sets MVK_CONFIG_* tuning)")
    parser.add_argument("--clear-mt", action="store_true", help="Proxy clears D3DCREATE_MULTITHREADED in CreateDevice")
    parser.add_argument("--sample-out", type=str, default=SAMPLE_OUT,
                        help="Stress mode: /usr/bin/sample output taken inside the combat window ('' = off)")
    parser.add_argument("--make-ab-script", type=str, default=None, metavar="PATH",
                        help="Generate an A/B alternation shell script (A,B,A,B,...) and exit; "
                             "arms come from --ab-arm-a/--ab-arm-b, count from --ab-pairs")
    parser.add_argument("--ab-arm-a", type=str, default="M2_PATHER_SSE=1",
                        help="A/B mode: space-separated VAR=VALUE env for arm A (candidate), default %(default)s")
    parser.add_argument("--ab-arm-b", type=str, default="",
                        help="A/B mode: space-separated VAR=VALUE env for arm B (control), default none")
    parser.add_argument("--ab-pairs", type=int, default=3,
                        help="A/B mode: number of full A,B passes in the generated script (default %(default)s)")
    parser.add_argument("--ab-note-a", type=str, default="candidate", help="A/B mode: arm A description")
    parser.add_argument("--ab-note-b", type=str, default="control", help="A/B mode: arm B description")
    parser.add_argument("--ab-report", type=str, default=None, metavar="DIR",
                        help="A/B mode: build AB_REPORT.json + one markdown table from a directory of "
                             "<label>.json[.fps.log] runs (as written by the generated script) and exit")
    args = parser.parse_args()
    BENCH_OPTS.update(hud=args.hud, icd=args.icd, clear_mt=args.clear_mt)
    if args.make_ab_script:
        path, n_runs = make_ab_script(
            args.make_ab_script,
            arms=[("A", args.ab_arm_a, args.ab_note_a), ("B", args.ab_arm_b, args.ab_note_b)],
            pairs=args.ab_pairs, dwell_sec=args.dwell, speed=args.speed,
            save=args.save or "bench_heavy")
        print(f"wrote {path} ({n_runs} runs, order " +
              " ".join(f"{a}{i}" for i in range(1, args.ab_pairs + 1) for a in "AB") + ")")
        raise SystemExit(0)
    if args.ab_report:
        out_dir = Path(args.ab_report)
        rep = ab_report(load_ab_runs(out_dir))
        (out_dir / "AB_REPORT.json").write_text(json.dumps(rep, indent=2, ensure_ascii=False),
                                                encoding="utf-8")
        print(format_ab_table(rep))
        v = rep.get("verdict") or {}
        print("\n" + format_verdict_line(v))
        if v.get("reason"):
            print(f"reason: {v['reason']}")
        raise SystemExit(0)
    if args.save or args.cmd:
        run_stress(save=args.save or "bench_heavy", dwell_sec=args.dwell, post_init_wait=args.post_init_wait,
                   max_timeout=max(args.timeout, 300.0), label=args.label, commands=args.cmd, speed=args.speed,
                   sample_out=args.sample_out,
                   out_json=args.out if args.out != "/tmp/m2_bench_latest.json" else "/tmp/m2_stress_latest.json")
        raise SystemExit(0)
    run_benchmark(label=args.label, dwell_sec=args.dwell, max_timeout=args.timeout, out_json=args.out)
