"""Smoke test: run gen_balance_h.py on the real balance.yaml and check
the generated header contains the expected #defines with the right values."""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "firmware", "scripts", "gen_balance_h.py")
YAML = os.path.join(ROOT, "firmware", "balance.yaml")
OUT = os.path.join(ROOT, "firmware", "src", "pet_balance_generated.h")


def main():
    # Run in standalone mode (no SCons env) — the script must support this for testing.
    proc = subprocess.run(
        [sys.executable, SCRIPT, "--yaml", YAML, "--out", OUT],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, f"codegen failed: {proc.stderr}"
    with open(OUT) as f:
        content = f.read()
    required = [
        "#define XP_PER_SESSION_CTX_PCT 5",
        "#define XP_PER_5H_PCT 10",
        "#define XP_PER_WEEKLY_PCT 30",
        "#define LEVEL_CURVE_BASE 200",
        "#define LEVEL_CURVE_EXP_BASE 1.05",
        "#define PET_MAX_LEVEL 30",
        "#define STAT_DECAY_INTERVAL 480",
        "#define FORGIVENESS_TRIGGER_NO_USAGE_SEC 86400",
        "#define FORGIVENESS_SLOWDOWN_FACTOR 4",
        "#define SHINY_DROP_RATE_DENOM 64",
    ]
    missing = [r for r in required if r not in content]
    assert not missing, f"missing defines:\n  " + "\n  ".join(missing)
    print("ok")


if __name__ == "__main__":
    main()
