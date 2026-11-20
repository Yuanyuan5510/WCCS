#!/usr/bin/env python3
"""Cross-language consistency runner (work.md sections 5.2 / 5.3).

Every binding emits a line ``indices: <100 ints>`` for the pinned vector.
This script hashes those 100 bytes and compares against the C reference
digest stored in tests/vectors/v1.txt.

Usage:  python tests/run_cross_lang.py [--only C,Python] [--allow-missing]
Exit 0 only when every language matches (section 5: "must be all green").

SPDX-License-Identifier: Apache-2.0
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VECTOR_FILE = ROOT / "tests" / "vectors" / "v1.txt"
INDEX_RE = re.compile(r"^\s*indices:\s*([0-9]+(?:\s+[0-9]+)*)\s*$", re.M)


def expected_digest() -> str:
    m = re.search(r"^sha256:\s*([0-9a-f]{64})\s*$", VECTOR_FILE.read_text(), re.M)
    if not m:
        raise SystemExit(f"Cannot read expected digest from {VECTOR_FILE}")
    return m.group(1)


def find_library() -> Path | None:
    dirs = (ROOT / "build", ROOT / "build" / "Release", ROOT / "build" / "Debug")
    for d in dirs:
        for name in ("libwccs.dll", "wccs.dll", "libwccs.so", "libwccs.dylib"):
            p = d / name
            if p.exists():
                return p
    return None


def find_exe(stem: str) -> Path | None:
    dirs = (ROOT / "build", ROOT / "build" / "Release", ROOT / "build" / "Debug")
    for d in dirs:
        for suffix in (".exe", ""):
            p = d / (stem + suffix)
            if p.exists():
                return p
    return None


def has(cmd: str) -> bool:
    return shutil.which(cmd) is not None


def languages():
    """(name, command, cwd, needs_external_tool)"""
    return [
        ("C",          [find_exe("wccs_test_vector")], ROOT, False),
        ("C++",        [find_exe("wccs_cpp_vector")], ROOT, False),
        ("Rust",       ["cargo", "run", "--quiet", "--example", "vector"],
         ROOT / "bindings" / "rust", True),
        ("Python",     [sys.executable, "tests/vector.py"],
         ROOT / "bindings" / "python", False),
        ("C#",         ["dotnet", "run", "-c", "Release", "--verbosity", "quiet"],
         ROOT / "bindings" / "csharp" / "Wccs.Tests", True),
        ("Node(CJS)",  ["node", "test/vector.cjs"], ROOT / "bindings" / "node", True),
        ("Node(ESM)",  ["node", "test/vector.mjs"], ROOT / "bindings" / "node", True),
    ]


def build_env() -> dict:
    env = os.environ.copy()
    lib = find_library()
    if lib:
        env["WCCS_LIBRARY_PATH"] = str(lib)
    py = str(ROOT / "bindings" / "python")
    env["PYTHONPATH"] = py + os.pathsep + env.get("PYTHONPATH", "")
    return env


def run_one(name: str, cmd, cwd: Path, external: bool, env: dict, allow_missing: bool):
    if cmd is None or cmd[0] is None:
        return "MISSING", "-", f"{name}: binary not built (run cmake --build build)"
    if isinstance(cmd[0], Path):
        cmd = [str(cmd[0])]
    if external and not has(cmd[0]):
        return "MISSING", "-", f"{name}: tool '{cmd[0]}' not found"
    try:
        p = subprocess.run(cmd, cwd=str(cwd), env=env, capture_output=True,
                           text=True, timeout=300)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return "ERROR", "-", f"{name}: {exc}"
    combined = p.stdout + "\n" + p.stderr
    m = INDEX_RE.search(combined)
    if not m:
        tail = combined.strip().splitlines()[-6:]
        return "ERROR", "-", f"{name}: no vector line (rc={p.returncode}): " + " | ".join(tail)
    values = [int(x) for x in m.group(1).split()]
    if len(values) != 100 or any(v < 0 or v > 255 for v in values):
        return "ERROR", "-", f"{name}: expected 100 uint8 indices, got {len(values)}"
    digest = hashlib.sha256(bytes(values)).hexdigest()
    return "OK" if p.returncode == 0 else "ERROR", digest, ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="comma-separated language names to run")
    ap.add_argument("--allow-missing", action="store_true",
                    help="treat missing tools/binaries as skipped instead of failures")
    args = ap.parse_args()

    expected = expected_digest()
    env = build_env()
    selected = set(args.only.split(",")) if args.only else None

    print(f"Expected SHA256: {expected}")
    print(f"Library:         {env.get('WCCS_LIBRARY_PATH', '(system path)')}")
    print("-" * 78)
    print(f"{'LANGUAGE':<12} {'STATUS':<9} SHA256")
    print("-" * 78)

    failures = []
    for name, cmd, cwd, external in languages():
        if selected and name not in selected:
            continue
        status, digest, detail = run_one(name, cmd, cwd, external, env,
                                         args.allow_missing)
        shown = digest if digest != "-" else "-"
        ok = (status == "OK" and digest == expected)
        if status == "MISSING" and args.allow_missing:
            print(f"{name:<12} {'SKIP':<9} {detail}")
            continue
        print(f"{name:<12} {'OK' if ok else status:<9} {shown}")
        if not ok:
            reason = detail
            if not reason:
                reason = ("non-zero exit status (vector still matched)"
                          if digest == expected else f"digest mismatch ({shown})")
            failures.append(f"{name}: {reason}")

    print("-" * 78)
    if failures:
        for f in failures:
            print("FAIL " + f)
        return 1
    print("ALL LANGUAGES MATCH THE C REFERENCE VECTOR")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
