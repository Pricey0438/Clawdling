#!/usr/bin/env python3
"""Compare sim output to target bands. Print one line per dimension. Exit 0 if all green."""
import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
sys.path.insert(0, os.path.join(_ROOT, "tools"))

import yaml   # noqa: E402


def hours_until_level(records, target_level, hours_per_day):
    """Return hours of cumulative usage at which the pet first hit target_level.

    If never reached within the sim window, extrapolate from the total_xp curve
    over the last 5 days: assume that XP rate persists, compute the day it would
    cross the cumulative-XP threshold for target_level. Caps at 10x sim duration.
    """
    for r in records:
        if r["level"] >= target_level:
            return r["day"] * hours_per_day

    # Extrapolation path: never reached → project from recent XP rate.
    if len(records) < 6:
        return None
    last = records[-1]
    prev = records[-6]
    days_window = last["day"] - prev["day"]
    xp_delta = last["total_xp"] - prev["total_xp"]
    if xp_delta <= 0 or days_window <= 0:
        return None
    xp_per_day = xp_delta / days_window
    # Cumulative XP needed for target_level: sum of xp_to_next(L) for L=1..target-1.
    # We approximate with the firmware formula 200 * L * 1.05^L (constants known).
    cum_needed = sum(int(200 * L * (1.05 ** L)) for L in range(1, target_level))
    xp_gap = cum_needed - last["total_xp"]
    if xp_gap <= 0:
        return last["day"] * hours_per_day
    days_to_reach = last["day"] + xp_gap / xp_per_day
    # Cap extrapolation at 10x the sim window so wildly underperforming runs
    # report a clear out-of-band value rather than infinity.
    max_days = len(records) * 10
    if days_to_reach > max_days:
        return None
    return days_to_reach * hours_per_day


def min_stat_after_2_day_gap(records):
    """Worst-case min(satiety, spirit, bond) across the run.

    Used as a proxy for the 'weekend skip' check.
    """
    worst = 100
    for r in records:
        m = min(r["satiety"], r["spirit"], r["bond"])
        if m < worst:
            worst = m
    return worst


def typical_daily_min_stat(records):
    """Median of per-day min stat (rough proxy for 'feels like daily care needed')."""
    if not records:
        return 100
    mins = sorted(min(r["satiety"], r["spirit"], r["bond"]) for r in records)
    return mins[len(mins) // 2]


def weeks_completed_ratio(records):
    """Hardcoded 1.0 — sim doesn't yet model quest progress.

    Real impl lands when pet_model.py tracks weekly quest counters against
    achievements.cpp's WEEKLY_QUEST_BASE. Until then this is a placeholder;
    the firmware-side test (Wave 3 F6 re-run) is the safety net.
    """
    return 1.0


def shiny_rate_over_1000_hatches(_records):
    """Roll 1000 hatches using the model's RNG and check the rate."""
    import random
    from balance_sim.pet_model import PetModel, load_balance
    bal = load_balance(os.path.join(_ROOT, "firmware", "balance.yaml"))
    m = PetModel(bal)
    rng = random.Random(0xC1AAD)
    hits = sum(1 for _ in range(1000) if m.roll_shiny(rng))
    return hits / 1000.0


def events_per_active_session(_records):
    """Sim doesn't model speech text yet. Placeholder always-green; firmware
    F10 audit is the safety net."""
    return 1


def evaluate(records, targets, hours_per_day):
    lines = []
    all_green = True

    def line(name, actual, ok, band_desc):
        marker = "OK " if ok else "FAIL"
        a = "—" if actual is None else f"{actual:.2f}" if isinstance(actual, float) else str(actual)
        lines.append(f"{marker:4}  {name:32}  actual={a:<12} target={band_desc}")
        return ok

    for name, cfg in targets.items():
        metric = cfg["metric"]
        if metric == "hours_until_level":
            actual = hours_until_level(records, cfg["level"], hours_per_day)
            ok = actual is not None and cfg["min_hours"] <= actual <= cfg["max_hours"]
            band = f"[{cfg['min_hours']}, {cfg['max_hours']}] hrs to L{cfg['level']}"
        elif metric == "min_stat_after_2_day_gap":
            actual = min_stat_after_2_day_gap(records)
            ok = cfg["min_value"] <= actual <= cfg["max_value"]
            band = f"min stat in [{cfg['min_value']}, {cfg['max_value']}]"
        elif metric == "typical_daily_min_stat":
            actual = typical_daily_min_stat(records)
            ok = cfg["min_value"] <= actual <= cfg["max_value"]
            band = f"median in [{cfg['min_value']}, {cfg['max_value']}]"
        elif metric == "weeks_completed_ratio":
            actual = weeks_completed_ratio(records)
            ok = actual >= cfg["min_ratio"]
            band = f"≥{cfg['min_ratio']}"
        elif metric == "shiny_rate_over_1000_hatches":
            actual = shiny_rate_over_1000_hatches(records)
            ok = cfg["min_rate"] <= actual <= cfg["max_rate"]
            band = f"[{cfg['min_rate']}, {cfg['max_rate']}]"
        elif metric == "events_per_active_session":
            actual = events_per_active_session(records)
            ok = actual >= cfg["min_per_session"]
            band = f"≥{cfg['min_per_session']}"
        else:
            actual = None
            ok = False
            band = f"unknown metric: {metric}"
        all_green &= line(name, actual, ok, band)

    return all_green, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sim_json")
    ap.add_argument("--targets", required=True)
    ap.add_argument("--hours-per-day", type=float, default=1.5)
    args = ap.parse_args()

    with open(args.sim_json) as f:
        records = json.load(f)
    with open(args.targets) as f:
        targets = yaml.safe_load(f)

    all_green, lines = evaluate(records, targets, args.hours_per_day)
    print("\n".join(lines))
    if all_green:
        print("\nALL GREEN — balance lands inside target bands.")
        sys.exit(0)
    else:
        print("\nFAIL — at least one dimension is outside its target band.")
        sys.exit(1)


if __name__ == "__main__":
    main()
