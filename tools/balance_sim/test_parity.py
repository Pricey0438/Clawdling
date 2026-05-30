#!/usr/bin/env python3
"""Parity test: Python pet_model.xp_to_next must match the firmware exactly.

Run the codegen, parse LEVEL_CURVE_BASE / LEVEL_CURVE_EXP_BASE / PET_MAX_LEVEL
from the generated header, compute xp_to_next(L) in Python using the same
formula firmware uses. Compare to the Python model's xp_to_next for L=1..max-1.

Failure prints both values per level and asserts equality.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GEN_SCRIPT = os.path.join(ROOT, "firmware", "scripts", "gen_balance_h.py")
YAML = os.path.join(ROOT, "firmware", "balance.yaml")
HEADER = os.path.join(ROOT, "firmware", "src", "pet_balance_generated.h")
sys.path.insert(0, os.path.join(ROOT, "tools"))
from balance_sim.pet_model import PetModel, load_balance   # noqa: E402


def regenerate_header():
    proc = subprocess.run(
        [sys.executable, GEN_SCRIPT, "--yaml", YAML, "--out", HEADER],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, f"codegen failed: {proc.stderr}"


def parse_define(name, content):
    m = re.search(rf"^#define\s+{re.escape(name)}\s+(\S+)\s*$", content, re.M)
    assert m, f"missing #define {name}"
    return m.group(1)


def main():
    regenerate_header()
    with open(HEADER) as f:
        content = f.read()
    base = int(parse_define("LEVEL_CURVE_BASE", content))
    exp_base = float(parse_define("LEVEL_CURVE_EXP_BASE", content))
    max_level = int(parse_define("PET_MAX_LEVEL", content))

    model = PetModel(load_balance(YAML))
    failures = []
    for L in range(1, max_level):
        firmware_val = int(base * L * (exp_base ** L))
        sim_val = model.xp_to_next(L)
        if firmware_val != sim_val:
            failures.append((L, firmware_val, sim_val))

    if failures:
        print("PARITY FAILURES:", file=sys.stderr)
        for L, fw, sim in failures:
            print(f"  L={L}: firmware={fw} sim={sim}", file=sys.stderr)
        sys.exit(1)
    print(f"parity ok ({max_level - 1} levels matched)")


if __name__ == "__main__":
    main()
