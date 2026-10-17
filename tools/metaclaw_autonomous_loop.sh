#!/usr/bin/env bash
[ "${M2_ALLOW_LEGACY_LOOP:-0}" = "1" ] || { echo "DEPRECATED: this loop compiles straight into the live game install with no restore. Use tools/m2_closed_loop.py (v4: staging + golden restore). Override: M2_ALLOW_LEGACY_LOOP=1" >&2; exit 1; }
set -uo pipefail

REPO="./scratch/majesty2-re"
SYSWOW="/Applications/Gamez/Majesty 2.app/Contents/SharedSupport/prefix/drive_c/windows/syswow64"
LOG="/tmp/metaclaw_m2_autonomous.log"
BEST_JSON="/tmp/m2_metaclaw_best.json"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] MetaClaw Autonomous M2 Optimizer started (PID $$)" | tee -a "$LOG"

# 1. Compile current Wave 7 d3d9_proxy.cpp on Clore #84966 and run baseline benchmark
$HOME/.local/bin/cc-clore-exec \
  --job m2-metaclaw-build \
  --sync-in "$REPO/src/d3d9_proxy.cpp" \
  --sync-out d3d9.dll:"$SYSWOW/d3d9.dll" \
  "i686-w64-mingw32-g++ -shared -O3 -msse2 -mfpmath=sse -s -static-libgcc -static-libstdc++ -Wl,--kill-at d3d9_proxy.cpp -o d3d9.dll -lgdi32 -luser32" >> "$LOG" 2>&1

cp "$REPO/src/d3d9_proxy.cpp" "$REPO/src/d3d9_proxy.cpp.best"
cp "$SYSWOW/d3d9.dll" "$SYSWOW/d3d9.dll.best"

M2_PROFILE=1 "$REPO/.venv/bin/python3" "$REPO/tools/m2_bench.py" \
  --label wave7_5x --save bench_heavy --dwell 20 --speed 5 --out "$BEST_JSON" >> "$LOG" 2>&1 || true

for WAVE in 8 9 10 11 12 13 14 15; do
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] === Starting Autonomous MetaClaw Wave $WAVE ===" | tee -a "$LOG"
  $HOME/.local/bin/cc-hybrid-delegate \
    --dir "$REPO" \
    --dir "/tmp" \
    --dir "/Applications/Gamez/Majesty 2.app" \
    --model sonnet \
    --effort low \
    --max-turns 25 \
    --out "/tmp/metaclaw_wave_${WAVE}_report.md" \
    "Ты автономный воркер MetaClaw Squad Gamma. НЕ спрашивай пользователя, работай полностью сам:
1. Прочитай текущий лучший результат в $BEST_JSON и профиль /tmp/m2_x86_profile.txt.
2. Через SSH на beyond-server (satrap-re-lab rea-ghidra) или локальный capstone в $REPO/.venv/bin/python3 разбери следующие 2-3 самые горячие функции из /tmp/m2_x86_profile.txt (scene.dll / render.dll / geometry.dll / traceable.dll / pf.dll / Majesty2.exe).
3. Добавь их SSE2-замены в $REPO/src/d3d9_proxy.cpp и скомпилируй через $HOME/.local/bin/cc-clore-exec в $SYSWOW/d3d9.dll.
4. Запусти M2_PROFILE=1 $REPO/.venv/bin/python3 $REPO/tools/m2_bench.py --label wave${WAVE}_5x --save bench_heavy --dwell 20 --speed 5 --out /tmp/m2_day230_wave${WAVE}_5x.json.
5. Если status == PASS и avg_fps вырос (или quant_ms упал) относительно $BEST_JSON — обнови $BEST_JSON, $REPO/src/d3d9_proxy.cpp.best и $SYSWOW/d3d9.dll.best. Если хуже или упало — откати из .best! Запиши итог в $REPO/HARNESS_BLACKBOARD.md." >> "$LOG" 2>&1 || true
done

echo "[$(date '+%Y-%m-%d %H:%M:%S')] MetaClaw Autonomous M2 Optimizer finished Waves 7..15." | tee -a "$LOG"
