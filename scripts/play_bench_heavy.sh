#!/usr/bin/env bash
# Осознанный запуск bench_heavy на ручную проверку (грузится и играется) через проверенную обвязку m2_bench.py:
# чистка процессов и локов, одноразовый autostart, WINEMSYNC=0. НЕ ПРОВЕРЕНО: собрано из существующих частей, ни разу не запускалось.
# usage: scripts/play_bench_heavy.sh [минут=30] [speed=0]   (speed 0 = обычные 1x, 5 = стресс-скорость)
set -euo pipefail

REPO="./scratch/majesty2-re"
MIN="${1:-30}"
SPEED="${2:-0}"

rm -f /tmp/m2_autostart.txt
trap 'rm -f /tmp/m2_autostart.txt' EXIT

cd "$REPO"
export WINEMSYNC=0 WINEESYNC=0 DXVK_ASYNC=1 MTL_HUD_ENABLED=0
"$REPO/.venv/bin/python3" tools/m2_bench.py --label play --save bench_heavy --speed "$SPEED" \
  --dwell "$((MIN * 60))" --timeout "$((MIN * 60 + 400))" --post-init-wait 150 --out /tmp/m2_play.json
echo "Готово. Итоги: /tmp/m2_play.json. Если игра зависла: pkill -9 -f 'Majesty2|wineserver|wine'"
