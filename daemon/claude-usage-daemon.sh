#!/bin/bash
# Claude Usage Tracker Daemon (BLE)
# Reads Claude Code OAuth token, polls usage via API, sends to ESP32 over BLE GATT.
# Auto-connects and reconnects to the Claude Controller BLE device.
# Dependencies: curl, awk, bluetoothctl

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=session-scanner.sh
source "$SCRIPT_DIR/session-scanner.sh"

DEVICE_NAME="Claude Controller"
DEVICE_MAC="${DEVICE_MAC:-}"  # auto-discovered if empty
SERVICE_UUID="4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID="4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID="4c41555a-4465-7669-6365-000000000004"
VERSION_CHAR_UUID="4c41555a-4465-7669-6365-000000000005"
POLL_INTERVAL=60
TICK=5
SAVED_MAC_FILE="$HOME/.config/claude-usage-monitor/ble-address"
REFRESH_FLAG="/tmp/claude-usage-refresh-$$"
DBUS_DEST="org.bluez"
NOTIFY_PID=""

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

# Daemon's own short SHA, captured once at startup.
DAEMON_SHA=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")

# Firmware version read from the BLE version characteristic once per connect.
# Format: "<semver> <sha> <board_env>" — single ASCII line, ≤ 64 bytes.
# Reset at the top of each outer connect cycle.
FW_VER=""

read_token() {
    grep -o '"accessToken":"[^"]*"' "$HOME/.claude/.credentials.json" | cut -d'"' -f4
}

# Convert MAC to D-Bus path: AA:BB:CC:DD:EE:FF -> dev_AA_BB_CC_DD_EE_FF
mac_to_dbus_path() {
    local adapter
    adapter=$(busctl --timeout=5 call org.bluez / org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null | grep -o '/org/bluez/hci[0-9]' | head -1)
    adapter=${adapter:-/org/bluez/hci0}
    echo "${adapter}/dev_$(echo "$1" | tr ':' '_')"
}

# Check if device is connected via D-Bus
is_connected() {
    local path
    path=$(mac_to_dbus_path "$DEVICE_MAC")
    busctl --timeout=5 get-property "$DBUS_DEST" "$path" org.bluez.Device1 Connected 2>/dev/null | grep -q "true"
}

# Load saved MAC address
load_mac() {
    if [ -n "$DEVICE_MAC" ]; then return 0; fi
    if [ -f "$SAVED_MAC_FILE" ]; then
        DEVICE_MAC=$(head -1 "$SAVED_MAC_FILE" | tr -d '\r\n ')
        if [[ "$DEVICE_MAC" =~ ^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$ ]]; then
            return 0
        fi
        log "Cached MAC is malformed, discarding"
        rm -f "$SAVED_MAC_FILE"
        DEVICE_MAC=""
    fi
    return 1
}

# Save MAC for fast reconnect
save_mac() {
    mkdir -p "$(dirname "$SAVED_MAC_FILE")"
    echo "$DEVICE_MAC" > "$SAVED_MAC_FILE"
}

# Scan for Claude Controller
scan_for_device() {
    log "Scanning for '$DEVICE_NAME'..."
    # Start LE scan
    bluetoothctl scan le &>/dev/null &
    local scan_pid=$!
    sleep 8
    kill "$scan_pid" 2>/dev/null
    wait "$scan_pid" 2>/dev/null

    # Pick the first matching device. Multiple matches happen when bluez
    # remembers old hardware (e.g. after swapping ESP boards). Stale entries
    # are removed on connect failure (see connect_device), so a few retry
    # cycles will converge on the live device.
    local found
    found=$(bluetoothctl devices 2>/dev/null | grep "$DEVICE_NAME" | head -1 | awk '{print $2}')
    if [ -n "$found" ]; then
        DEVICE_MAC="$found"
        save_mac
        log "Found: $DEVICE_MAC"
        return 0
    fi
    return 1
}

# Connect to the device
connect_device() {
    log "Connecting to $DEVICE_MAC..."

    # Trust first (allows auto-reconnect). Wrap in timeout so a wedged bluez
    # adapter can't pin the whole daemon — SIGTERM from systemd can't fire
    # while the shell is blocked in a syscall on a stuck bluetoothctl child.
    timeout 5 bluetoothctl trust "$DEVICE_MAC" &>/dev/null

    # Connect
    timeout 10 bluetoothctl connect "$DEVICE_MAC" &>/dev/null
    sleep 2

    if is_connected; then
        log "Connected"
        return 0
    fi
    log "Connection failed"
    if [ -f "$SAVED_MAC_FILE" ] && [ "$(cat "$SAVED_MAC_FILE")" = "$DEVICE_MAC" ]; then
        log "Invalidating cached MAC, will rescan by name"
        rm -f "$SAVED_MAC_FILE"
    fi
    # Remove from bluez so the next scan won't re-pick this dead MAC.
    # If the device comes back online it'll re-advertise and be re-discovered.
    timeout 5 bluetoothctl remove "$DEVICE_MAC" &>/dev/null
    DEVICE_MAC=""
    return 1
}

# Find a GATT characteristic path by UUID via D-Bus
find_char_path_by_uuid() {
    local target_uuid="$1"
    local dev_path
    dev_path=$(mac_to_dbus_path "$DEVICE_MAC")

    busctl --timeout=5 tree "$DBUS_DEST" 2>/dev/null | grep -o "${dev_path}/service[0-9a-f]*/char[0-9a-f]*" | while read -r char_path; do
        local uuid
        uuid=$(busctl --timeout=5 get-property "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 UUID 2>/dev/null | tr -d '"' | awk '{print $2}')
        if [ "$uuid" = "$target_uuid" ]; then
            echo "$char_path"
            return 0
        fi
    done
}

# Subscribe to refresh-request notifications. The ESP fires this when it
# has no usage data yet (e.g. after a fresh boot). Daemon awk drops a flag
# file that the inner loop picks up on its next 5s tick.
#
# Implementation notes:
# - dbus-monitor must be running BEFORE we call StartNotify, because busctl
#   exits immediately, the subscription tears down within milliseconds, and
#   the ESP's notify fires inside that brief window.
# - stdbuf -oL forces line-buffered stdout on dbus-monitor; without it,
#   glibc switches to block buffering when stdout is a pipe and signals
#   never reach awk until ~4KB accumulates.
# - The pipeline runs in a setsid'd child so we can kill the whole process
#   group (dbus-monitor + awk) atomically. Killing only awk leaves
#   dbus-monitor orphaned, and `wait $!` in bash waits on the whole job
#   until every pipeline member exits, hanging the daemon.
start_notify_subscriber() {
    local req_path
    req_path=$(find_char_path_by_uuid "$REQ_CHAR_UUID")
    if [ -z "$req_path" ]; then
        log "Refresh char not found, skipping notify subscriber"
        return 1
    fi

    setsid bash -c "stdbuf -oL dbus-monitor --system \"type='signal',interface='org.freedesktop.DBus.Properties',path='$req_path',member='PropertiesChanged'\" 2>/dev/null | awk -v flag='$REFRESH_FLAG' '/Value/ { system(\"touch \" flag); fflush() }'" &
    NOTIFY_PID=$!

    # Give dbus-monitor a moment to register its match rule, then trigger
    # the GATT subscription that causes the ESP to fire its notify.
    sleep 0.3
    busctl --timeout=5 call "$DBUS_DEST" "$req_path" org.bluez.GattCharacteristic1 StartNotify >/dev/null 2>&1

    log "Refresh subscriber started (pgid=$NOTIFY_PID)"
}

stop_notify_subscriber() {
    if [ -n "$NOTIFY_PID" ]; then
        # Kill the whole process group (setsid made NOTIFY_PID the leader).
        # Don't wait — we don't care about exit status and waiting can hang
        # if any group member is slow to exit.
        kill -TERM -"$NOTIFY_PID" 2>/dev/null
        NOTIFY_PID=""
    fi
    rm -f "$REFRESH_FLAG"
}

# Result format: "<semver> <sha> <board_env>" as plain ASCII, ≤ 64 bytes.
read_fw_version() {
    FW_VER=""
    local ver_path
    ver_path=$(find_char_path_by_uuid "$VERSION_CHAR_UUID")
    if [ -z "$ver_path" ]; then
        log "Version characteristic not found (old firmware?), skipping"
        return 1
    fi
    # ReadValue returns an array of bytes via D-Bus; convert to ASCII string.
    local raw
    raw=$(busctl --timeout=5 call "$DBUS_DEST" "$ver_path" \
        org.bluez.GattCharacteristic1 ReadValue "a{sv}" 0 2>/dev/null \
        | grep -oP '(?<=ay )\d+ [0-9 ]+' | head -1)
    if [ -z "$raw" ]; then
        log "Version read returned empty, skipping"
        return 1
    fi
    # raw is "<count> <b1> <b2> ... <bN>" — drop the count, convert bytes to chars.
    local count bytes_str
    read -r count bytes_str <<< "$raw"
    FW_VER=$(printf '%b' "$(printf '\\x%02x' $bytes_str)" 2>/dev/null | tr -d '\0')
    if [ -n "$FW_VER" ]; then
        log "Firmware version: $FW_VER"
        # Atomic write so an interrupted daemon can't leave a truncated file
        # that an external consumer would misread as the real version.
        mkdir -p "$HOME/.cache/clawdmeter"
        local cache_file="$HOME/.cache/clawdmeter/firmware-version"
        printf '%s\n' "$FW_VER" > "$cache_file.tmp" && mv -f "$cache_file.tmp" "$cache_file"
    else
        log "Version decode failed, skipping"
        return 1
    fi
}

# Write data to the RX characteristic via D-Bus
write_gatt() {
    local char_path="$1"
    local data="$2"

    # Convert string to byte array for D-Bus: "hi" -> 0x68 0x69
    local bytes=""
    for ((i = 0; i < ${#data}; i++)); do
        local byte
        byte=$(printf "0x%02x" "'${data:$i:1}")
        bytes="$bytes $byte"
    done
    local count=${#data}

    busctl --timeout=5 call "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 \
        WriteValue "aya{sv}" "$count" $bytes 0 2>/dev/null
}

# Cached most-recent usage payload pieces (filled by fetch_usage).
CACHED_S=0; CACHED_SR=0; CACHED_W=0; CACHED_WR=0; CACHED_ST="unknown"; CACHED_OK=false

fetch_usage() {
    # Reset health on every attempt — sticky `true` from a past success would
    # mask a current outage (expired OAuth token, transient network error,
    # API schema change) by continuing to ship the last known values to the
    # device. The ESP renders `ok:false` distinctly so the user knows.
    CACHED_OK=false

    local token now
    token=$(read_token) || { log "Error: could not read token"; CACHED_ST="error"; return 1; }
    now=$(date +%s)

    local headers
    headers=$(curl -s -D - -o /dev/null \
        "https://api.anthropic.com/v1/messages" \
        -H "Authorization: Bearer $token" \
        -H "anthropic-version: 2023-06-01" \
        -H "anthropic-beta: oauth-2025-04-20" \
        -H "Content-Type: application/json" \
        -H "User-Agent: claude-code/2.1.5" \
        -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}' \
        2>/dev/null) || { log "Error: API call failed"; CACHED_ST="error"; return 1; }

    local s5h_util s5h_reset s7d_util s7d_reset status
    s5h_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-utilization" | tr -d '\r' | cut -d: -f2- | tr -d ' \r\n')
    s5h_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-reset"       | tr -d '\r' | cut -d: -f2- | tr -d ' \r\n')
    s7d_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-utilization"  | tr -d '\r' | cut -d: -f2- | tr -d ' \r\n')
    s7d_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-reset"       | tr -d '\r' | cut -d: -f2- | tr -d ' \r\n')
    status=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-status"          | tr -d '\r' | cut -d: -f2- | tr -d ' \r\n')

    # An expired token returns 401 with no anthropic-ratelimit-* headers. Without
    # this guard, the `${var:-0}` defaults below would ship a healthy-looking
    # "0% used, ok:true" payload that the firmware can't distinguish from
    # legitimate idle usage. Require at least one numeric utilization header.
    if [ -z "$s5h_util" ] && [ -z "$s7d_util" ]; then
        log "fetch_usage: no anthropic-ratelimit-* headers (token expired / API change?); marking unhealthy"
        CACHED_ST="error"
        return 1
    fi
    # Numeric sanity check: both util headers, if present, must look like a float.
    if [ -n "$s5h_util" ] && ! [[ "$s5h_util" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
        log "fetch_usage: unrecognized 5h-utilization value '$s5h_util'; marking unhealthy"
        CACHED_ST="error"
        return 1
    fi

    s5h_util=${s5h_util:-0}; s5h_reset=${s5h_reset:-0}
    s7d_util=${s7d_util:-0}; s7d_reset=${s7d_reset:-0}; status=${status:-unknown}

    CACHED_S=$(awk -v u="$s5h_util" 'BEGIN{printf "%.0f", u*100}')
    CACHED_SR=$(awk -v r="$s5h_reset" -v n="$now" 'BEGIN{v=(r-n)/60; printf "%.0f", (v>0?v:0)}')
    CACHED_W=$(awk -v u="$s7d_util" 'BEGIN{printf "%.0f", u*100}')
    CACHED_WR=$(awk -v r="$s7d_reset" -v n="$now" 'BEGIN{v=(r-n)/60; printf "%.0f", (v>0?v:0)}')
    CACHED_ST="$status"
    CACHED_OK=true
}

compose_payload() {
    local ses="$1"
    local payload
    payload=$(compose_ble_payload \
        "$CACHED_S" "$CACHED_SR" "$CACHED_W" "$CACHED_WR" \
        "$CACHED_ST" "$CACHED_OK" "$ses" "$(date +%s)")
    # Stamp version fields when firmware version is known.
    # set -- on $FW_VER splits on whitespace (intentional; do not quote).
    # shellcheck disable=SC2086
    if [ -n "$FW_VER" ]; then
        set -- $FW_VER
        local fw_semver="$1"
        local fw_sha="${2:-unknown}"
        payload=$(printf '%s' "$payload" | jq -c \
            --arg fw  "$fw_semver" \
            --arg fws "$fw_sha" \
            --arg d   "$DAEMON_SHA" \
            '. + {fw:$fw, fws:$fws, d:$d}')
    fi
    printf '%s' "$payload"
}

push_payload() {
    local ses="$1"
    local payload; payload=$(compose_payload "$ses")
    log "Sending: $payload"
    write_gatt "$RX_CHAR_PATH" "$payload" || { log "Write failed"; return 1; }
}

cleanup() {
    stop_notify_subscriber
    log "Daemon stopped"
    exit 0
}

trap cleanup INT TERM

log "=== Claude Usage Tracker Daemon (BLE) ==="
log "Poll interval: ${POLL_INTERVAL}s"

BACKOFF=1

while true; do
    FW_VER=""   # reset per connect cycle

    # Find the device
    if ! load_mac; then
        scan_for_device || {
            log "Device not found, retrying in ${BACKOFF}s..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Connect if not connected
    if ! is_connected; then
        connect_device || {
            log "Retrying in ${BACKOFF}s..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Find the GATT characteristic
    RX_CHAR_PATH=$(find_char_path_by_uuid "$RX_CHAR_UUID")
    if [ -z "$RX_CHAR_PATH" ]; then
        log "Error: RX characteristic not found, retrying..."
        sleep 5
        continue
    fi
    log "GATT RX path: $RX_CHAR_PATH"

    BACKOFF=1  # reset backoff on successful connection

    start_notify_subscriber

    # Read firmware version once per connect; cached in FW_VER for the
    # duration of this connection. compose_payload stamps it into every push.
    read_fw_version || true

    # Poll loop: tick every $TICK seconds. Poll Anthropic when the
    # interval has elapsed OR when the ESP requested a refresh.
    # Also push whenever the session list changes (cheap ~50ms scan).
    LAST_POLL=0
    LAST_SES_JSON=""
    while is_connected; do
        NOW=$(date +%s)
        need_push=0

        # 1) Refresh-on-request or scheduled fetch
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            [ -f "$REFRESH_FLAG" ] && { log "Refresh requested by device"; rm -f "$REFRESH_FLAG"; }
            if fetch_usage; then
                LAST_POLL=$NOW
                need_push=1
            fi
        fi

        # 2) Session change detection (cheap; ~50ms per scan). Compute once
        #    and reuse in push_payload to avoid a second scan in the same tick.
        SES_JSON=$(scan_active_sessions)
        if [ "$SES_JSON" != "$LAST_SES_JSON" ]; then
            LAST_SES_JSON="$SES_JSON"
            need_push=1
        fi

        if [ "$need_push" = "1" ] && [ "$CACHED_OK" = "true" ]; then
            push_payload "$SES_JSON"
        fi

        sleep "$TICK"
    done

    stop_notify_subscriber
    log "Device disconnected, reconnecting..."
    sleep 2
done
