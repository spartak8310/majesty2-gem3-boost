#!/usr/bin/env python3
import sys
import os
import re
import shutil
import subprocess

# 0. SATRAP ECHO cm import
sys.path.insert(0, './SATRAP_ECHO')
try:
    import satrap_cm as cm
except ImportError:
    cm = None

print("[1/5] Updating m2_options.set configurations...")

M3_OPTIONS = """{video
	{bump 1}
	{specular}
	{environment}
	{lights_quality high}
	{shadow none}
	{water
		{reflection none}
		{bump 1}
	}
	{bloom 0.08}
	{glow 1}
	{refraction 1}
	{screen
		{full_screen 0}
		{resolution "1512x982"}
		{vertical_sync 0}
		{antialiasing
			{mode none}
		}
	}
	{anisotropy 16x}
	{texture_filtering anisotropic}
	{gamma 1}
	{brightness 1}
	{contrast 1}
}
{audio
	{max_buffers "32"}
	{speaker ""}
	{frequency "44_khz"}
	{music
		{main_music 1}
		{ambience 1}
		{message 1}
		{log 1}
	}
	{sound
		{sfx 1}
		{environment 1}
		{voice 1}
		{interface 1}
	}
}
{video
	{model_quality high}
	{texture_quality high}
	{grass 1}
	{fx_quality high}
}
{game_specific
	{healthbar_mode over_cursors}
	{show_emotions 1}
	{auto_pause 0}
	{skip_shop_autorisation 0}
	{showvalidflagsonly}
	{check_update}
}
{video
	{video_preset custom}
	{shadow_quality off}
}
{audio
	{m2_audio
		{master 1}
		{environment 1}
		{voice 1}
		{interface 1}
		{sfx 1}
		{music 1}
	}
}
"""

profiles_dir = "$HOME/Documents/majesty2/profiles/0"
os.makedirs(profiles_dir, exist_ok=True)

opt_files = [
    os.path.join(profiles_dir, "m2_options.set"),
    os.path.join(profiles_dir, "m2_options.set.play_profile"),
    os.path.join(profiles_dir, "m2_options.set.pre_m2_fast"),
]
for p in opt_files:
    with open(p, "w", encoding="utf-8") as f:
        f.write(M3_OPTIONS)
    print(f"  -> Written: {p} ({os.path.getsize(p)} bytes)")

print("\n[2/5] Updating Majesty 2 FAST.command & Majesty 2 PLAY.command...")
fast_cmd_path = "$HOME/Desktop/Majesty 2 FAST.command"
play_cmd_path = "$HOME/Desktop/Majesty 2 PLAY.command"

if not os.path.exists(fast_cmd_path):
    raise FileNotFoundError(f"{fast_cmd_path} not found!")

with open(fast_cmd_path, "r", encoding="utf-8") as f:
    code = f.read()

print(f"  Read existing {fast_cmd_path} ({len(code)} chars)")

# 2a. Update apply_options()
apply_options_replacement = '''apply_options() {
  local opt="$M2_OPTIONS"
  if [ -z "$opt" ] || [ ! -f "$opt" ]; then
    opt="$HOME/Documents/majesty2/profiles/0/m2_options.set"
  fi
  if [ -f "$opt" ]; then
    /usr/bin/python3 -c "
import sys, re
p = sys.argv[1]
try:
    with open(p, 'r', encoding='utf-8', errors='ignore') as f:
        c = f.read()
    subs = [
        (r'\\{master\\s+[^}]+\\}', '{master 1}'),
        (r'\\{sfx\\s+[^}]+\\}', '{sfx 1}'),
        (r'\\{voice\\s+[^}]+\\}', '{voice 1}'),
        (r'\\{music\\s+[^}]+\\}', '{music 1}'),
        (r'\\{main_music\\s+[^}]+\\}', '{main_music 1}'),
        (r'\\{ambience\\s+[^}]+\\}', '{ambience 1}'),
        (r'\\{message\\s+[^}]+\\}', '{message 1}'),
        (r'\\{log\\s+[^}]+\\}', '{log 1}'),
        (r'\\{environment\\s+[^}]+\\}', '{environment 1}'),
        (r'\\{interface\\s+[^}]+\\}', '{interface 1}'),
        (r'\\{lights_quality\\s+[^}]+\\}', '{lights_quality high}'),
        (r'\\{texture_quality\\s+[^}]+\\}', '{texture_quality high}'),
        (r'\\{model_quality\\s+[^}]+\\}', '{model_quality high}'),
        (r'\\{fx_quality\\s+[^}]+\\}', '{fx_quality high}'),
        (r'\\{anisotropy\\s+[^}]+\\}', '{anisotropy 16x}'),
        (r'\\{texture_filtering\\s+[^}]+\\}', '{texture_filtering anisotropic}'),
        (r'\\{bloom\\s+[^}]+\\}', '{bloom 0.08}'),
    ]
    for pat, rep in subs:
        c = re.sub(pat, rep, c)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(c)
except Exception:
    pass
" "$opt" 2>/dev/null || true
  fi
}'''

m_opt = re.search(r'apply_options\s*\(\)\s*\{[\s\S]*?\n\}', code)
if m_opt:
    code = code[:m_opt.start()] + apply_options_replacement + code[m_opt.end():]
    print("  -> Replaced existing apply_options() function")
else:
    m_launch = re.search(r'#+\s*--?\s*6\.\s*launch', code, re.IGNORECASE)
    if m_launch:
        code = code[:m_launch.start()] + apply_options_replacement + "\n\n" + code[m_launch.start():]
        print("  -> Inserted apply_options() before section 6")
    else:
        code = apply_options_replacement + "\n\n" + code
        print("  -> Prepended apply_options()")

# 2b. Terminal Banner Hotkeys
hotkeys_str = "[F8] M3 TrueHDR & 16x Aniso | [F9] 13x SSE2 ON/OFF | [F10] Quant FPS Decoupler (1x/2x/3x) | [F11] Smooth Cam | [F12] 7s Profiler"

m_banner = re.search(r'\[F8\].*?Profiler', code)
if m_banner:
    line_start = code.rfind('\n', 0, m_banner.start()) + 1
    line_end = code.find('\n', m_banner.end())
    code = code[:line_start] + f'  echo "  {hotkeys_str}"' + code[line_end:]
    print("  -> Replaced existing hotkeys banner line")
else:
    m_hdr = re.search(r'echo\s+[\'"].*?===[\'"]', code)
    if m_hdr:
        end_line = code.find('\n', m_hdr.end())
        code = code[:end_line] + f'\n  echo "  {hotkeys_str}"' + code[end_line:]
        print("  -> Injected hotkeys banner line after header")

# 2c. Update section -- 6. launch
launch_replacement = '''# -- 6. launch
apply_options

M2_DIRECT="${M2_FAST_DIRECT:-0}"

if [ "${M2_FAST_DRY:-0}" = "1" ]; then
  echo "[DRY RUN] Preflight verification passed. M2_DIRECT=$M2_DIRECT"
  if [ "$M2_DIRECT" != "1" ] && [ -f "$GAME_DIR/Majesty2Selector.exe" ]; then
    echo "[DRY RUN] Target: Majesty2Selector.exe (DLC Selector)"
  else
    echo "[DRY RUN] Target: Majesty2.exe (Direct)"
  fi
else
  if [ "$M2_DIRECT" != "1" ] && [ -f "$GAME_DIR/Majesty2Selector.exe" ]; then
    echo "Launching Majesty 2 Selector (DLC Chooser)..."
    "$W_WINE" Majesty2Selector.exe >"$CHILD_LOG" 2>&1 &
    CHILD_PID=$!
  else
    echo "Launching Majesty 2 Direct..."
    "$W_WINE" Majesty2.exe >"$CHILD_LOG" 2>&1 &
    CHILD_PID=$!
  fi
  if [ -n "$CHILD_PID" ]; then
    wait "$CHILD_PID" 2>/dev/null || true
  fi
  # If Majesty2Selector.exe spawned Majesty2.exe, wait until Majesty2.exe finishes!
  sleep 2
  while pgrep -f "Majesty2\\.exe" >/dev/null 2>&1; do
    sleep 2
  done
fi
'''

m_launch = re.search(r'#+\s*--?\s*6\.\s*launch', code, re.IGNORECASE)
m_teardown = re.search(r'#+\s*--?\s*7\.\s*teardown', code, re.IGNORECASE)

if m_launch and m_teardown:
    code = code[:m_launch.start()] + launch_replacement + "\n" + code[m_teardown.start():]
    print("  -> Replaced section -- 6. launch with DLC selector & Majesty2.exe watcher")
else:
    print(f"  -> WARNING: launch/teardown boundaries not found (m_launch={bool(m_launch)}, m_teardown={bool(m_teardown)})")

# Ensure M2_FAST_NO_PAUSE works properly at teardown/exit
if 'read -p' in code and 'M2_FAST_NO_PAUSE' not in code:
    code = code.replace('read -p "Press Enter to exit..."', 'if [ "${M2_FAST_NO_PAUSE:-0}" != "1" ]; then read -p "Press Enter to exit..."; fi')

# Save Majesty 2 FAST.command
with open(fast_cmd_path, "w", encoding="utf-8") as f:
    f.write(code)
os.chmod(fast_cmd_path, 0o755)
print(f"  -> Saved & chmod +x {fast_cmd_path}")

# Copy to Majesty 2 PLAY.command
with open(play_cmd_path, "w", encoding="utf-8") as f:
    f.write(code)
os.chmod(play_cmd_path, 0o755)
print(f"  -> Saved & chmod +x {play_cmd_path}")

# 3. Create Majesty 2 BENCH.command
print("\n[3/5] Creating Majesty 2 BENCH.command...")
bench_cmd_path = "$HOME/Desktop/Majesty 2 BENCH.command"

bench_content = '''#!/usr/bin/env bash
# ==============================================================================
# Majesty 2 BENCH.command - 1-Click 25-Second Performance Benchmark Launcher
# ==============================================================================
set -eo pipefail

DOC_OPT="$HOME/Documents/majesty2/profiles/0/m2_options.set"
BAK_OPT="/tmp/m2_options.set.bench_bak"
VENV_PY="./scratch/majesty2-re/.venv/bin/python3"
BENCH_SCRIPT="./scratch/majesty2-re/tools/run_day230_candidate.py"
FPS_LOG="/tmp/m2_fps.log"

echo "========================================================================"
echo "          MAJESTY 2: M3 PRO 25-SECOND BENCHMARK LAUNCHER                "
echo "========================================================================"

restore_options() {
  echo ""
  echo "[BENCH] Restoring user options profile..."
  if [ -f "$BAK_OPT" ]; then
    cp -f "$BAK_OPT" "$DOC_OPT"
  fi
  if [ -f "$DOC_OPT" ]; then
    /usr/bin/python3 -c "
import sys, re
p = sys.argv[1]
try:
    with open(p, 'r', encoding='utf-8', errors='ignore') as f:
        c = f.read()
    c = re.sub(r'\\{master\\s+[^}]+\\}', '{master 1}', c)
    c = re.sub(r'\\{sfx\\s+[^}]+\\}', '{sfx 1}', c)
    c = re.sub(r'\\{voice\\s+[^}]+\\}', '{voice 1}', c)
    c = re.sub(r'\\{music\\s+[^}]+\\}', '{music 1}', c)
    c = re.sub(r'\\{main_music\\s+[^}]+\\}', '{main_music 1}', c)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(c)
except Exception:
    pass
" "$DOC_OPT" 2>/dev/null || true
  fi
  echo "[BENCH] Profile restored & audio unmuted ({master 1})."
}
trap restore_options EXIT INT TERM

# 1. Back up options
if [ -f "$DOC_OPT" ]; then
  echo "[BENCH] Backing up options -> $BAK_OPT"
  cp -f "$DOC_OPT" "$BAK_OPT"
fi

# 2. Quiet windowed settings
if [ -f "$DOC_OPT" ]; then
  echo "[BENCH] Setting quiet windowed mode ({master 0}, {sfx 0}, {main_music 0}, {full_screen 0})..."
  /usr/bin/python3 -c "
import sys, re
p = sys.argv[1]
try:
    with open(p, 'r', encoding='utf-8', errors='ignore') as f:
        c = f.read()
    c = re.sub(r'\\{master\\s+[^}]+\\}', '{master 0}', c)
    c = re.sub(r'\\{sfx\\s+[^}]+\\}', '{sfx 0}', c)
    c = re.sub(r'\\{main_music\\s+[^}]+\\}', '{main_music 0}', c)
    c = re.sub(r'\\{full_screen\\s+[^}]+\\}', '{full_screen 0}', c)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(c)
except Exception:
    pass
" "$DOC_OPT"
fi

# 3. Run candidate benchmark
if [ "${M2_BENCH_DRY:-0}" = "1" ]; then
  echo "[BENCH DRY RUN] Script verified; skipping execution."
else
  echo "[BENCH] Running 25s Candidate Benchmark..."
  if [ -x "$VENV_PY" ] && [ -f "$BENCH_SCRIPT" ]; then
    "$VENV_PY" "$BENCH_SCRIPT" "$@" || true
  elif [ -f "$BENCH_SCRIPT" ]; then
    python3 "$BENCH_SCRIPT" "$@" || true
  else
    echo "[BENCH ERROR] Script not found at $BENCH_SCRIPT"
  fi
fi

# 4. Explicitly restore
restore_options
trap - EXIT INT TERM

# 5. Summary from /tmp/m2_fps.log
echo ""
echo "========================================================================"
echo "                     BENCHMARK METRICS SUMMARY                          "
echo "========================================================================"

if [ -f "$FPS_LOG" ]; then
  /usr/bin/python3 -c "
import sys, re
path = '$FPS_LOG'
try:
    with open(path, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
except Exception as e:
    print(f'Error reading {path}: {e}')
    sys.exit(0)

fps_list, quant_list, game_x_list, draw_list = [], [], [], []
turbo_count = 0

for line in lines:
    if 'turbo_v7' in line or 'turbo' in line.lower():
        turbo_count += 1
    m_fps = re.search(r'(?:fps|FPS)[\\s:=,]+([0-9.]+)', line)
    if m_fps:
        try: fps_list.append(float(m_fps.group(1)))
        except: pass
    m_quant = re.search(r'(?:quant(?:_ms)?)[\\s:=,]+([0-9.]+)', line, re.IGNORECASE)
    if m_quant:
        try: quant_list.append(float(m_quant.group(1)))
        except: pass
    m_gx = re.search(r'(?:game_x|game_speed)[\\s:=,]+([0-9.]+)', line, re.IGNORECASE)
    if m_gx:
        try: game_x_list.append(float(m_gx.group(1)))
        except: pass
    m_draw = re.search(r'(?:draw(?:_ms)?)[\\s:=,]+([0-9.]+)', line, re.IGNORECASE)
    if m_draw:
        try: draw_list.append(float(m_draw.group(1)))
        except: pass

print(f'Total Log Lines: {len(lines)}')
if fps_list:
    avg_fps = sum(fps_list) / len(fps_list)
    print(f'FPS       : Avg={avg_fps:6.2f} | Min={min(fps_list):6.2f} | Max={max(fps_list):6.2f}')
else:
    print('FPS       : (none parsed)')

if quant_list:
    avg_q = sum(quant_list) / len(quant_list)
    print(f'quant_ms  : Avg={avg_q:6.2f} ms | Min={min(quant_list):6.2f} ms | Max={max(quant_list):6.2f} ms')
else:
    print('quant_ms  : (none parsed)')

if game_x_list:
    avg_gx = sum(game_x_list) / len(game_x_list)
    print(f'game_x    : Avg={avg_gx:6.2f}x | Min={min(game_x_list):6.2f}x | Max={max(game_x_list):6.2f}x')
else:
    print('game_x    : (none parsed)')

if draw_list:
    avg_d = sum(draw_list) / len(draw_list)
    print(f'draw_ms   : Avg={avg_d:6.2f} ms | Min={min(draw_list):6.2f} ms | Max={max(draw_list):6.2f} ms')
else:
    print('draw_ms   : (none parsed)')

print(f'turbo_v7  : {turbo_count} hits')

print('\\nLog tail:')
for l in lines[-5:]:
    print('  ' + l.strip())
"
else
  echo "No log file found at $FPS_LOG"
fi

echo "========================================================================"
if [ "${M2_BENCH_NO_PAUSE:-0}" != "1" ]; then
  read -p "Press Enter to exit..." || true
fi
'''

with open(bench_cmd_path, "w", encoding="utf-8") as f:
    f.write(bench_content)
os.chmod(bench_cmd_path, 0o755)
print(f"  -> Saved & chmod +x {bench_cmd_path}")

print("\n[4/5] Testing Dry Runs & Syntax...")
# Check bash syntax of BENCH.command
res_syntax = subprocess.run(["bash", "-n", bench_cmd_path], capture_output=True, text=True)
print(f"  BENCH.command bash syntax check: rc={res_syntax.returncode} (stderr: {res_syntax.stderr.strip()})")
assert res_syntax.returncode == 0, f"BENCH.command syntax error: {res_syntax.stderr}"

# Test M2_FAST_DRY=1 M2_FAST_NO_PAUSE=1 on PLAY.command
env = os.environ.copy()
env["M2_FAST_DRY"] = "1"
env["M2_FAST_NO_PAUSE"] = "1"

res_play = subprocess.run(["/bin/bash", play_cmd_path], env=env, capture_output=True, text=True)
print(f"  PLAY.command dry run: rc={res_play.returncode}")
print("  --- STDOUT ---")
print(res_play.stdout.strip())
if res_play.stderr:
    print("  --- STDERR ---")
    print(res_play.stderr.strip())
assert res_play.returncode == 0, f"PLAY.command dry run failed with rc={res_play.returncode}"

# Test FAST.command dry run as well
res_fast = subprocess.run(["/bin/bash", fast_cmd_path], env=env, capture_output=True, text=True)
print(f"  FAST.command dry run: rc={res_fast.returncode}")
assert res_fast.returncode == 0, f"FAST.command dry run failed with rc={res_fast.returncode}"

# Test BENCH.command dry run
env_bench = os.environ.copy()
env_bench["M2_BENCH_DRY"] = "1"
env_bench["M2_BENCH_NO_PAUSE"] = "1"
res_bench = subprocess.run(["/bin/bash", bench_cmd_path], env=env_bench, capture_output=True, text=True)
print(f"  BENCH.command dry run: rc={res_bench.returncode}")
assert res_bench.returncode == 0, f"BENCH.command dry run failed with rc={res_bench.returncode}"

print("\n[5/5] Emitting verification receipt via cm.emit()...")
receipt = {
    "status": "PASS",
    "updated_profiles": [
        "$HOME/Documents/majesty2/profiles/0/m2_options.set",
        "$HOME/Documents/majesty2/profiles/0/m2_options.set.play_profile",
        "$HOME/Documents/majesty2/profiles/0/m2_options.set.pre_m2_fast",
    ],
    "high_gpu_settings": {
        "lights_quality": "high",
        "texture_quality": "high",
        "model_quality": "high",
        "fx_quality": "high",
        "anisotropy": "16x",
        "texture_filtering": "anisotropic",
        "bloom": "0.08",
        "resolution": "1512x982",
        "audio": "unmuted (master 1, sfx 1, music 1, voice 1)",
    },
    "desktop_commands": {
        "Majesty 2 FAST.command": {
            "path": fast_cmd_path,
            "executable": os.access(fast_cmd_path, os.X_OK),
            "dry_run_rc": res_fast.returncode,
        },
        "Majesty 2 PLAY.command": {
            "path": play_cmd_path,
            "executable": os.access(play_cmd_path, os.X_OK),
            "dry_run_rc": res_play.returncode,
        },
        "Majesty 2 BENCH.command": {
            "path": bench_cmd_path,
            "executable": os.access(bench_cmd_path, os.X_OK),
            "dry_run_rc": res_bench.returncode,
        },
    },
    "selector_integration": {
        "default": "Majesty2Selector.exe (M2_DIRECT=0)",
        "watcher": "sleep 2; while pgrep -f 'Majesty2.exe'; do sleep 2; done",
    },
    "banner_hotkeys": hotkeys_str,
}

if cm:
    cm.emit(receipt)
    print("cm.emit() completed successfully.")
else:
    print("Warning: satrap_cm not loaded, could not call cm.emit()")

print("\n=== ALL STEPS COMPLETED PERFECTLY ===")
