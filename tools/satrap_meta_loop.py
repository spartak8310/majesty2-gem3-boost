#!/usr/bin/env python3
"""
SATRAP Meta-Loop & Self-Improving Engine Orchestrator
Operates in parallel with MetaClaw closed-loop to:
1. Analyze rejection patterns in closed-loop iterations
2. Audit in-game telemetry & benchmark receipts
3. Sync Hermes Kanban tasks (mimalloc arena, multithread quant)
4. Inject high-leverage architectural hypotheses into HARNESS_BLACKBOARD.md
5. Maintain continuous evolution without manual grinding
"""

import json
import os
import re
import subprocess
import time
from pathlib import Path
from typing import Dict, Any, List

REPO = Path("./scratch/majesty2-re")
BLACKBOARD = REPO / "HARNESS_BLACKBOARD.md"
CLOSED_LOOP_LOG = Path("/tmp/m2_closed_loop.log")
BEST_JSON = Path("/tmp/m2_metaclaw_best.json")
AUDIT_LOG = REPO / "meta_loop_audit.jsonl"


def get_closed_loop_state() -> Dict[str, Any]:
    state = {
        "running": False,
        "pid": None,
        "current_iteration": None,
        "best_fps": 5.04,
        "best_quant_ms": 144.48,
        "recent_events": [],
        "rejection_streak": 0,
    }

    # Check process
    try:
        out = subprocess.check_output(["pgrep", "-fl", "m2_closed_loop.py"], text=True)
        for line in out.splitlines():
            if "m2_closed_loop.py" in line:
                state["running"] = True
                state["pid"] = int(line.strip().split()[0])
                break
    except Exception:
        state["running"] = False

    # Check best json
    if BEST_JSON.exists():
        try:
            with open(BEST_JSON) as f:
                d = json.load(f)
                state["best_fps"] = d.get("avg_fps", state["best_fps"])
                state["best_quant_ms"] = d.get("quant_ms", state["best_quant_ms"])
        except Exception:
            pass

    # Parse closed loop log
    if CLOSED_LOOP_LOG.exists():
        lines = CLOSED_LOOP_LOG.read_text(encoding="utf-8", errors="ignore").splitlines()
        rejections = 0
        for line in reversed(lines):
            if ">>> ITERATION" in line and state["current_iteration"] is None:
                m = re.search(r"ITERATION (\d+)/(\d+)", line)
                if m:
                    state["current_iteration"] = int(m.group(1))
            if "REJECTED:" in line:
                rejections += 1
            if "SUCCESS:" in line:
                break
        state["rejection_streak"] = rejections
        state["recent_events"] = lines[-12:]

    return state


def audit_and_steer(state: Dict[str, Any]) -> Dict[str, Any]:
    recommendations = []
    
    # Plateau detection: If 2 or more iterations rejected, micro-optimizing proxy reached a plateau!
    if state["rejection_streak"] >= 2:
        recommendations.append(
            "PLATEAU_DETECTED: Leaf-level SSE2 optimizations in d3d9_proxy.cpp reached diminishing returns. "
            "Action: Pivot Haiku focus from scalar math to Subsystem Surgery: "
            "(1) Thread-local lock-free arena for small allocations (bypassing RtlAllocateHeap), "
            "(2) Batch chunking for entity animation/quant evaluation."
        )

    # Check if blackboard needs refresh
    update_blackboard = False
    if BLACKBOARD.exists():
        bb_content = BLACKBOARD.read_text(encoding="utf-8", errors="ignore")
        if "CURRENT_STATUS" not in bb_content or str(state["best_fps"]) not in bb_content:
            update_blackboard = True

    audit_entry = {
        "timestamp": time.time(),
        "time_iso": time.strftime("%Y-%m-%d %H:%M:%S"),
        "state": state,
        "recommendations": recommendations,
        "plateau": state["rejection_streak"] >= 2,
    }

    # Append to audit log
    with open(AUDIT_LOG, "a", encoding="utf-8") as f:
        f.write(json.dumps(audit_entry, ensure_ascii=False) + "\n")

    return audit_entry


def main():
    state = get_closed_loop_state()
    audit = audit_and_steer(state)
    print(json.dumps(audit, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
