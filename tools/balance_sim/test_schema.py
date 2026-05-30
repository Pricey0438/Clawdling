#!/usr/bin/env python3
"""Schema lint: every key the firmware and sim need must exist with the right type."""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
YAML = os.path.join(ROOT, "firmware", "balance.yaml")
sys.path.insert(0, os.path.join(ROOT, "tools"))
from balance_sim.pet_model import load_balance   # noqa: E402

SCHEMA = {
    "xp.per_session_ctx_pct": int,
    "xp.per_5h_pct": int,
    "xp.per_weekly_pct": int,
    "level_curve.base": int,
    "level_curve.exp_base": float,
    "pet.max_level": int,
    "pet.nvs_flush_interval_sec": int,
    "pet.gallery_cap": int,
    "pet.num_species": int,
    "care.decay_interval_sec": int,
    "care.action_refill_amount": int,
    "care.action_cooldown_sec": int,
    "care.sad_threshold": int,
    "care.neglect_rearm_threshold": int,
    "forgiveness.trigger_no_usage_sec": int,
    "forgiveness.slowdown_factor": int,
    "speech.neglect_min_pet_age_sec": int,
    "speech.neglect_absence_sec": int,
    "shiny.drop_rate_denom": int,
    "vacation.max_days": int,
}


def main():
    data = load_balance(YAML)
    errors = []
    for dotted, expected_type in SCHEMA.items():
        top, _, sub = dotted.partition(".")
        if top not in data or (sub and sub not in data[top]):
            errors.append(f"missing: {dotted}")
            continue
        val = data[top][sub] if sub else data[top]
        if not isinstance(val, expected_type):
            errors.append(f"{dotted}: expected {expected_type.__name__}, got {type(val).__name__} ({val!r})")
    if errors:
        print("SCHEMA FAILURES:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        sys.exit(1)
    print(f"schema ok ({len(SCHEMA)} keys validated)")


if __name__ == "__main__":
    main()
