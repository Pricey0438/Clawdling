"""Trace replay: walks ~/.claude/projects/*.jsonl and emits a payload sequence.

Imports daemon/usage_trace.py for compose_wire_payload so the simulator and
production daemon share the same wire shape.
"""
import glob
import json
import os
import sys
from datetime import datetime, timezone

# Make `daemon/` importable so `from usage_trace import ...` works.
_DAEMON_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "daemon"))
if _DAEMON_DIR not in sys.path:
    sys.path.insert(0, _DAEMON_DIR)
from usage_trace import compose_wire_payload   # noqa: E402


def jsonl_paths(roots):
    """Resolve a list of jsonl files from a directory, a single file, or a glob."""
    out = []
    for r in roots:
        if os.path.isdir(r):
            out.extend(sorted(glob.glob(os.path.join(r, "**", "*.jsonl"), recursive=True)))
        elif os.path.isfile(r):
            out.append(r)
        else:
            out.extend(sorted(glob.glob(r)))
    return out


def iter_events(path):
    """Yield (epoch_seconds, raw_event_dict) from a jsonl file."""
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            ts_str = ev.get("timestamp", "")
            if not ts_str:
                continue
            try:
                dt = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
            except ValueError:
                continue
            yield int(dt.replace(tzinfo=dt.tzinfo or timezone.utc).timestamp()), ev


def derive_usage_from_jsonl(path, day_idx):
    """Synthesize a usage dict for a given day.

    Approximation: session_pct = min(100, total_output_tokens_on_day / 1000).
    Real Anthropic ratelimit utilization is opaque and changes per account;
    this synthetic mapping is close enough for relative balance tuning. The
    real-world ratelimit-based pct only matters for the firmware-side XP
    grant, which we replay through the model.
    """
    total = 0
    for ts, ev in iter_events(path):
        # Filter to just this day. Day 0 = first event's day.
        # The caller-supplied day_idx is relative to the trace start.
        usage = ev.get("message", {}).get("usage", {})
        total += int(usage.get("output_tokens", 0)) + int(usage.get("input_tokens", 0))
    pct = min(100.0, total / 1000.0)
    return {
        "session_pct": pct,
        "session_reset_mins": 300,   # nominal 5h window
        "weekly_pct": min(100.0, total / 50_000.0),
        "weekly_reset_mins": 7 * 24 * 60,
        "status": "ok",
    }


def replay_trace(roots, days=30, cadence_sec=300):
    """Yield payload dicts {t_sec, wall_day_idx, wire} chronologically for `days` days.

    cadence_sec is the time step between emitted payloads. Default 300 (5min);
    use the firmware's actual 5s only for very small day windows or you'll
    spend the whole simulation in loop overhead.
    """
    files = jsonl_paths(roots)
    if not files:
        return
    # Collect all events, sort by timestamp, group by day.
    events = []
    for p in files:
        events.extend(iter_events(p))
    if not events:
        return
    events.sort(key=lambda x: x[0])
    t0 = events[0][0]
    # Day-bucket events.
    by_day = {}
    for ts, ev in events:
        day = (ts - t0) // 86400
        by_day.setdefault(day, []).append((ts, ev))
    # Compute session_pct on a rolling 5h window (mirrors Anthropic's actual
    # 5h ratelimit window — utilization is "tokens consumed in the last 5h" not
    # cumulative-since-day-start). Scale so a casual user (~2-3k tokens/hr at
    # peak engagement) lands at 30-50% util, and a multi-hour heavy session
    # saturates at 100%.
    SESSION_WINDOW_SEC = 5 * 3600
    SESSION_TOKEN_SCALE = 8000   # 8k tokens-in-5h-window → 100%
    WEEKLY_WINDOW_SEC = 7 * 86400
    WEEKLY_TOKEN_SCALE = 250_000   # ~250k tokens-in-7d → 100%

    # All events as a flat list with (t, tokens) for sliding-window math.
    flat_events = []
    for day_evs in by_day.values():
        for ts, ev in day_evs:
            u = ev.get("message", {}).get("usage", {})
            tok = int(u.get("output_tokens", 0)) + int(u.get("input_tokens", 0))
            flat_events.append((ts, tok))
    flat_events.sort()

    def tokens_in_window(t, window_sec):
        cutoff = t - window_sec
        return sum(tok for ts, tok in flat_events if cutoff <= ts <= t)

    for day in range(days):
        day_start = t0 + day * 86400
        for t in range(day_start, day_start + 86400, cadence_sec):
            s_tokens = tokens_in_window(t, SESSION_WINDOW_SEC)
            w_tokens = tokens_in_window(t, WEEKLY_WINDOW_SEC)
            s_pct = min(100.0, s_tokens / SESSION_TOKEN_SCALE * 100.0)
            w_pct = min(100.0, w_tokens / WEEKLY_TOKEN_SCALE * 100.0)
            wire = compose_wire_payload(
                ts=t, tz_off=0,
                usage={
                    "session_pct": s_pct,
                    "session_reset_mins": 300,
                    "weekly_pct": w_pct,
                    "weekly_reset_mins": 7 * 24 * 60,
                    "status": "ok",
                },
                sessions=[],
            )
            yield {"t_sec": t, "wall_day_idx": day, "wire": wire}
