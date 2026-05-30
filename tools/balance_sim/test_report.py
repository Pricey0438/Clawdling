"""Tests for the target-bands report."""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REPORT = os.path.join(ROOT, "tools", "balance_sim", "report.py")
TARGETS = os.path.join(ROOT, "tools", "balance_sim", "targets.yaml")


def fake_sim_output_all_green():
    """Build a sim record set that lands inside every band."""
    records = []
    for d in range(30):
        records.append({
            "day": d,
            "level": min(30, 1 + d),
            "xp": 0,
            "total_xp": d * 1000,
            "satiety": 60,
            "spirit": 60,
            "bond": 60,
            "is_shiny": False,
            "feed_count": d,
            "play_count": d,
        })
    return records


def run_report(records):
    with tempfile.NamedTemporaryFile(suffix=".json", mode="w", delete=False) as tmp:
        json.dump(records, tmp)
        sim_path = tmp.name
    try:
        env = os.environ.copy()
        env["PYTHONPATH"] = os.path.join(ROOT, "tools")
        proc = subprocess.run(
            [sys.executable, REPORT, sim_path, "--targets", TARGETS, "--hours-per-day", "1.5"],
            capture_output=True, text=True, env=env,
        )
        return proc
    finally:
        os.unlink(sim_path)


def test_report_exits_nonzero_when_any_band_red():
    records = [{"day": d, "level": 1, "xp": 0, "total_xp": 0,
                "satiety": 0, "spirit": 0, "bond": 0, "is_shiny": False,
                "feed_count": 0, "play_count": 0} for d in range(30)]
    proc = run_report(records)
    assert proc.returncode == 1, f"expected exit 1 on red bands; got {proc.returncode}\n{proc.stdout}"


def test_report_prints_dimension_lines():
    records = fake_sim_output_all_green()
    proc = run_report(records)
    for dim in ["time_to_l10", "time_to_l4", "time_to_l30",
                "weekly_quest_completion_rate", "shiny_drop_rate",
                "care_stat_daily_dip"]:
        assert dim in proc.stdout, f"missing dimension {dim} in report:\n{proc.stdout}"
