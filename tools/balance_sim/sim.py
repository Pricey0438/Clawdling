#!/usr/bin/env python3
"""Pet balance simulator entry point.

Reads firmware/balance.yaml, replays a usage trace, projects pet state day
by day, writes a JSON record per simulated day.

Example:
  python3 tools/balance_sim/sim.py \
    --trace ~/.claude/projects/ --balance firmware/balance.yaml \
    --days 30 --out out/sim_30d.json
"""
import argparse
import json
import os
import random
import sys
from itertools import chain

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
sys.path.insert(0, os.path.join(_ROOT, "tools"))
sys.path.insert(0, os.path.join(_ROOT, "daemon"))

from balance_sim.pet_model import PetModel, load_balance   # noqa: E402
from balance_sim.replay import replay_trace                # noqa: E402


def diffs_to_xp(prev, curr, bal):
    """Mirror firmware pet_on_metrics: per-pct XP grants per delta."""
    xp = 0
    ds = max(0.0, curr["s"] - prev["s"])
    xp += int(ds * bal["xp"]["per_5h_pct"])   # firmware's 5h sense
    dw = max(0.0, curr["w"] - prev["w"])
    xp += int(dw * bal["xp"]["per_weekly_pct"])
    # session_ctx_pct is per-session info; not synthesized in replay, so 0 here.
    return xp


def run_simulation(trace_paths, balance_path, days, cadence_sec, rng_seed=42):
    bal = load_balance(balance_path)
    model = PetModel(bal)
    rng = random.Random(rng_seed)
    if model.roll_shiny(rng):
        model.pet.is_shiny = True

    prev = {"s": 0.0, "w": 0.0}
    daily_records = []
    last_day = -1
    actions_today = False   # have we already fired feed/play/pet this day?

    # Sim driver responsibility (per pet_model.py docstring): rebase decay
    # anchors at boot so the first tick_decay doesn't catch up from epoch 0.
    payload_iter = replay_trace(trace_paths, days=days, cadence_sec=cadence_sec)
    try:
        first_payload = next(payload_iter)
    except StopIteration:
        return []   # empty trace → no records
    sim_start_t = first_payload["t_sec"]
    model.satiety_decay_ts = sim_start_t
    model.spirit_decay_ts  = sim_start_t
    model.bond_decay_ts    = sim_start_t
    model.last_xp_event_sec = sim_start_t   # so forgiveness doesn't trigger immediately

    # Chain the first payload back in for the main loop.
    payload_stream = chain([first_payload], payload_iter)

    for payload in payload_stream:
        t = payload["t_sec"]
        day = payload["wall_day_idx"]
        wire = payload["wire"]

        # Detect day rollover → snapshot previous day and reset the action latch.
        if day != last_day:
            if last_day >= 0:
                daily_records.append(snapshot_day(model, last_day))
            last_day = day
            actions_today = False

        # Tick decay every payload, granting XP for any session/weekly delta.
        model.tick_decay(now_sec=t)
        xp = diffs_to_xp(prev, wire, bal)
        if xp > 0:
            model.grant_xp(xp, now_sec=t)

        # "Engaged user" care model: on the first payload of any day where
        # the user has actual usage (s > 0), fire all three care actions.
        # This mirrors a casual user who picks up the device once per day
        # and gives their pet a few taps. Without this, decay catches up
        # before any user touch, stats reach 0, and XP multiplier zeroes out.
        if wire["s"] > 0 and not actions_today:
            model.feed(now_sec=t)
            model.play(now_sec=t)
            model.pet_action(now_sec=t)
            actions_today = True

        prev = {"s": wire["s"], "w": wire["w"]}

    # Final day snapshot.
    if last_day >= 0:
        daily_records.append(snapshot_day(model, last_day))

    # Pad to `days` if the trace was shorter (fill with last state).
    while len(daily_records) < days:
        if daily_records:
            tail = dict(daily_records[-1])
            tail["day"] = len(daily_records)
            daily_records.append(tail)
        else:
            daily_records.append(snapshot_day(model, len(daily_records)))

    return daily_records


def snapshot_day(model, day):
    return {
        "day": day,
        "level": model.pet.level,
        "xp": model.pet.xp,
        "total_xp": model.pet.total_xp_earned,
        "satiety": model.pet.satiety,
        "spirit": model.pet.spirit,
        "bond": model.pet.bond,
        "is_shiny": model.pet.is_shiny,
        "feed_count": model.feed_count,
        "play_count": model.play_count,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", action="append", required=True,
                    help="path to ~/.claude/projects/ or a specific .jsonl (repeatable)")
    ap.add_argument("--balance", default=os.path.join(_ROOT, "firmware", "balance.yaml"))
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--cadence-sec", type=int, default=300)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    records = run_simulation(
        args.trace, args.balance, args.days, args.cadence_sec, args.seed,
    )
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(records, f, indent=2)
    print(f"sim: wrote {args.out} ({len(records)} day records)")


if __name__ == "__main__":
    main()
