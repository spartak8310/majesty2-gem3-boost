#!/usr/bin/env python3
"""
8-Node Parallel Claude Haiku 5.5 Mega-Swarm for GEM 3 Engine (`claude_haiku_mega_swarm.py`)
Automatically extracts the top x87 / CPU hotspot functions across 8 GEM 3 engine modules
(`geometry.dll`, `traceable.dll`, `scene.dll`, `features.dll`, `render.dll`, `ui.dll`, `script.dll`, `app.dll`)
and dispatches 8 concurrent `claude-haiku-5-5` workers via `$HOME/.local/bin/cc-haiku-swarm`
to synthesize position-independent SSE2 / C++ patches.
"""
import concurrent.futures
import json
import os
import subprocess
import time
import pefile
import capstone

RE_DIR = "./scratch/majesty2-re"
GAME_DIR = "$HOME/Documents/Majesty2_Archive/Majesty 2 Collection"
SWARM_OUT_DIR = os.path.join(RE_DIR, "decompiled/claude_swarm")
CC_HAIKU_BIN = "$HOME/.local/bin/cc-haiku-swarm"

os.makedirs(SWARM_OUT_DIR, exist_ok=True)

with open(os.path.join(RE_DIR, "sdk/exports_by_dll.json"), "r", encoding="utf-8") as f:
    db = json.load(f)


def extract_x87_heavy_exports(dll_name: str, limit: int = 4) -> list:
    pe = pefile.PE(os.path.join(GAME_DIR, dll_name), fast_load=True)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    scored = []
    for e in db["binaries"].get(dll_name, {}).get("exports", []):
        rva_str = e.get("rva", "0x0")
        rva = int(rva_str, 16) if isinstance(rva_str, str) else int(rva_str)
        if rva == 0:
            continue
        try:
            off = pe.get_offset_from_rva(rva)
        except Exception:
            continue
        code = pe.__data__[off : off + 320]
        insns = []
        x87_count = 0
        for ins in md.disasm(code, pe.OPTIONAL_HEADER.ImageBase + rva):
            insns.append(f"0x{ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}")
            if ins.mnemonic.startswith("f") and ins.mnemonic not in ("femms",):
                x87_count += 1
            if ins.mnemonic in ("ret", "retn") and len(insns) > 3:
                break
        if len(insns) >= 10:
            scored.append(
                {
                    "mangled": e.get("mangled", ""),
                    "demangled": e.get("demangled", ""),
                    "rva": rva_str,
                    "x87_insns": x87_count,
                    "total_insns": len(insns),
                    "asm": "\n".join(insns[:45]),
                }
            )
    scored.sort(key=lambda x: (x["x87_insns"], x["total_insns"]), reverse=True)
    return scored[:limit]


SWARM_NODES = [
    ("mega1_geometry_frustum", "geometry.dll", "Перепиши самые тяжёлые x87-функции пересечений (Intersecting / Frustum / Sphere / Box) из geometry.dll на чистый позиционно-независимый SSE2 (Intel syntax asm + C++)."),
    ("mega2_traceable_raycast", "traceable.dll", "Декомпилируй самые тяжёлые x87-функции трассировки лучей и коллизий (xTraceable) из traceable.dll и напиши SSE2-замену без ветвлений."),
    ("mega3_scene_culling", "scene.dll", "Декомпилируй горячие x87-функции обхода графа сцены и видимости из scene.dll и предложи SSE2 / C++ хук ускорения."),
    ("mega4_features_collision", "features.dll", "Декомпилируй горячие функции коллизий юнитов (xPather::xCollision::eResolver) и физики из features.dll и напиши быстрый C++/SSE2 патч."),
    ("mega5_render_batching", "render.dll", "Декомпилируй горячие x87-функции матриц/шейдеров/батчинга из render.dll и напиши SSE2-ускоритель."),
    ("mega6_ui_layout", "ui.dll", "Декомпилируй самые тяжёлые функции пересчёта UI/текста из ui.dll и напиши C++ хук кэширования (dirty-flag) чтобы UI не пересчитывался каждый кадр."),
    ("mega7_script_vm", "script.dll", "Декомпилируй горячие функции скриптовой машины xScript из script.dll и предложи C++ хук оптимизации вызовов квантов."),
    ("mega8_app_mainloop", "app.dll", "Декомпилируй функции главного цикла приложения и таймера квантов из app.dll и предложи хук отвязки частоты рендера от частоты симуляции."),
]


def run_worker(item):
    node_id, dll_name, goal = item
    funcs = extract_x87_heavy_exports(dll_name, limit=4)
    prompt = (
        f"Ты — воркер [{node_id}] в рое Claude Haiku 5.5 по реверс-инжинирингу движка GEM 3 (Majesty 2, 32-bit x86 MSVC).\n"
        f"Задача: {goal}\n\n"
        f"Топ-4 горячие функции из `{dll_name}`:\n"
        f"{json.dumps(funcs, ensure_ascii=False, indent=2)}\n\n"
        f"Дай краткий, технически точный C++ декомпилированный код и готовый SSE2/C++ патч."
    )
    env = os.environ.copy()
    env["CC_HAIKU_EFFORT"] = "low"
    t0 = time.time()
    proc = subprocess.run(
        [CC_HAIKU_BIN],
        input=prompt,
        capture_output=True,
        text=True,
        env=env,
        timeout=130,
    )
    elapsed = round(time.time() - t0, 2)
    raw = proc.stdout.strip()
    res_text = raw
    cost = 0.0
    try:
        j = json.loads(raw)
        res_text = j.get("result", raw)
        cost = j.get("total_cost_usd", 0.0)
    except Exception:
        pass
    out_path = os.path.join(SWARM_OUT_DIR, f"{node_id}.md")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(f"# Claude Haiku 5.5 Mega-Swarm — `{node_id}` (`{dll_name}`)\n")
        f.write(f"- **Elapsed:** `{elapsed}s` | **Cost:** `${cost:.5f}`\n\n")
        f.write(res_text + "\n")
    return {
        "node_id": node_id,
        "dll": dll_name,
        "rc": proc.returncode,
        "elapsed_s": elapsed,
        "chars": len(res_text),
        "cost_usd": cost,
    }


def main():
    print("Launching 8-node Claude Haiku 5.5 Mega-Swarm across 8 GEM 3 DLLs...")
    t0 = time.time()
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futs = [pool.submit(run_worker, item) for item in SWARM_NODES]
        for fut in concurrent.futures.as_completed(futs):
            r = fut.result()
            results.append(r)
            print(f"  [DONE] {r['node_id']} ({r['dll']}): rc={r['rc']}, {r['elapsed_s']}s, chars={r['chars']}, cost=${r['cost_usd']:.4f}")
    results.sort(key=lambda x: x["node_id"])
    with open(os.path.join(SWARM_OUT_DIR, "MEGA_SWARM_SUMMARY.json"), "w", encoding="utf-8") as f:
        json.dump({"wall_time_s": round(time.time() - t0, 2), "nodes": results}, f, indent=2)
    print(f"8-node Mega-Swarm finished in {round(time.time() - t0, 2)}s!")


if __name__ == "__main__":
    main()
