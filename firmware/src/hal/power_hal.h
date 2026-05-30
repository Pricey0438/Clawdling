#pragma once

// Power / battery / power-button abstraction. Replaces the legacy power.h
// API but keeps the same shape so existing call sites stay clean.
//
// Some boards (AMOLED-2.16) wire PWR through the PMU's PKEY IRQ; others
// (AMOLED-1.8) route it through an IO expander. The HAL hides which
// source produced the press — shared code just polls
// power_hal_pwr_pressed() once per loop.

void power_hal_init(void);
void power_hal_tick(void);

int  power_hal_battery_pct(void);  // 0..100, or -1 if no battery (see BoardCaps.has_battery)
bool power_hal_is_charging(void);
bool power_hal_is_vbus_in(void);   // USB cable present (true even without a battery)

// Edge-triggered: returns true once per PWR short-press, then clears.
bool power_hal_pwr_pressed(void);

// Test/QA hook: when pct is in 0..100, power_hal_battery_pct() returns that
// value instead of the cached hardware reading. Pass -1 to clear the override
// and fall back to the live PMU value. Used by the `sim low-batt` serial cmd
// to capture low-battery UI states without physically draining the cell.
void power_hal_set_sim_battery(int pct);
