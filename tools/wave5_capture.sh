#!/usr/bin/env bash
# Wave 5 capture: walks every screen + sub-state on AMOLED-2.16, dumps to
# docs/superpowers/audits/wave-5/screenshots/2-16/<name>.png.
#
# Requires the device to be flashed with the Wave 5 'sim' cmd family
# (Task 1) and reachable on PORT.
#
# Idempotent — re-running overwrites. Sequence is documented inline.

set -eu

PORT="${PORT:-/dev/ttyACM0}"
case "$(uname -s)" in Darwin) PORT="${PORT:-/dev/cu.usbmodem101}";; esac
OUTDIR="docs/superpowers/audits/wave-5/screenshots/2-16"
mkdir -p "$OUTDIR"

PY="python3"
python3 -c "import serial" 2>/dev/null || PY="$HOME/.platformio/penv/bin/python"

send() {
    # send a serial cmd; let the device settle before next action
    "$PY" -c "
import serial,sys,time
p=serial.Serial('$PORT',115200,timeout=2)
p.write(('$1'+'\n').encode())
p.flush()
time.sleep(0.4)
p.close()
"
    sleep 0.6   # LVGL render settle
}

shoot() {
    local name="$1"
    echo "--- capturing $name ---"
    ./screenshot.sh "$OUTDIR/$name.png" "$PORT"
}

# ---- reset to a known clean state ----
# Note: `sim vacation 0` doesn't clear vacation (Task 1 rejects N<=0 with a
# usage message). Use the existing `vacation off` cmd to actually clear.
send "sim low-batt off"
send "vacation off"
send "menu" ; sleep 0.3

# ---- main screens (in cycle order) ----
send "usage"        ; shoot 01-usage
send "session"      ; shoot 02-session
send "pet"          ; shoot 03-pet
send "menu"         ; shoot 04-menu

# ---- non-cycle screens reached via menu ----
send "stats"        ; shoot 05-stats
send "catalog"      ; shoot 06-catalog
send "achievements" ; shoot 07-achievements
send "retirement"   ; shoot 08-retirement

# ---- splash ----
# No `splash` serial cmd exists in main.cpp dispatch — splash is the boot
# screen and only advances on a physical button press. Capture manually:
# flash the device fresh (or temporarily set ui_show_screen(SCREEN_SPLASH)
# default in main.cpp setup) and run ./screenshot.sh against it.
# Slot 09-splash reserved; capture by hand.

# ---- sub-states ----
send "pet"
send "sim vacation 3"
shoot 10-pet-vacation-banner
send "vacation off"

send "sim low-batt 5"
send "pet"
shoot 11-pet-low-battery
send "sim low-batt off"

send "sim recap-now"
shoot 12-pet-recap-modal

send "rename"
sleep 1.2
shoot 13-keyboard-overlay
send "menu"

send "pet"
shoot 14-pet-speech-bubble

send "sim hatch"
sleep 0.8
send "pet"
shoot 15-hatch-flow

# Graduate ceremony — must re-create a pet after the hatch wipe
# (graduate needs an active pet). Run this only if practical; else
# document as deferred.
# send "sim graduate"; sleep 1.0; shoot 16-graduate-ceremony

echo
echo "Capture complete. PNGs in $OUTDIR/"
echo "Manual captures still needed:"
echo "  - SCREEN_SPLASH (09-splash): no serial cmd — flash fresh or temporarily"
echo "    set ui_show_screen(SCREEN_SPLASH) default and screenshot by hand"
echo "  - SCREEN_PROVISIONING: factory reset (BOOT held 5s) → capture by hand"
echo "  - BLE-disconnected indicator: unplug daemon → capture pet/usage screen"
echo "  - WiFi-configuring state: during provisioning AP boot → capture by hand"
echo "  - sim graduate: re-hatch a pet then trigger (see comment in script)"
