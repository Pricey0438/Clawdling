#!/usr/bin/env python3
"""Golden-trace baseline: sim output on the synthetic 30-day trace must hash to a
stable value. Any YAML edit that changes balance will change this hash, surfacing
the impact in PR diffs as a visible noise signal.

Update protocol when balance.yaml is intentionally tuned:
  1. Run this test, copy the printed actual_hash, paste into EXPECTED_HASH below.
  2. Commit the new hash + the YAML change in the same commit so the audit trail
     is one click away.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SIM = os.path.join(ROOT, "tools", "balance_sim", "sim.py")
YAML = os.path.join(ROOT, "firmware", "balance.yaml")
FIXTURE = os.path.join(ROOT, "tools", "balance_sim", "fixtures", "synthetic_casual_30d.jsonl")

# Updated each time balance.yaml is intentionally tuned. Set to "" to force a
# fresh capture on first run.
EXPECTED_HASH = "2510054e3505cf5a"


def main():
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out_path = tmp.name
    try:
        env = os.environ.copy()
        env["PYTHONPATH"] = os.path.join(ROOT, "tools") + os.pathsep + os.path.join(ROOT, "daemon")
        proc = subprocess.run(
            [sys.executable, SIM,
             "--trace", FIXTURE, "--balance", YAML,
             "--days", "30", "--seed", "42",
             "--out", out_path],
            capture_output=True, text=True, env=env,
        )
        assert proc.returncode == 0, f"sim failed: {proc.stderr}"
        with open(out_path) as f:
            records = json.load(f)
        digest_input = json.dumps(
            [{k: r[k] for k in ("day", "level", "xp", "total_xp", "satiety", "spirit", "bond")}
             for r in records],
            sort_keys=True,
        )
        actual_hash = hashlib.sha256(digest_input.encode()).hexdigest()[:16]
        if not EXPECTED_HASH:
            print(f"golden: first-run capture — actual_hash={actual_hash}")
            print("→ set EXPECTED_HASH in this file to the value above and re-run.")
            sys.exit(1)
        if actual_hash != EXPECTED_HASH:
            print(f"GOLDEN FAILED — sim output changed.", file=sys.stderr)
            print(f"  expected: {EXPECTED_HASH}", file=sys.stderr)
            print(f"  actual:   {actual_hash}", file=sys.stderr)
            print("If this change was intentional (balance.yaml tuned), update", file=sys.stderr)
            print("EXPECTED_HASH in tools/balance_sim/test_golden.py.", file=sys.stderr)
            sys.exit(1)
        print(f"golden ok (hash={actual_hash})")
    finally:
        os.unlink(out_path)


if __name__ == "__main__":
    main()
