#!/bin/sh
REPO=./scratch/majesty2-re
OUT_DIR=./scratch/majesty2-re/re/x5_out
RUNS=1
# X5 (task t_0b519eff) — .hands microtest v2: x87 acceleration for a 32-bit PE
# under Rosetta 2 on macOS 27.0.1, with the engine switchable.
#
#   X5_ENGINE=sidecar   (default) athei/x87sidecar — the maintained successor of
#                       rosettax87_jit (MIT, v1.9.0 2026-10-13). Its README states
#                       it runs on macOS 27 and it is benchmarked on macOS 27.0;
#                       it finds hooks by anchors, so a Rosetta update cannot
#                       silently break it, and `--probe` reports the verdict.
#   X5_ENGINE=rosettax87 (legacy) the X4 path. KNOWN BROKEN on macOS 27.0.1:
#                       upstream issue #14 (PT_THUPDATE detach) and the offset
#                       scan failure we saw locally; kept for A/B and for the
#                       record only.
#
# What is measured: build/v4/fpbench2.exe (sha256 06a89248…, PE32) x87 loops
#   A) baseline : wine fpbench2.exe --cw 027F
#   B) engine   : <engine> <wine-Mach-O> fpbench2.exe --cw 027F
#   C) optional : B + cooperative attach (X5_COOP=1; needs a wine built with the
#                 handshake — athei/wine-build — and ROSETTA_X87_PATH support)
#
# GATE (unchanged from X4): game work only if B/A >= 3x on the x87 loops
# (DIST_X87_NS / HEAPKEY_X87_NS) AND COMPARE_*_MISMATCHES_CW027F == 0, and only
# by the owner's decision on a separate card.
#
# Mac facts this script assumes (see re/X5_ROSETTAX87_FIX.md):
#   * macOS 27.0.1 (26A434) + the Rosetta runtime of 2026-08-08;
#   * the first default-attach run shows a "developer tools / authorize" dialog
#     once per login session — a human at the Mac must accept it, or set
#     X5_SUDO=1 (root, as upstream's own CI does);
#   * the cooperative path needs no dialog, no root and no entitlements.
#   * the game is NOT launched; only the console fpbench2.exe in the stock
#     prefix (read/exec) or a prefix of your own via PREFIX=…
#
# Usage (Cowork, on the Mac):
#   sh metaclaw_v4/hands_x4_fpbench.sh                 # sidecar: install+probe+A+B(+C)
#   X5_SUDO=1 sh metaclaw_v4/hands_x4_fpbench.sh       # no dialog: root attach
#   X5_ENGINE=rosettax87 sh metaclaw_v4/hands_x4_fpbench.sh   # legacy A/B
#   X5_BUILD=1 sh metaclaw_v4/hands_x4_fpbench.sh      # build the engine from source
#   X5_ROLLBACK=0 X5_KEEP=1 sh …                       # keep the temp tree for debugging
#
# As a .hands task: .hands/inbox/NNN_x5_fpbench.sh (chmod 700) → .hands/out/NNN.log.
# This script was NOT executed by the card that wrote it (t_0b519eff).

set -u

# ─────────────────────────── parameters ───────────────────────────
# Card asks for the temp build tree under R/tools/…; REPO defaults to this
# repo's root (metaclaw_v4/..), so the engine trees live in tools/ and are
# removed by the rollback. Override with X5_DIR / X4_DIR if you prefer $HOME.
REPO="${REPO:-$(cd "$(dirname "$0")/.." 2>/dev/null && pwd || echo "$PWD")}"
X5_DIR="${X5_DIR:-$REPO/tools/x87sidecar}"                # successor engine
X4_DIR="${X4_DIR:-$REPO/tools/rosettax87}"                # legacy engine
OUT_DIR="${OUT_DIR:-$HOME/x5_fpbench_out}"                # logs/summary
FPBENCH="${FPBENCH:-$REPO/build/v4/fpbench2.exe}"
FPBENCH_SHA="06a89248ff7bd525a4b1812602e77b21eac419bee52683d53b89ccf56d6a1713"
APP="${APP:-/Applications/Gamez/Majesty 2.app}"
WINE="${WINE:-$APP/Contents/SharedSupport/wine/bin/wine}"
LIBDIR="${LIBDIR:-$APP/Contents/Frameworks:$APP/Contents/SharedSupport/wine/lib}"
PREFIX="${PREFIX:-$APP/Contents/SharedSupport/prefix}"
CW="${CW:-027F}"                                          # 007F | 027F | 037F
RUNS="${RUNS:-2}"                                         # runs per arm (thermal drift)
X5_ENGINE="${X5_ENGINE:-sidecar}"                         # sidecar | rosettax87
X5_SUDO="${X5_SUDO:-0}"                                   # 1 = arm B under sudo
X5_COOP="${X5_COOP:-0}"                                   # 1 = arm C via ROSETTA_X87_PATH
X5_BUILD="${X5_BUILD:-0}"                                 # 1 = cmake build from source
X5_ROLLBACK="${X5_ROLLBACK:-1}"                           # 1 = remove the temp tree
X5_KILL_WINESERVER="${X5_KILL_WINESERVER:-0}"

# x87sidecar v1.9.0 (2026-10-13) — pinned release assets, sha256 measured
# 2026-10-16 by downloading them from the release page.
SIDECAR_TAG="v1.9.0"
SIDECAR_TARBALL_URL="https://github.com/athei/x87sidecar/releases/download/$SIDECAR_TAG/x87sidecar_entitled.tar.xz"
SIDECAR_TARBALL_SHA="4eed9abeeb06a75daed3935980bf849554ab964c6fe2489a5be2efb846711704"
SIDECAR_SRC_TAG="v1.9.0"
# Legacy rosettax87_jit release (the asset our bundle ships; predates the
# 2026-07-14 offset-scan rework — see the receipt §2.3).
X4_TARBALL_URL="https://github.com/Lifeisawful/rosettax87_jit/releases/download/latest/rosettax87-jit-macos-arm64.tar.gz"
X4_TARBALL_SHA="a1e7d85eb83451c2eef24530a508fccd45720b1fde87c0097a44f12ffdf90539"

mkdir -p "$OUT_DIR"
SUMMARY="$OUT_DIR/x5_summary.txt"
: >"$SUMMARY"
log() { echo "$@" | tee -a "$SUMMARY"; }
say() { echo "== $*"; }

# ─────────────────────────── 0. environment ───────────────────────────
say "X5 fpbench microtest (engine=$X5_ENGINE)  $(date '+%Y-%m-%d %H:%M:%S %Z')"
log "X5_FPBENCH start=$(date -u '+%Y-%m-%dT%H:%M:%SZ') engine=$X5_ENGINE"
log "host: $(uname -a)"
log "macOS: $(sw_vers -productVersion 2>/dev/null || echo unknown) build $(sw_vers -buildVersion 2>/dev/null || echo unknown)"
log "kern.version: $(sysctl -n kern.version 2>/dev/null || echo unknown)"
log "rosetta-runtime: $(ls -l /usr/libexec/rosetta/runtime 2>/dev/null || echo MISSING)"
log "rosetta-runtime sha256: $(shasum -a 256 /usr/libexec/rosetta/runtime 2>/dev/null | awk '{print $1}')"
log "wine: $WINE"
log "prefix: $PREFIX"
log "fpbench2: $FPBENCH"

[ -f "$FPBENCH" ] || { log "FATAL: fpbench2.exe not found: $FPBENCH"; exit 2; }
HAVE_SHA="$(shasum -a 256 "$FPBENCH" 2>/dev/null | awk '{print $1}')"
log "fpbench2 sha256: $HAVE_SHA (expect $FPBENCH_SHA)"
[ "$HAVE_SHA" = "$FPBENCH_SHA" ] || { log "FATAL: fpbench2.exe sha256 mismatch — not running."; exit 2; }
{ [ -x "$WINE" ] || [ -f "$WINE" ]; } || { log "FATAL: wine not found: $WINE"; exit 2; }

# wrap target: runtime_loader/sidecar must wrap a Mach-O, not a shell script
WINE_BIN_DIR="$(dirname "$WINE")"
WRAP_TARGET="$WINE"
if ! file "$WINE" 2>/dev/null | grep -q "Mach-O"; then
  for cand in "$WINE_BIN_DIR/wine64-preloader" "$WINE_BIN_DIR/wine64" "$WINE_BIN_DIR/wine-preloader"; do
    if [ -f "$cand" ] && file "$cand" 2>/dev/null | grep -q "Mach-O"; then WRAP_TARGET="$cand"; break; fi
  done
  log "wine is not Mach-O; wrapping: $WRAP_TARGET"
fi

# ─────────────────────────── 1. engine install ───────────────────────────
if [ "$X5_ENGINE" = "sidecar" ]; then
  say "1. x87sidecar $SIDECAR_TAG → $X5_DIR"
  mkdir -p "$X5_DIR"
  ENGINE="$X5_DIR/x87sidecar_entitled"

  if [ "$X5_BUILD" = "1" ]; then
    say "1b. build from source (cmake + clang; needs the macOS SDK via Command Line Tools)"
    if ! command -v cmake >/dev/null 2>&1; then
      log "cmake not installed — falling back to the pinned release binary (nothing is installed system-wide)."
    else
      SRC="$X5_DIR/src"
      if [ ! -d "$SRC/.git" ]; then
        git clone -q --depth 1 --branch "$SIDECAR_SRC_TAG" https://github.com/athei/x87sidecar.git "$SRC" \
          || { log "FATAL: clone of $SIDECAR_SRC_TAG failed"; exit 3; }
      fi
      log "source commit: $(git -C "$SRC" rev-parse HEAD 2>/dev/null)"
      ( cd "$SRC" && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(sysctl -n hw.ncpu)" ) \
        >"$OUT_DIR/build.log" 2>&1
      BRC=$?
      log "build rc=$BRC (log: $OUT_DIR/build.log)"
      if [ "$BRC" = "0" ] && [ -x "$SRC/build/bin/x87sidecar_entitled" ]; then
        ENGINE="$SRC/build/bin/x87sidecar_entitled"
      elif [ "$BRC" = "0" ] && [ -x "$SRC/build/bin/x87sidecar" ]; then
        ENGINE="$SRC/build/bin/x87sidecar"
        log "note: unentitled build → default attach needs root (X5_SUDO=1) or --cooperative"
      else
        log "build failed — falling back to the pinned release binary"
      fi
    fi
  fi

  if [ ! -f "$ENGINE" ]; then
    TB="$X5_DIR/x87sidecar_entitled.tar.xz"
    curl -fL --retry 3 -o "$TB" "$SIDECAR_TARBALL_URL" || { log "FATAL: download failed: $SIDECAR_TARBALL_URL"; exit 3; }
    TB_SHA="$(shasum -a 256 "$TB" | awk '{print $1}')"
    log "tarball sha256: $TB_SHA (expect $SIDECAR_TARBALL_SHA)"
    [ "$TB_SHA" = "$SIDECAR_TARBALL_SHA" ] || { log "FATAL: sha256 mismatch — install stopped."; exit 3; }
    tar -xJf "$TB" -C "$X5_DIR" || { log "FATAL: tar failed"; exit 3; }
    ENGINE="$X5_DIR/x87sidecar_entitled"
  fi
  [ -f "$ENGINE" ] || { log "FATAL: no engine binary after install"; exit 3; }
  xattr -d com.apple.quarantine "$ENGINE" 2>/dev/null || true
  chmod +x "$ENGINE" 2>/dev/null || true
  log "engine: $ENGINE  sha256=$(shasum -a 256 "$ENGINE" | awk '{print $1}')"
  log "engine file: $(file -b "$ENGINE" 2>/dev/null)"
  codesign -dv --verbose=2 "$ENGINE" 2>&1 | sed 's/^/codesign: /' | tee -a "$SUMMARY" >/dev/null || true

  # probe gate: exits 0 only when every feature is supported on THIS Rosetta
  say "1c. engine probe (read-only)"
  "$ENGINE" --probe >"$OUT_DIR/probe.log" 2>&1
  PROBE_RC=$?
  log "probe rc=$PROBE_RC (log: $OUT_DIR/probe.log)"
  sed 's/^/    /' "$OUT_DIR/probe.log" | tail -25 | tee -a "$SUMMARY" >/dev/null
  if [ "$PROBE_RC" != "0" ]; then
    log "GATE: probe reports this Rosetta as UNSUPPORTED — arms B/C would run UNHOOKED."
    log "      Do not read a ratio from this run; report probe.log upstream instead."
  fi

  # cooperative mode availability: does this wine know ROSETTA_X87_PATH?
  COOP_SUPPORT=0
  grep -a -q -m1 "ROSETTA_X87_PATH" "$WINE_BIN_DIR"/* 2>/dev/null && COOP_SUPPORT=1
  log "wine ROSETTA_X87_PATH support: $([ "$COOP_SUPPORT" = 1 ] && echo FOUND || echo 'not found (stock Sikarugir wine; needs athei/wine-build for arm C)')"
else
  say "1. legacy rosettax87_jit → $X4_DIR  (KNOWN BROKEN on macOS 27 — see re/X5_ROSETTAX87_FIX.md)"
  mkdir -p "$X4_DIR"
  if [ ! -f "$X4_DIR/runtime_loader" ]; then
    TB="$X4_DIR/rosettax87-jit-macos-arm64.tar.gz"
    curl -fL --retry 3 -o "$TB" "$X4_TARBALL_URL" || { log "FATAL: download failed: $X4_TARBALL_URL"; exit 3; }
    TB_SHA="$(shasum -a 256 "$TB" | awk '{print $1}')"
    log "tarball sha256: $TB_SHA (expect $X4_TARBALL_SHA)"
    [ "$TB_SHA" = "$X4_TARBALL_SHA" ] || { log "FATAL: sha256 mismatch — install stopped."; exit 3; }
    tar -xzf "$TB" -C "$X4_DIR" || { log "FATAL: tar failed"; exit 3; }
    xattr -dr com.apple.quarantine "$X4_DIR" 2>/dev/null || true
    chmod +x "$X4_DIR/runtime_loader" 2>/dev/null || true
  fi
  ENGINE="$X4_DIR/runtime_loader"
  log "engine: $ENGINE sha256=$(shasum -a 256 "$ENGINE" | awk '{print $1}')"
  log "NOTE: this asset predates upstream's 2026-07-14 offset-scan rework, so it"
  log "      prints 'Offset not found in libRosettaRuntime binary' on the current runtime."
fi

# ─────────────────────────── 2. arms ───────────────────────────
run_arm() { # run_arm <label> [--sudo] <cmd...>
  label="$1"; shift
  n=1
  while [ "$n" -le "$RUNS" ]; do
    LOGF="$OUT_DIR/$label.$n.log"
    say "arm $label, run $n -> $LOGF"
    if [ "${1:-}" = "--sudo" ]; then
      shift
      sudo -n env "HOME=$HOME" "WINEPREFIX=$PREFIX" "DYLD_FALLBACK_LIBRARY_PATH=$LIBDIR" WINEDEBUG=-all "$@" >"$LOGF" 2>&1
    else
      ( export WINEPREFIX="$PREFIX" DYLD_FALLBACK_LIBRARY_PATH="$LIBDIR" WINEDEBUG=-all; "$@" ) >"$LOGF" 2>&1
    fi
    rc=$?
    done_mark="$(grep -c '^DONE=1' "$LOGF" 2>/dev/null)"; done_mark="${done_mark:-0}"
    echo "   rc=$rc  DONE=$done_mark  $(grep -m1 -E '^(DIST_X87_NS|CW_ACTIVE)=' "$LOGF" 2>/dev/null || true)"
    n=$((n+1))
  done
}

say "2. wine health-check (cmd /c echo X5_WINE_OK)"
( export WINEPREFIX="$PREFIX" DYLD_FALLBACK_LIBRARY_PATH="$LIBDIR" WINEDEBUG=-all; "$WINE" cmd /c echo X5_WINE_OK ) >"$OUT_DIR/health.log" 2>&1
HRC=$?
if grep -q "X5_WINE_OK" "$OUT_DIR/health.log"; then log "wine health: OK (rc=$HRC)"; else log "wine health: PROBLEM (rc=$HRC) — see health.log"; fi

say "3. arm A — baseline (stock Rosetta, no engine)"
run_arm A "$WINE" "$FPBENCH" --cw "$CW"

say "4. arm B — engine attached (default attach)"
if [ "$X5_SUDO" = 1 ]; then
  log "! arm B under sudo (no authorization dialog); upstream CI uses this mode too"
  run_arm B --sudo "$ENGINE" "$WRAP_TARGET" "$FPBENCH" --cw "$CW"
else
  log "! the first run shows a developer-tools/authorize dialog — accept it at the Mac"
  run_arm B "$ENGINE" "$WRAP_TARGET" "$FPBENCH" --cw "$CW"
fi

if [ "$X5_ENGINE" = "sidecar" ] && [ "$X5_COOP" = 1 ]; then
  say "5. arm C — cooperative attach via ROSETTA_X87_PATH"
  if [ "$COOP_SUPPORT" != 1 ]; then
    log "arm C skipped: this wine has no ROSETTA_X87_PATH handshake (build/use athei/wine-build)"
  else
    n=1
    while [ "$n" -le "$RUNS" ]; do
      LOGF="$OUT_DIR/C.$n.log"
      say "arm C, run $n -> $LOGF"
      ( export WINEPREFIX="$PREFIX" DYLD_FALLBACK_LIBRARY_PATH="$LIBDIR" WINEDEBUG=-all
        ROSETTA_X87_PATH="$ENGINE" "$WINE" "$FPBENCH" --cw "$CW" ) >"$LOGF" 2>&1
      echo "   rc=$?  DONE=$(grep -c '^DONE=1' "$LOGF" 2>/dev/null || echo 0)"
      n=$((n+1))
    done
  fi
fi

# ─────────────────────────── 6. parse ───────────────────────────
say "6. parse"
val() { awk -F= -v k="$2" '$1==k {v=$2} END{print v}' "$1" 2>/dev/null; }
minv() { # minv <label> <key>
  mm=""
  for f in "$OUT_DIR"/$1.*.log; do
    [ -f "$f" ] || continue
    v="$(val "$f" "$2")"
    case "$v" in ''|*[!0-9.]*) continue;; esac
    if [ -z "$mm" ]; then mm="$v"; else mm="$(awk -v a="$mm" -v b="$v" 'BEGIN{print (b<a)?b:a}')"; fi
  done
  echo "$mm"
}
mism() { # mism <label> <key> — total mismatches over runs
  s=0
  for f in "$OUT_DIR"/$1.*.log; do
    [ -f "$f" ] || continue
    v="$(val "$f" "$2")"; case "$v" in ''|*[!0-9]*) continue;; esac
    s=$((s+v))
  done
  echo "$s"
}

A_DIST="$(minv A DIST_X87_NS)"; B_DIST="$(minv B DIST_X87_NS)"
A_HEAP="$(minv A HEAPKEY_X87_NS)"; B_HEAP="$(minv B HEAPKEY_X87_NS)"
B_DONE=0
for f in "$OUT_DIR"/B.*.log; do
  [ -f "$f" ] || continue
  d="$(grep -c '^DONE=1' "$f" 2>/dev/null)"; d="${d:-0}"; B_DONE=$((B_DONE+d))
done
B_BANNER="$(grep -h -m1 -E 'RosettaRuntimex87 built|x87sidecar|SIDECAR' "$OUT_DIR"/B.*.log 2>/dev/null | head -1 || true)"
B_UNHOOKED="$(grep -h -c 'RUNNING WITHOUT X87 ACCELERATION' "$OUT_DIR"/B.*.log 2>/dev/null | awk '{s+=$1} END{print s+0}')"

log "----------------------------- SUMMARY -----------------------------"
log "engine=$X5_ENGINE CW=$CW RUNS=$RUNS (times are ns/iteration, minimum over runs)"
log "A baseline: DIST_X87_NS=$A_DIST HEAPKEY_X87_NS=$A_HEAP"
log "B engine  : DIST_X87_NS=$B_DIST HEAPKEY_X87_NS=$B_HEAP (DONE runs: $B_DONE; banner: ${B_BANNER:-NONE})"
log "B unhooked banners: $B_UNHOOKED"
log "precision B (sum COMPARE_*_MISMATCHES_CW027F): L1=$(mism B COMPARE_L1_MISMATCHES_CW027F) L3=$(mism B COMPARE_L3_MISMATCHES_CW027F)"
log "precision A:                                     L1=$(mism A COMPARE_L1_MISMATCHES_CW027F) L3=$(mism A COMPARE_L3_MISMATCHES_CW027F)"

VERDICT="unknown"; RATIO="n/a"
if [ -n "$A_DIST" ] && [ -n "$B_DIST" ]; then
  RATIO="$(awk -v a="$A_DIST" -v b="$B_DIST" 'BEGIN{ if (b>0) printf "%.2f", a/b; else print "n/a" }')"
fi
if [ "$B_UNHOOKED" != "0" ]; then
  VERDICT="BLOCKED: engine refused to hook (see probe.log / B.*.log) — no acceleration measured"
elif [ "$B_DONE" -ge 1 ] 2>/dev/null && [ "$RATIO" != "n/a" ]; then
  if awk -v r="$RATIO" 'BEGIN{exit !(r>=3.0)}'; then
    VERDICT="PASS (>=3x): game work only on a separate card by the owner's decision"
  else
    VERDICT="FAIL (<3x): gate not met"
  fi
elif [ "$X5_ENGINE" = "rosettax87" ] && grep -q "PT_THUPDATE" "$OUT_DIR"/B.*.log 2>/dev/null; then
  VERDICT="BLOCKED: legacy engine, upstream issue #14 (PT_THUPDATE) — switch to X5_ENGINE=sidecar"
else
  VERDICT="BLOCKED: no DONE=1 from arm B — see B.*.log"
fi
log "RATIO A/B (DIST_X87_NS): $RATIO"
log "VERDICT: $VERDICT"
log "RESULT x5_fpbench engine=$X5_ENGINE ratio_dist=$RATIO B_done=$B_DONE verdict=$VERDICT"
log "logs: $OUT_DIR"

# ─────────────────────────── 7. rollback ───────────────────────────
if [ "$X5_ROLLBACK" = 1 ]; then
  say "7. rollback"
  pkill -f "$X5_DIR" 2>/dev/null || true
  pkill -f "$X4_DIR" 2>/dev/null || true
  rm -rf "$X5_DIR" "$X4_DIR"
  log "rollback: engine processes stopped, temp trees removed ($X5_DIR, $X4_DIR)"
  if [ "$X5_KILL_WINESERVER" = 1 ]; then
    ( export WINEPREFIX="$PREFIX"; "$WINE" wineserver -k ) 2>/dev/null || true
    log "rollback: wineserver -k for $PREFIX"
  fi
else
  say "7. rollback disabled (X5_ROLLBACK=0); manual: rm -rf $X5_DIR $X4_DIR"
fi
say "ALL DONE $(date '+%Y-%m-%d %H:%M:%S')"
