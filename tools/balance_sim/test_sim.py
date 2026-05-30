"""End-to-end sim test using the tiny fixture trace."""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SIM = os.path.join(ROOT, "tools", "balance_sim", "sim.py")
FIXTURE = os.path.join(ROOT, "tools", "balance_sim", "fixtures", "tiny_trace.jsonl")
YAML = os.path.join(ROOT, "firmware", "balance.yaml")


def test_sim_produces_one_record_per_day():
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out_path = tmp.name
    try:
        env = os.environ.copy()
        env["PYTHONPATH"] = os.path.join(ROOT, "tools") + os.pathsep + os.path.join(ROOT, "daemon")
        proc = subprocess.run(
            [sys.executable, SIM,
             "--trace", FIXTURE,
             "--balance", YAML,
             "--days", "4",
             "--out", out_path],
            capture_output=True, text=True, env=env,
        )
        assert proc.returncode == 0, f"sim failed: {proc.stderr}"
        with open(out_path) as f:
            records = json.load(f)
        assert len(records) == 4, f"expected 4 day records, got {len(records)}"
        required = {"day", "level", "xp", "total_xp", "satiety", "spirit", "bond"}
        for r in records:
            missing = required - set(r.keys())
            assert not missing, f"day {r.get('day')} missing keys: {missing}"
        # Satiety decays at -1 per 480s; even after the day-0 feed it should
        # land well below 100 by end-of-day-0 (180 ticks/day with no forgiveness
        # while XP is actively granting). Days 1-2 have no usage → no feed, so
        # satiety stays at/below the day-0 floor.
        assert records[0]["satiety"] < 100, "satiety should decay within day 0"
        assert records[2]["satiety"] <= records[0]["satiety"], "satiety should not recover across gap days"
    finally:
        os.unlink(out_path)


def test_sim_accumulates_total_xp():
    """The fixture trace has tokens; sim must award nonzero total_xp."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out_path = tmp.name
    try:
        env = os.environ.copy()
        env["PYTHONPATH"] = os.path.join(ROOT, "tools") + os.pathsep + os.path.join(ROOT, "daemon")
        proc = subprocess.run(
            [sys.executable, SIM,
             "--trace", FIXTURE,
             "--balance", YAML,
             "--days", "4",
             "--out", out_path],
            capture_output=True, text=True, env=env,
        )
        assert proc.returncode == 0, f"sim failed: {proc.stderr}"
        with open(out_path) as f:
            records = json.load(f)
        # Final-day total_xp should be > 0; the fixture has ~1325 tokens of
        # output across two active days, synthetic session_pct rises monotonically,
        # so XP grants should fire.
        final_total = records[-1]["total_xp"]
        assert final_total > 0, f"expected total_xp > 0; got {final_total}"
    finally:
        os.unlink(out_path)
