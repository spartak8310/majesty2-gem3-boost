import sys, json
from pathlib import Path
sys.path.insert(0, str(Path.home()/ "mnt/SATRAP ECHO/tools"))
import metaclaw_project_factory as f
decs=json.load(open(str(Path.home()/"scr/decisions.json")))
sup={x for d in decs for x in d.get("supersedes",[])}
decs=[d for d in decs if d["record_id"] not in sup]
telemetry={
 "3D Main Menu FPS":"60 FPS (16.5 ms) [menu; carried over, NOT re-measured]",
 "Day 230 Heavy Bench, 1x":"2.96-3.7 fps, quant 175-260 ms [verified, no env, n=4]",
 "Day 230 Heavy Bench, 5x":"1.7-1.8 fps, quant 414-445 ms [verified, no env]",
 "Fast A* (opt-in M2_ASTAR=1)":"3.6 fps, quant 143 ms; NOT result-equivalent",
 "Engine Level Load":"~110 s (menu 31 s); first 2 quants ~5.4 s each",
 "x87 share of EIP samples":"78% (Quant 82%); Rosetta x87 9-143 ns/insn vs SSE2 ~1 ns",
 "Pather L1-L3 (SSE2 target)":"~48% of samples; bit-exact SSE2 ok (CW 0x027F, MXCSR 0x1FA0)",
 "WINEMSYNC A/B (1x, n=1 each)":"msync=1 3.29 fps / load 108 s; msync=0 3.52 fps / 110 s; no gain, no hang [verified]",
 "Golden DLL":"d3d9_safe.dll md5 f810f846; distributor/arena/zone/pacing/A* opt-in",
 "Clore Build Latency":"0.41s tmpfs, Ryzen 5950X [carried over, NOT re-measured]",
 "Wine Prefix":"/Applications/Gamez/Majesty 2.app/.../syswow64",
}
inv=[
 "M3 Pro 5 P-cores: 4 workers + 1 caller = 5 threads max [policy, owner rule]",
 "WINEMSYNC=0/WINEESYNC=0 stays default (Info.plist); A/B shows no gain from =1 [measured n=1]",
 "Fast_QuatToMat3x4 / Fast_Box3Union hooks stay disabled [from earlier mesh-corruption incident]",
 "PE32 builds only on Clore (satrap-clore-exec), never on the Mac [policy]",
 "Multi-step work goes to the m2-* squad; Mac runs only the verified bench gate [policy]",
 "A win = Mac run with JSON+log, repeated >=2x, no env hacks; else quarantined [rule]",
]
home=Path.home()/ "mnt/majesty2-re"
f.render_project_optical_sheet(slug="majesty-claw",title="Majesty 2 Collection Reverse Engineering & Decoupled Engine MetaClaw",invariants=inv,telemetry=telemetry,decisions=decs,out_png=home/"OPTICAL_SHEET_MAJESTY.png",out_jpg=home/"OPTICAL_SHEET_MAJESTY.jpg")
print("ok", len(decs))
