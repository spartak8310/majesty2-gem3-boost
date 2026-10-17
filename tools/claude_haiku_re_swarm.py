#!/usr/bin/env python3
"""
Parallel Claude Haiku Swarm (via $HOME/.local/bin/cc-haiku-swarm -> beyond-server Claude Max)
Decompiles GEM 3 Engine x86 hotspots from Majesty 2 (base.dll, render.dll, pf.dll, scene.dll)
and synthesizes C++ hook/patch implementations for single-core CPU optimization.
"""
import concurrent.futures
import json
import os
import subprocess
import time

RE_DIR = "./scratch/majesty2-re"
HOTSPOTS_JSON = os.path.join(RE_DIR, "sdk/hotspots_asm.json")
SWARM_OUT_DIR = os.path.join(RE_DIR, "decompiled/claude_swarm")
CC_HAIKU_BIN = "$HOME/.local/bin/cc-haiku-swarm"

os.makedirs(SWARM_OUT_DIR, exist_ok=True)

with open(HOTSPOTS_JSON, "r", encoding="utf-8") as f:
    hotspots = json.load(f)

TASKS = {
    "node1_base_heap": {
        "dll": "base.dll",
        "goal": (
            "Проанализируй и декомпилируй в чистый C++ (32-bit MSVC __thiscall/__cdecl) дизассемблированные функции "
            "`xBase::eHeap` и `xBase::eFastTimer` из `base.dll` (движок GEM 3, игра Majesty 2). "
            "Напиши готовый C++ код перехвата (inline hook / IAT patch) для Proxy DLL (`version.dll` / `d3dx9_38.dll`), "
            "который заменяет аллокатор `eHeap` и `MSVCR80.dll` (`malloc`/`free`/`operator new`) на lock-free пул / `mimalloc`."
        ),
    },
    "node2_render_occlusion": {
        "dll": "render.dll",
        "goal": (
            "Проанализируй и декомпилируй в чистый C++ дизассемблированные функции `xRender::xOcclusion::eDetector` "
            "и рендер-пайплайна из `render.dll` (движок GEM 3, игра Majesty 2). "
            "Объясни, где именно происходит блокирующее ожидание D3D9 Occlusion Query на главном потоке CPU, "
            "и напиши C++ хук, который устраняет синхронный CPU-GPU stall."
        ),
    },
    "node3_pf_particles": {
        "dll": "pf.dll",
        "goal": (
            "Проанализируй и декомпилируй в чистый C++ функции `xParticleFlow` из `pf.dll` (система частиц GEM 3, Majesty 2). "
            "Напиши C++ хук для троттлинга / отсечения (frustum & distance culling) невидимых партиклов и ограничения "
            "числа активных эмиттеров в кадре, чтобы магия десятков героев не вешала главный поток."
        ),
    },
    "node4_scene_pather": {
        "dll": "scene.dll",
        "goal": (
            "Проанализируй и декомпилируй в чистый C++ функции `xPather` из `scene.dll` (поиск пути A* и навигация GEM 3, Majesty 2). "
            "Напиши C++ хук для Time-Slicing (квотирования числа пересчётов маршрута за один квант симуляции) и кэширования путей."
        ),
    },
}


def run_haiku_worker(node_id: str, cfg: dict) -> dict:
    dll = cfg["dll"]
    funcs = hotspots.get(dll, [])
    prompt = (
        f"Ты — эксперт по реверс-инжинирингу x86 PE32 (MSVC 2005, C++ __thiscall) в составе Claude Haiku Swarm.\n"
        f"Задача узла [{node_id}] ({dll}):\n{cfg['goal']}\n\n"
        f"Дизассемблированные экспортируемые функции из `{dll}`:\n"
        f"```json\n{json.dumps(funcs, ensure_ascii=False, indent=2)}\n```\n\n"
        f"Верни подробный технический разбор на русском языке с восстановленным псевдокодом C++ каждой функции и готовым кодом C++ хука."
    )
    t0 = time.time()
    proc = subprocess.run(
        [CC_HAIKU_BIN],
        input=prompt,
        capture_output=True,
        text=True,
        timeout=140,
    )
    elapsed = round(time.time() - t0, 2)
    raw = proc.stdout.strip()
    result_text = raw
    cost_usd = 0.0
    model_used = "claude-haiku"
    try:
        parsed = json.loads(raw)
        result_text = parsed.get("result", raw)
        cost_usd = parsed.get("total_cost_usd", 0.0)
        if parsed.get("modelUsage"):
            model_used = ", ".join(parsed["modelUsage"].keys())
    except Exception:
        pass

    md_path = os.path.join(SWARM_OUT_DIR, f"{node_id}.md")
    with open(md_path, "w", encoding="utf-8") as out:
        out.write(f"# Claude Haiku Swarm — `{node_id}` (`{dll}`)\n")
        out.write(f"- **Model:** `{model_used}`\n")
        out.write(f"- **Elapsed:** `{elapsed}s` | **Cost:** `${cost_usd:.5f}`\n\n")
        out.write(result_text + "\n")

    return {
        "node_id": node_id,
        "dll": dll,
        "returncode": proc.returncode,
        "elapsed_s": elapsed,
        "cost_usd": cost_usd,
        "model": model_used,
        "md_path": md_path,
        "chars": len(result_text),
        "stderr": proc.stderr.strip()[:300] if proc.returncode != 0 else "",
    }


def main():
    print(f"Launching 4-node Claude Haiku Swarm via {CC_HAIKU_BIN}...")
    t0 = time.time()
    summary = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futs = {
            pool.submit(run_haiku_worker, node_id, cfg): node_id
            for node_id, cfg in TASKS.items()
        }
        for fut in concurrent.futures.as_completed(futs):
            res = fut.result()
            summary.append(res)
            print(
                f"  [DONE] {res['node_id']} ({res['dll']}): rc={res['returncode']}, "
                f"{res['elapsed_s']}s, model={res['model']}, chars={res['chars']}"
            )

    summary.sort(key=lambda x: x["node_id"])
    report_path = os.path.join(SWARM_OUT_DIR, "SWARM_SUMMARY.json")
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(
            {
                "total_wall_time_s": round(time.time() - t0, 2),
                "nodes": summary,
            },
            f,
            indent=2,
            ensure_ascii=False,
        )
    print(f"All 4 Claude Haiku Swarm nodes completed in {round(time.time() - t0, 2)}s!")


if __name__ == "__main__":
    main()
