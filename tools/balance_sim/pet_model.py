"""Python port of firmware/src/pet.cpp state machine.

Models:
  - XP accumulation from session/5h/weekly deltas
  - Level-up loop (firmware behavior: xp = 0 on level-up, no carry-over)
  - Care stat decay (with Wave-4 forgiveness slowdown)
  - Care-stat XP multiplier (tiered: min-stat ladder + perfect-care bonus)
  - Shiny roll on hatch (seeded RNG for reproducibility)

NOT modeled:
  - Persistence (sim is pure in-memory)
  - Transport (BLE / HTTP)
  - UI rendering
  - Speech bubble text selection (only event firings tracked)

Reference: firmware/src/pet.cpp lines 960-985 (decay), 1215-1292 (XP grant +
level-up), 1302-1307 (xp_to_next curve).
"""
import random
import sys
from dataclasses import dataclass, field
from typing import Optional

try:
    import yaml
except ImportError:
    print("balance_sim requires PyYAML: pip install pyyaml", file=sys.stderr)
    raise


def load_balance(path: str) -> dict:
    """Read firmware/balance.yaml into a nested dict."""
    with open(path) as f:
        return yaml.safe_load(f)


@dataclass
class PetState:
    level: int = 1
    xp: int = 0
    satiety: int = 100
    spirit: int = 100
    bond: int = 100
    is_shiny: bool = False
    total_xp_earned: int = 0


@dataclass
class PetModel:
    bal: dict
    pet: PetState = field(default_factory=PetState)
    # Decay anchors (last-decay-tick timestamp per stat). Initialized to 0 so
    # the first tick_decay call advances them; sim driver explicitly rebases
    # at boot if needed.
    satiety_decay_ts: int = 0
    spirit_decay_ts: int = 0
    bond_decay_ts: int = 0
    # Bookkeeping for forgiveness logic — the wall clock of the last XP event.
    last_xp_event_sec: int = 0
    # Cumulative care-action counts (for weekly quest accounting in sim driver)
    feed_count: int = 0
    play_count: int = 0
    pet_count: int = 0

    def xp_to_next(self, level: int) -> int:
        """Mirrors firmware pet_xp_to_next: base * L * exp_base^L.

        See pet.cpp:1302-1307. Uses double precision and truncates to uint32_t.
        """
        if level <= 0 or level >= self.bal["pet"]["max_level"]:
            return 2**32 - 1
        base = self.bal["level_curve"]["base"]
        exp_base = self.bal["level_curve"]["exp_base"]
        return int(base * level * (exp_base ** level))

    def care_multiplier_pct(self) -> int:
        """Firmware tiered multiplier — returns percent (0..300+).

        Mirrors pet.cpp:1225-1242:
          base = avg(s, p, b) * 2            (0..200)
          min_stat penalty ladder:
            0       → 0%
            < 10    → x 0.25
            < 25    → x 0.50
            < 50    → x 0.80
          perfect-care bonus:
            100/100/100 → x 1.50
            avg >= 90   → x 1.25
            avg >= 75   → x 1.10
        """
        s, p, b = self.pet.satiety, self.pet.spirit, self.pet.bond
        min_stat = min(s, p, b)
        avg = (s + p + b) // 3
        mul_pct = avg * 2  # 0..200

        if min_stat == 0:
            mul_pct = 0
        elif min_stat < 10:
            mul_pct = (mul_pct * 25) // 100
        elif min_stat < 25:
            mul_pct = (mul_pct * 50) // 100
        elif min_stat < 50:
            mul_pct = (mul_pct * 80) // 100

        if s == 100 and p == 100 and b == 100:
            mul_pct = (mul_pct * 150) // 100
        elif avg >= 90:
            mul_pct = (mul_pct * 125) // 100
        elif avg >= 75:
            mul_pct = (mul_pct * 110) // 100

        return mul_pct

    def grant_xp(self, raw_xp: int, now_sec: int) -> int:
        """Apply care-stat multiplier, add to xp, run the level-up loop.

        Returns the actual XP added (post-multiplier).

        Firmware level-up behavior (pet.cpp:1260-1292): on each level-up,
        xp is RESET to 0 (no carry-over). A single huge grant only awards
        one level — cascading multi-level jumps from one payload are
        intentionally disallowed by the firmware.
        """
        if raw_xp <= 0:
            return 0
        mul_pct = self.care_multiplier_pct()
        applied = (raw_xp * mul_pct) // 100
        if applied <= 0:
            return 0
        self.pet.xp += applied
        self.pet.total_xp_earned += applied
        self.last_xp_event_sec = now_sec
        max_level = self.bal["pet"]["max_level"]
        while self.pet.level < max_level:
            cost = self.xp_to_next(self.pet.level)
            if self.pet.xp < cost:
                break
            # Firmware: xp = 0 (NOT xp -= cost). See pet.cpp:1266.
            self.pet.xp = 0
            self.pet.level += 1
            # NOTE: firmware's loop will exit on the next iteration because
            # xp is now 0 < cost. We match that exact behavior.
        return applied

    def _decay_interval_now(self, now_sec: int) -> int:
        """Return the decay interval, scaled up if forgiveness is active.

        Wave-4 forgiveness (modeled here ahead of firmware Task 10):
          idle_sec >= trigger_no_usage_sec → interval *= slowdown_factor
        """
        base_interval = self.bal["care"]["decay_interval_sec"]
        trigger = self.bal["forgiveness"]["trigger_no_usage_sec"]
        factor = self.bal["forgiveness"]["slowdown_factor"]
        idle_sec = now_sec - self.last_xp_event_sec
        if idle_sec >= trigger:
            return base_interval * factor
        return base_interval

    def _decay_one(self, stat_attr: str, ts_attr: str, now_sec: int):
        """Tick one care stat down by 1 per decay-interval elapsed.

        Mirrors pet.cpp:960-967 lambda decay_one — while-loop catches up
        on long offline gaps rather than dropping ticks. Decay anchors
        default to 0 in PetState; sim drivers should rebase explicitly if
        they want to skip the initial catch-up burst.
        """
        # NOTE: interval is sampled once per call (not per loop iteration). If the
        # forgiveness trigger crosses mid-catch-up, this sim collapses the boundary
        # into a single sampled interval. Task 10's firmware impl decides the
        # canonical behavior; until then, this matches the per-call sampling that
        # Task 10's plan code uses.
        interval = self._decay_interval_now(now_sec)
        ts = getattr(self, ts_attr)
        while now_sec > ts and (now_sec - ts) > interval:
            stat = getattr(self.pet, stat_attr)
            if stat > 0:
                setattr(self.pet, stat_attr, stat - 1)
            ts += interval
        setattr(self, ts_attr, ts)

    def tick_decay(self, now_sec: int):
        """Advance all three care stats to `now_sec`."""
        self._decay_one("satiety", "satiety_decay_ts", now_sec)
        self._decay_one("spirit",  "spirit_decay_ts",  now_sec)
        self._decay_one("bond",    "bond_decay_ts",    now_sec)

    # Care actions: refill stat by action_refill_amount, rebase decay anchor.
    # COOLDOWN NOT MODELED — firmware enforces action_cooldown_sec between same-action
    # uses; sim drivers are expected to respect cadence themselves. Return True is
    # always-success for that reason.
    def feed(self, now_sec: int) -> bool:
        refill = self.bal["care"]["action_refill_amount"]
        self.pet.satiety = min(100, self.pet.satiety + refill)
        self.satiety_decay_ts = now_sec  # rebase — refill "freshens" decay
        self.feed_count += 1
        return True

    def play(self, now_sec: int) -> bool:
        refill = self.bal["care"]["action_refill_amount"]
        self.pet.spirit = min(100, self.pet.spirit + refill)
        self.spirit_decay_ts = now_sec
        self.play_count += 1
        return True

    def pet_action(self, now_sec: int) -> bool:
        refill = self.bal["care"]["action_refill_amount"]
        self.pet.bond = min(100, self.pet.bond + refill)
        self.bond_decay_ts = now_sec
        self.pet_count += 1
        return True

    def roll_shiny(self, rng: random.Random) -> bool:
        """1 / drop_rate_denom probability."""
        denom = self.bal["shiny"]["drop_rate_denom"]
        return rng.randrange(denom) == 0
