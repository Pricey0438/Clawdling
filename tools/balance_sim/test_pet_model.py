"""Tests for the pet state machine Python port."""
import os
import pytest
from balance_sim.pet_model import PetModel, load_balance

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
YAML = os.path.join(ROOT, "firmware", "balance.yaml")


def fresh_model():
    return PetModel(load_balance(YAML))


def test_xp_to_next_l1():
    m = fresh_model()
    # 200 * 1 * 1.05^1 = 210
    assert m.xp_to_next(1) == 210


def test_xp_to_next_l29():
    m = fresh_model()
    # 200 * 29 * 1.05^29 ≈ 23,873
    val = m.xp_to_next(29)
    assert 23_800 <= val <= 23_900


def test_xp_grant_levels_up():
    m = fresh_model()
    m.grant_xp(300, now_sec=0)   # > L1→2 cost of 210
    assert m.pet.level == 2
    # Firmware behavior (pet.cpp:1266): `g_current.xp = 0` on level-up — no
    # carry-over. A single huge grant only awards one level. Confirmed against
    # pet.cpp lines 1260-1292 ("each level resets XP to 0 — no carry-over").
    assert m.pet.xp == 0


def test_xp_grant_blocked_by_zero_care_stat():
    m = fresh_model()
    m.pet.satiety = 0   # any stat at 0 → 0% XP multiplier
    m.grant_xp(1000, now_sec=0)
    assert m.pet.level == 1
    assert m.pet.xp == 0


def test_care_decay_decrements_after_interval():
    m = fresh_model()
    initial = m.pet.satiety
    interval = m.bal["care"]["decay_interval_sec"]
    m.tick_decay(now_sec=interval + 1)
    assert m.pet.satiety == initial - 1


def test_forgiveness_slows_decay_after_24h_idle():
    m = fresh_model()
    interval = m.bal["care"]["decay_interval_sec"]
    factor   = m.bal["forgiveness"]["slowdown_factor"]
    idle     = m.bal["forgiveness"]["trigger_no_usage_sec"]
    # Rebase decay anchors + last-xp-event to the start of the forgiveness
    # window, then drain the pet to a known state at that moment. This mirrors
    # the firmware lifecycle: decay anchors are kept fresh by the tick loop,
    # so when forgiveness kicks in we measure forward-going slowdown, not
    # catch-up from epoch.
    m.satiety_decay_ts = idle
    m.spirit_decay_ts  = idle
    m.bond_decay_ts    = idle
    m.last_xp_event_sec = 0  # idle since t=0; at t=idle the trigger fires
    initial = m.pet.satiety
    # Advance by exactly one normal decay interval past the trigger.
    # Without forgiveness this would tick 1 decay; WITH forgiveness the
    # effective interval is base*factor (= 1920s) so 481s elapsed yields 0.
    m.tick_decay(now_sec=idle + interval + 1)
    decayed = initial - m.pet.satiety
    assert decayed <= 1, f"forgiveness failed to slow decay (decayed={decayed})"


def test_care_multiplier_tier_below_10():
    """min < 10 → base × 25%"""
    m = fresh_model()
    m.pet.satiety = 5    # min is 5 → below-10 tier
    m.pet.spirit = 50
    m.pet.bond = 50
    # avg = (5+50+50)/3 = 35; base = 35*2 = 70; tier mul = 70*25/100 = 17
    assert m.care_multiplier_pct() == 17, f"expected 17, got {m.care_multiplier_pct()}"


def test_care_multiplier_tier_below_25():
    """10 <= min < 25 → base × 50%"""
    m = fresh_model()
    m.pet.satiety = 15   # min is 15 → below-25 tier
    m.pet.spirit = 50
    m.pet.bond = 50
    # avg = (15+50+50)/3 = 38; base = 38*2 = 76; tier mul = 76*50/100 = 38
    assert m.care_multiplier_pct() == 38, f"expected 38, got {m.care_multiplier_pct()}"


def test_care_multiplier_tier_below_50():
    """25 <= min < 50 → base × 80%"""
    m = fresh_model()
    m.pet.satiety = 30   # min is 30 → below-50 tier
    m.pet.spirit = 50
    m.pet.bond = 50
    # avg = (30+50+50)/3 = 43; base = 43*2 = 86; tier mul = 86*80/100 = 68
    assert m.care_multiplier_pct() == 68, f"expected 68, got {m.care_multiplier_pct()}"


def test_care_multiplier_perfect_care_bonus():
    """all-100 → base × 150%"""
    m = fresh_model()
    m.pet.satiety = 100
    m.pet.spirit = 100
    m.pet.bond = 100
    # avg = 100; base = 200; no tier penalty; perfect-care = 200*150/100 = 300
    assert m.care_multiplier_pct() == 300, f"expected 300, got {m.care_multiplier_pct()}"


def test_care_multiplier_avg_90_bonus():
    """avg >= 90 → base × 125%"""
    m = fresh_model()
    m.pet.satiety = 90
    m.pet.spirit = 90
    m.pet.bond = 90
    # avg = 90; base = 180; no tier penalty; avg-90 bonus = 180*125/100 = 225
    assert m.care_multiplier_pct() == 225, f"expected 225, got {m.care_multiplier_pct()}"


def test_max_level_cap_blocks_levelup():
    """Pet at PET_MAX_LEVEL with care=100 should not level past cap."""
    m = fresh_model()
    m.pet.level = m.bal["pet"]["max_level"]   # 30
    m.pet.xp = 0
    m.grant_xp(1_000_000, now_sec=0)
    assert m.pet.level == m.bal["pet"]["max_level"], f"level shouldn't exceed cap; got {m.pet.level}"


def test_xp_to_next_at_max_level_returns_sentinel():
    """xp_to_next(max_level) returns the uint32_t sentinel."""
    m = fresh_model()
    val = m.xp_to_next(m.bal["pet"]["max_level"])
    assert val == 2**32 - 1, f"max-level should return sentinel; got {val}"
