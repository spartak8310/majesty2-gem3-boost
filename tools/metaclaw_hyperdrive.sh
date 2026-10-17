#!/usr/bin/env bash
[ "${M2_ALLOW_LEGACY_LOOP:-0}" = "1" ] || { echo "DEPRECATED: this loop compiles straight into the live game install with no restore. Use tools/m2_closed_loop.py (v4: staging + golden restore). Override: M2_ALLOW_LEGACY_LOOP=1" >&2; exit 1; }
set -uo pipefail

# ==============================================================================
# MetaClaw HyperDrive: Autonomous Architectural Optimizer for Majesty 2
# ==============================================================================
# Paradigms:
#   1. Spatial Simulation LOD (Frustum Culling of off-screen particles, bones & events)
#   2. "Ship of Theseus" Subsystem Replacement (pf.dll particles, scene.dll spatial hash)
#   3. Hybrid Decoupled 120 FPS Render Loop (Present & Camera interpolation)
#
# Hardware Backend:
#   - Clore.ai #84966: 32-thread Ryzen 9 5950X, 36GB tmpfs RAM (35 GB/s) via cc-clore-exec
#   - Autonomous Agent: cc-hybrid-delegate (Claude Sonnet 4.6 Low / Haiku 5.5)
#   - Benchmark: tools/m2_bench.py (Day 230 / save167 / 300+ units battle / speed 5)
# ==============================================================================

REPO="./scratch/majesty2-re"
SYSWOW="/Applications/Gamez/Majesty 2.app/Contents/SharedSupport/prefix/drive_c/windows/syswow64"
LOG="/tmp/metaclaw_hyperdrive.log"
BEST_JSON="/tmp/m2_metaclaw_best.json"
PID_FILE="/tmp/metaclaw_hyperdrive.pid"

echo $$ > "$PID_FILE"
echo "[$(date '+%Y-%m-%d %H:%M:%S')] === MetaClaw HyperDrive Autonomous Harness Started (PID $$) ===" | tee -a "$LOG"

cleanup() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] HyperDrive shutting down..." | tee -a "$LOG"
    rm -f "$PID_FILE"
    pkill -9 -f "Majesty2|wineserver|wine" 2>/dev/null || true
    exit 0
}
trap cleanup SIGINT SIGTERM

# Step 0: Ensure baseline is compiled and valid
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Verifying baseline build via Clore..." | tee -a "$LOG"
$HOME/.local/bin/cc-clore-exec \
  --job m2-metaclaw-build \
  --sync-in "$REPO/src/d3d9_proxy.cpp" \
  --sync-out d3d9.dll:"$SYSWOW/d3d9.dll" \
  "i686-w64-mingw32-g++ -shared -O3 -msse2 -mfpmath=sse -s -static-libgcc -static-libstdc++ -Wl,--kill-at d3d9_proxy.cpp -o d3d9.dll -lgdi32 -luser32" >> "$LOG" 2>&1

cp "$REPO/src/d3d9_proxy.cpp" "$REPO/src/d3d9_proxy.cpp.best"
cp "$SYSWOW/d3d9.dll" "$SYSWOW/d3d9.dll.best"

# Run initial baseline benchmark if no best exists
if [[ ! -f "$BEST_JSON" ]]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Running baseline Day 230 5x benchmark..." | tee -a "$LOG"
    M2_PROFILE=1 "$REPO/.venv/bin/python3" "$REPO/tools/m2_bench.py" \
      --label baseline_hyperdrive --save bench_heavy --dwell 20 --speed 5 --out "$BEST_JSON" >> "$LOG" 2>&1 || true
fi

# Define the Macro Waves
# Wave 8: Spatial Frustum Culling & Global Camera Plane Extraction in d3d9_proxy.cpp
# Wave 9: Particle System Off-screen Culling (pf.dll / xParticleFlow)
# Wave 10: Off-screen Animation & Bone Transform Throttling (scene.dll)
# Wave 11: Decoupled 120 FPS Present & Frame Interpolation Hook

declare -a WAVES=(
  "wave8:SpatialFrustumCulling:Внедри в src/d3d9_proxy.cpp перехват SetTransform(D3DTS_VIEW=2) и SetTransform(D3DTS_PROJECTION=3). Извлеки 6 плоскостей усечённой пирамиды камеры (Frustum Planes по алгоритму Gribb-Hartmann). Создай потокобезопасную функцию IsInCameraFrustum(float x, float y, float z, float radius, float margin=60.0f). Внедри проверку в Hook_eZones_UpdateArea_96B20: если зона находится вне фрустума камеры + margin, пропускай тяжёлое обновление матриц и геометрии коллизий или обновляй её не чаще 1 раза в 10 кадров."
  "wave9:ParticleFlowCulling:Внедри отсечение частиц pf.dll (xParticleFlow): найди через дамп SDK функции создания и отрисовки эмиттеров частиц (xParticleFlow::PaintPoint, xParticleFlow::eOperator::operator()). Если источник частиц находится вне пирамиды видимости камеры (IsInCameraFrustum возвращает false), полностью глуши расчёт и отрисовку частиц. На Day 230 это должно устранить миллионы бесполезных вычислений магии и пыли за пределами экрана."
  "wave10:SkeletonAnimThrottling:Внедри прореживание скелетной анимации в scene.dll для невидимых юнитов: найди функцию обновления костей/матриц скиннинга моделей (UpdateSkeleton / EvaluateBones). Для моделей, центр которых вне экрана, пропускай конкатенацию матриц костей (Matrix34 scaling/rotation), обновляя их только при входе в поле зрения камеры."
  "wave11:Decoupled120FPSRender:Внедри гибридную отвязку рендера от симуляции в d3d9_proxy.cpp: если квант симуляции Quant длится дольше 20 мс, не блокируй вызовы Present(), а интерполируй положение камеры и курсора мыши, чтобы экран обновлялся на 120 FPS ProMotion независимо от скорости симуляции."
)

for WAVE_ENTRY in "${WAVES[@]}"; do
  IFS=":" read -r W_ID W_NAME W_TASK <<< "$WAVE_ENTRY"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] ==========================================" | tee -a "$LOG"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Launching MetaClaw Autonomous Wave: $W_NAME" | tee -a "$LOG"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] ==========================================" | tee -a "$LOG"

  REPORT_FILE="/tmp/hyperdrive_${W_ID}.md"
  
  $HOME/.local/bin/cc-hybrid-delegate \
    --dir "$REPO" \
    --dir "/tmp" \
    --dir "/Applications/Gamez/Majesty 2.app" \
    --model sonnet \
    --effort low \
    --max-turns 30 \
    --out "$REPORT_FILE" \
    "Ты ведущий инженер-архитектор движков и мета-харнесс билдер MetaClaw.
Твоя цель: совершить квантовый скачок FPS в Majesty 2 (с 5 FPS до 20-60+ FPS симуляции и 120 FPS рендера) через крупноблочную архитектурную замену, а не микро-гриндинг!

Твоя задача в этой волне ($W_NAME):
$W_TASK

Правила работы:
1. Изучи текущий код $REPO/src/d3d9_proxy.cpp и блэкборд $REPO/HARNESS_BLACKBOARD.md.
2. Реализуй архитектурное изменение прямо в $REPO/src/d3d9_proxy.cpp (или новых файлах).
3. Скомпилируй через Clore:
   $HOME/.local/bin/cc-clore-exec --job m2-metaclaw-build --sync-in \"$REPO/src/d3d9_proxy.cpp\" --sync-out d3d9.dll:\"$SYSWOW/d3d9.dll\" \"i686-w64-mingw32-g++ -shared -O3 -msse2 -mfpmath=sse -s -static-libgcc -static-libstdc++ -Wl,--kill-at d3d9_proxy.cpp -o d3d9.dll -lgdi32 -luser32\"
4. Запусти бенчмарк:
   M2_PROFILE=1 $REPO/.venv/bin/python3 $REPO/tools/m2_bench.py --label ${W_ID}_5x --save bench_heavy --dwell 20 --speed 5 --out /tmp/m2_${W_ID}.json
5. Сравни результат с $BEST_JSON:
   - Если статус PASS и avg_fps вырос (или quant_ms упал) без визуальных глитчей:
     cp /tmp/m2_${W_ID}.json $BEST_JSON
     cp $REPO/src/d3d9_proxy.cpp $REPO/src/d3d9_proxy.cpp.best
     cp $SYSWOW/d3d9.dll $SYSWOW/d3d9.dll.best
     echo 'WAVE $W_NAME SUCCEEDED' >> $REPO/HARNESS_BLACKBOARD.md
   - Если краш или FPS упал:
     cp $REPO/src/d3d9_proxy.cpp.best $REPO/src/d3d9_proxy.cpp
     cp $SYSWOW/d3d9.dll.best $SYSWOW/d3d9.dll
     echo 'WAVE $W_NAME ROLLED BACK' >> $REPO/HARNESS_BLACKBOARD.md
6. Сформируй детальный инженерный отчёт в $REPORT_FILE." >> "$LOG" 2>&1 || true

done

echo "[$(date '+%Y-%m-%d %H:%M:%S')] === MetaClaw HyperDrive Complete! ===" | tee -a "$LOG"
