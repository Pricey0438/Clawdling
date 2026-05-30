"""Tests for the trace → payload replay."""
import os
from balance_sim.replay import replay_trace, derive_usage_from_jsonl

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE = os.path.join(ROOT, "tools", "balance_sim", "fixtures", "tiny_trace.jsonl")


def test_replay_yields_at_least_one_payload_per_day_with_events():
    payloads = list(replay_trace([FIXTURE], days=4, cadence_sec=300))
    # Two days have events (2026-04-01 and 2026-04-03); each should produce
    # at least one payload with non-zero session_pct.
    days_with_events = {p["wall_day_idx"] for p in payloads if p["wire"]["s"] > 0}
    assert len(days_with_events) >= 2, f"expected ≥2 active days; got {days_with_events}"


def test_replay_payloads_are_chronological():
    payloads = list(replay_trace([FIXTURE], days=4, cadence_sec=300))
    ts = [p["t_sec"] for p in payloads]
    assert ts == sorted(ts), "payloads must be chronological"


def test_derive_usage_returns_session_pct_in_range():
    usage = derive_usage_from_jsonl(FIXTURE, day_idx=0)
    pct = usage.get("session_pct", 0.0)
    assert 0 <= pct <= 100, f"session_pct out of range: {pct}"
