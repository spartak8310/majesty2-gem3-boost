set -u
R=./scratch/majesty2-re
APP="/Applications/Gamez/Majesty 2.app"
export WINEPREFIX="$APP/Contents/SharedSupport/prefix"
export DYLD_FALLBACK_LIBRARY_PATH="$APP/Contents/Frameworks:$APP/Contents/SharedSupport/wine/lib"
export SikarugirAppWine11=1 ROSETTA_ADVERTISE_AVX=1 WINEDEBUG=-all WINEMSYNC=0 WINEESYNC=0
WINE="$APP/Contents/SharedSupport/wine/bin/wine"
cd "$R/build/v4"
pgrep -fl 'Majesty2' | grep -v m2_hands || echo "no game running"
for cw in 037F 027F 007F; do
  echo "######## fpbench2 --cw $cw  $(date +%T)"
  "$WINE" fpbench2.exe --cw $cw 2>&1 | tr -d '\r' | tee /tmp/fpbench2_$cw.log
  echo "rc=$?  $(date +%T)"
done
"$APP/Contents/SharedSupport/wine/bin/wineserver" -k 2>/dev/null; echo done
