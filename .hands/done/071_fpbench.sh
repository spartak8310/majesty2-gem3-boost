set -u
R=./scratch/majesty2-re
APP="/Applications/Gamez/Majesty 2.app"
export WINEPREFIX="$APP/Contents/SharedSupport/prefix"
export DYLD_FALLBACK_LIBRARY_PATH="$APP/Contents/Frameworks:$APP/Contents/SharedSupport/wine/lib"
export SikarugirAppWine11=1 ROSETTA_ADVERTISE_AVX=1 WINEDEBUG=-all WINEMSYNC=0 WINEESYNC=0
WINE="$APP/Contents/SharedSupport/wine/bin/wine"
cd "$R/build/v4"
pgrep -fl 'Majesty2' | grep -v m2_hands || echo "no game running"
for exe in fpbench_x87.exe fpbench_sse.exe; do
  echo "######## $exe  $(date +%T)"
  "$WINE" "$exe" 2>&1 | tr -d '\r'
  echo "rc=$?"
done
"$APP/Contents/SharedSupport/wine/bin/wineserver" -k 2>/dev/null; echo done
