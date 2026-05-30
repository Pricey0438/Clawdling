#!/usr/bin/env python3
"""Boot-stress harness for the ipc1 crash investigation.

Resets the device N times (USB-serial-JTAG reset pulse) and, for each
power-on, counts how many crash-reboots (Guru Meditation / ipc1 stack canary)
occur before the device reaches "Dashboard ready". Used to measure the
ipc1 stack-overflow crash rate before/after the WiFi-deferral fix.

Usage: python3 tools/boot_stress.py [port] [iterations]

CAVEAT: the per-cycle RTS reset pulse drives the ESP32-S3 native USB-serial-JTAG,
which re-enumerates the USB device on every reset. After a handful of rapid
pulses the device can drop off USB entirely (port vanishes) and need a physical
power-cycle to return. Use a small iteration count and expect occasional
"NO-UI" cycles that are re-enumeration artifacts (crash count 0), not firmware
crashes. The crash-banner count is the signal to trust, not UI-reached.
"""
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
iters = int(sys.argv[2]) if len(sys.argv) > 2 else 10
PER_BOOT_TIMEOUT = 9.0

def reset(s):
    # USB-serial-JTAG hard reset: assert then release EN via RTS.
    s.setDTR(False); s.setRTS(True); time.sleep(0.12)
    s.setRTS(False); time.sleep(0.02)

total_crashes = 0
cycles_with_crash = 0
cycles_reached_ui = 0

for i in range(1, iters + 1):
    s = serial.Serial(port, 115200, timeout=0.2)
    reset(s)
    deadline = time.time() + PER_BOOT_TIMEOUT
    crashes_this_cycle = 0
    reached = False
    buf = ""
    seen_crash_markers = 0
    while time.time() < deadline:
        try:
            d = s.read(2048)
        except serial.SerialException:
            # Native USB-CDC re-enumerates on reset/crash, invalidating the
            # handle. Reopen and keep observing this cycle.
            try:
                s.close()
            except Exception:
                pass
            time.sleep(0.3)
            try:
                s = serial.Serial(port, 115200, timeout=0.2)
            except Exception:
                time.sleep(0.3)
                continue
            continue
        if not d:
            continue
        buf += d.decode("utf-8", "replace")
        # Count each distinct crash banner.
        c = buf.count("Stack canary watchpoint triggered")
        if c > seen_crash_markers:
            crashes_this_cycle += (c - seen_crash_markers)
            seen_crash_markers = c
        if "Dashboard ready" in buf:
            reached = True
            break
    s.close()
    total_crashes += crashes_this_cycle
    if crashes_this_cycle:
        cycles_with_crash += 1
    if reached:
        cycles_reached_ui += 1
    tag = ("OK" if reached else f"NO-UI-in-{PER_BOOT_TIMEOUT:.0f}s")
    print(f"[{i:2d}/{iters}] {tag} — {crashes_this_cycle} crash-reboot(s) before UI")

print("\n=== summary ===")
print(f"reset cycles:                 {iters}")
print(f"cycles with >=1 crash-reboot: {cycles_with_crash}")
print(f"total crash-reboots seen:     {total_crashes}")
print(f"cycles that reached UI:       {cycles_reached_ui}")
