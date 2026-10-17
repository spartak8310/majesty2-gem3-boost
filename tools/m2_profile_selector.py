#!/usr/bin/env python3
"""
Native macOS / Cross-Platform Replacement for:
  - Majesty2Selector.exe
  - Change profile name.exe
=====================================================================
Reads and writes `profiles.list` using Windows-1251 (cp1251) encoding
(fixing the UTF-8 corruption bug in Majesty2Selector.exe while keeping
its safe atomic backup to `profiles.old`).
"""

import argparse
import os
import re
import shutil
import sys


DEFAULT_CANDIDATE_PATHS = [
    "$HOME/Documents/Majesty2_Archive/Documents_majesty2/profiles/profiles.list",
    "$HOME/Documents/Majesty2_Archive/Desktop_majesty2/profiles/profiles.list",
    os.path.expanduser("~/Documents/majesty2/profiles/profiles.list"),
]


def load_profiles(path: str):
    raw = open(path, "rb").read().decode("cp1251", errors="replace")
    names = {0: "", 1: "", 2: "", 3: ""}
    active = 0
    for line in raw.splitlines():
        m_prof = re.search(r'\{\s*profile\s+([0-3])\s+"(.*)"\s*\}', line)
        if m_prof:
            names[int(m_prof.group(1))] = m_prof.group(2)
        m_act = re.search(r"\{\s*active\s+([0-3])\s*\}", line)
        if m_act:
            active = int(m_act.group(1))
    return names, active


def save_profiles(path: str, names: dict, active: int):
    old_path = os.path.join(os.path.dirname(path), "profiles.old")
    if os.path.exists(path):
        shutil.copy2(path, old_path)

    lines = []
    for i in range(4):
        lines.append(f'{{profile {i} "{names.get(i, "")}"}}\r\n')
    lines.append(f"{{active {active}}}\r\n")
    payload = "".join(lines).encode("cp1251")
    with open(path, "wb") as f:
        f.write(payload)


def main():
    parser = argparse.ArgumentParser(description="Majesty 2 Native macOS Profile Selector & Name Editor (CP1251)")
    parser.add_argument("--file", help="Path to profiles.list")
    parser.add_argument("--active", type=int, choices=[0, 1, 2, 3], help="Set active profile index (0..3)")
    parser.add_argument("--set-name", nargs=2, metavar=("INDEX", "NAME"), help="Set profile name for index 0..3")
    args = parser.parse_args()

    target = args.file
    if not target:
        for cand in DEFAULT_CANDIDATE_PATHS:
            if os.path.exists(cand):
                target = cand
                break
    if not target or not os.path.exists(target):
        print("[!] Could not find profiles.list. Specify --file <path>", file=sys.stderr)
        sys.exit(1)

    names, active = load_profiles(target)
    modified = False

    if args.set_name:
        idx = int(args.set_name[0])
        if idx not in (0, 1, 2, 3):
            raise ValueError("Profile index must be 0..3")
        names[idx] = args.set_name[1]
        modified = True

    if args.active is not None:
        active = args.active
        modified = True

    if modified:
        save_profiles(target, names, active)
        print(f"[+] Updated {target} (CP1251, backup at profiles.old)")

    print(f"=== Majesty 2 Profiles ({target}) ===")
    for i in range(4):
        marker = "*" if i == active else " "
        print(f" [{marker}] Profile {i}: {names[i]!r}")


if __name__ == "__main__":
    main()
