#!/usr/bin/env python3
"""Compare the G2/G3 A* result-hash checkpoints of two m2_fps.log files
(t_v4_g2_ahash_gate, extended by t_12baab3f).

The proxy's M2_AHASH=1 mode appends, every 30 presented frames:

    # ahash: frame=<N> calls=<K> hash=0x<16 hex>

and, since G3, the same line also carries the cumulative simulation-quant index:

    # ahash: frame=<N> calls=<K> hash=0x<16 hex> q=<Q>

`hash` is one sliding FNV1a-64 over the A* results of frames up to N, `calls` is how many A*
results went into it, `q` is how many simulation quants had run by then.  Two runs of the same save
must produce the same checkpoint sequence when the code under the gate is bitwise identical; the
first frame where they differ localises the divergence to a 30-frame window.

Two comparison modes:

  strict  same frames, same calls, same hash -- the literal G2 criterion (`--align=strict`).
  offset  the same sequence shifted by ONE constant frame offset: run B reaches the same simulation
          state `delta` frames later.  The frame at which a saved game finishes loading is real-time
          dependent, so the phase can differ while the simulation itself is deterministic.  A clean
          offset alignment is reported as ALIGNED (exit 3) and is the honest determinism verdict
          when the strict test fails on phase only.

Usage:
    m2_ahash_compare.py <log_a> <log_b> [--by frame|eq] [--align=auto|strict|offset]
                        [--max-offset N] [--json OUT] [-q]

  --by eq   key checkpoints by the eq= field (engine quant counter, ms, restored from the save by
            eQuantControl::Load -- the M2_DETERMINISTIC=1 anchor) instead of by frame number.
            Two runs of one save share the eq grid by construction, so this is the honest
            comparison whenever the logs carry eq= (G4).  A log without any eq= field is
            "not comparable" (exit 2).  In this mode --max-offset is in eq (ms) units.

Exit codes:
    0  strict match: every checkpoint of both logs matched (same keys, same calls, same hash)
    3  ALIGNED: no strict match, but one constant offset makes every compared checkpoint of
       both logs agree (deterministic up to the start phase; the offset is reported).  The
       report also carries sim_quants=<d>: the difference in simulation-quant index between the
       two runs at their first compared checkpoint.  0 is the signature of a pure phase shift
       (the same state reached with the same number of quants); a nonzero value means the runs
       executed a different number of quants per frame, which needs explaining before calling it
       deterministic.
    1  divergence: a compared checkpoint differs, a checkpoint is missing inside the overlap, or no
       constant offset explains the two sequences
    2  not comparable: a file is missing/unreadable or carries no `# ahash:` lines at all
       (e.g. the run did not set M2_AHASH=1, or the game died before the first checkpoint)

Output (one line, machine-friendly):
    AHASH: MATCH checkpoints=13 frames=30..390
    AHASH: ALIGNED delta=+4 matched=13/13 frames=240..600 (a=240..600 b=244..604) sim_quants=+4
    AHASH: DIVERGE first_frame=90 matched=2/4 detail=a(calls=1001 hash=0x..) b(calls=1001 hash=0x..)
"""
import argparse
import json
import re
import sys
from pathlib import Path

# frame, calls, hash, then optional trailing `k=v` fields (G3: q=<quants>; unknown fields are kept
# in the parsed record but do not take part in the comparison, so a future field cannot break it).
AHASH_RE = re.compile(
    r"^#\s*ahash:\s*frame=(\d+)\s+calls=(\d+)\s+hash=0x([0-9A-Fa-f]+)"
    r"(?P<extra>(?:\s+[a-z_]+=-?\d+)*)\s*$")
EXTRA_RE = re.compile(r"([a-z_]+)=(-?\d+)")


def parse_log(path: Path) -> dict:
    """frame -> (calls, hash, extras).  Later duplicates of a frame win (a re-run appended to the log)."""
    out = {}
    for ln in path.read_text(errors="replace").splitlines():
        m = AHASH_RE.match(ln.strip())
        if not m:
            continue
        extras = {k: int(v) for k, v in EXTRA_RE.findall(m.group("extra") or "")}
        out[int(m.group(1))] = (int(m.group(2)), int(m.group(3), 16), extras)
    return out


def compare(a: dict, b: dict):
    """Strict comparison.  Return (first_divergent_frame, matched, total, detail)."""
    frames = sorted(set(a) | set(b))
    matched = 0
    first = None
    detail = ""
    for f in frames:
        in_a, in_b = f in a, f in b
        if in_a and in_b and a[f][:2] == b[f][:2]:
            matched += 1
            continue
        if first is None:
            first = f
            if not in_b:
                detail = f"frame {f} present only in log_a"
            elif not in_a:
                detail = f"frame {f} present only in log_b"
            else:
                detail = (f"frame {f}: a(calls={a[f][0]} hash=0x{a[f][1]:016X}) "
                          f"!= b(calls={b[f][0]} hash=0x{b[f][1]:016X})")
    return first, matched, len(frames), detail


def compare_offset(a: dict, b: dict, delta: int):
    """Compare A[f] with B[f+delta] over the overlapping window, in both directions.

    Returns (pairs, mismatches, unmatched): `unmatched` counts checkpoints inside the other log's
    observed range that have no counterpart -- a skipped checkpoint is a divergence, not a phase.
    """
    lo_a, hi_a = min(a), max(a)
    lo_b, hi_b = min(b), max(b)
    pairs, mismatches, unmatched = 0, [], []
    for f in sorted(a):
        g = f + delta
        if g < lo_b or g > hi_b:
            continue
        if g not in b:
            unmatched.append(f)
        elif a[f][:2] != b[g][:2]:
            mismatches.append(f)
        else:
            pairs += 1
    for g in sorted(b):
        f = g - delta
        if f < lo_a or f > hi_a:
            continue
        if f not in a:
            unmatched.append(g)
    return pairs, mismatches, unmatched


def try_align(a: dict, b: dict, max_offset: int, min_pairs: int = 2):
    """Find a constant frame offset that explains both logs.  Returns (delta, pairs) or (None, None)."""
    cands = [d for d in range(-max_offset, max_offset + 1) if d != 0]
    # the phase difference of the first checkpoint is the physically meaningful candidate: try first
    cands.sort(key=lambda d: (abs(d - (min(b) - min(a))), abs(d)))
    for delta in cands:
        pairs, mismatches, unmatched = compare_offset(a, b, delta)
        if mismatches or unmatched or pairs < min_pairs:
            continue
        return delta, pairs
    return None, None


def load(path: str):
    p = Path(path)
    if not p.is_file():
        print(f"AHASH: ERROR log not found: {p}", file=sys.stderr)
        return None
    d = parse_log(p)
    if not d:
        print(f"AHASH: ERROR no '# ahash:' checkpoints in {p} "
              f"(was M2_AHASH=1 set for this run?)", file=sys.stderr)
        return None
    return d


def remap_by_eq(d: dict):
    """frame-keyed -> eq-keyed.  Returns (remapped, dropped) where dropped counts checkpoints
    without an eq= field (frame-keyed lines from a non-deterministic run mixed in)."""
    out, dropped = {}, 0
    for _f, (calls, h, extras) in d.items():
        if "eq" not in extras:
            dropped += 1
            continue
        out[int(extras["eq"])] = (calls, h, extras)
    return out, dropped


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log_a")
    ap.add_argument("log_b")
    ap.add_argument("--by", default="frame", choices=("frame", "eq"),
                    help="key checkpoints by frame number (default) or by the eq= field "
                         "(engine quant counter, the G4 M2_DETERMINISTIC anchor)")
    ap.add_argument("--align", default="auto", choices=("auto", "strict", "offset"),
                    help="auto (default): strict first, then one constant frame offset; "
                         "strict: frames must line up exactly; offset: only the aligned test")
    ap.add_argument("--max-offset", type=int, default=90,
                    help="largest frame offset the alignment search may use (default 90 = 3 checkpoints; "
                         "in --by=eq mode this is in eq ms units -- pass e.g. 200000)")
    ap.add_argument("--json", default=None, help="also write the verdict as JSON")
    ap.add_argument("-q", "--quiet", action="store_true", help="suppress the summary line for matching logs")
    a = ap.parse_args()

    A, B = load(a.log_a), load(a.log_b)
    if A is None or B is None:
        return 2

    keyname = "frame"
    if a.by == "eq":
        A, drA = remap_by_eq(A)
        B, drB = remap_by_eq(B)
        if not A or not B:
            print("AHASH: ERROR no eq= checkpoints (was M2_DETERMINISTIC=1 active for this run? "
                  f"dropped-without-eq: a={drA} b={drB})", file=sys.stderr)
            return 2
        keyname = "eq"

    first, matched, total, detail = compare(A, B)
    if keyname == "eq":
        detail = detail.replace("frame ", "eq ", 1)
    fa = min(A)
    qa = [A[f][2].get("q") for f in sorted(A)]
    qb = [B[f][2].get("q") for f in sorted(B)]
    result = {
        "log_a": a.log_a, "log_b": a.log_b, "by": a.by, "key": keyname,
        "checkpoints_a": len(A), "checkpoints_b": len(B),
        "matched": matched, "total_frames": total,
        "frames_a": [fa, max(A)], "frames_b": [min(B), max(B)],
        "match": first is None, "strict_match": first is None,
        "first_divergent_frame": first, "detail": detail,
        "align_mode": a.align, "max_offset": a.max_offset,
    }
    if first is None:
        result["verdict"] = "MATCH"
        if not a.quiet:
            if keyname == "frame":
                print(f"AHASH: MATCH checkpoints={matched} frames={min(fa, min(B))}..{max(max(A), max(B))}")
            else:
                print(f"AHASH: MATCH by={keyname} checkpoints={matched} "
                      f"{keyname}={min(fa, min(B))}..{max(max(A), max(B))}")
        rc = 0
    else:
        delta, pairs = (None, None)
        if a.align in ("auto", "offset"):
            delta, pairs = try_align(A, B, a.max_offset)
        if delta is not None:
            dq = None
            if delta and all(x is not None for x in qa) and all(x is not None for x in qb):
                dq = qb[0] - qa[0]     # sim quants advance once per frame in the fixed-dt mode
            result.update({"verdict": "ALIGNED", "delta": delta, "aligned_pairs": pairs,
                           "sim_quants_delta": dq})
            if keyname == "frame":
                msg = (f"AHASH: ALIGNED delta={delta:+d} matched={pairs}/{min(len(A), len(B))} "
                       f"a={fa}..{max(A)} b={min(B)}..{max(B)}")
            else:
                msg = (f"AHASH: ALIGNED by={keyname} delta={delta:+d} matched={pairs}/{min(len(A), len(B))} "
                       f"a={fa}..{max(A)} b={min(B)}..{max(B)}")
            if dq is not None:
                msg += f" sim_quants={dq:+d}"
            print(msg)
            rc = 3
        else:
            result["verdict"] = "DIVERGE"
            print(f"AHASH: DIVERGE by={keyname} first_{keyname}={first} "
                  f"matched={matched}/{total} detail={detail}")
            if a.align in ("auto", "offset"):
                print(f"  (no constant {keyname} offset in +-{a.max_offset} explains the sequences)")
            rc = 1
    if a.json:
        Path(a.json).write_text(json.dumps(result, indent=1) + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
