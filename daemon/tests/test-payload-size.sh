#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../session-scanner.sh"

test_small_payload_passes_through() {
    local ses='[{"id":"a1","n":"x","c":1,"st":"t","a":0}]'
    local out; out=$(compose_ble_payload 0 0 0 0 "ok" "true" "$ses")
    local n; n=$(echo "$out" | jq '.ses | length')
    assert_eq "$n" "1" "1 session kept (small payload)"
    local ts_type; ts_type=$(echo "$out" | jq -r '.ts | type')
    assert_eq "$ts_type" "number" "ts field is a JSON number"
    local size=${#out}
    [ "$size" -le "$PAYLOAD_MAX" ] || { echo "  FAIL: size $size > $PAYLOAD_MAX" >&2; __TEST_FAILS=$((__TEST_FAILS+1)); }
}

test_oversize_payload_drops_oldest() {
    # 8 sessions × 22-char names → ~530B raw; should trim 1-2 oldest
    local ses
    ses=$(jq -nc --argjson n 8 '[range(0;$n) | {
        id: ("id" + (.|tostring)),
        n:  "verylongnamehere2222xx",
        c:  100,
        st: "t",
        a:  .
    }]')
    local before; before=$(jq 'length' <<< "$ses")
    local out; out=$(compose_ble_payload 99 99 99 99 "allowed" "true" "$ses" 2>/dev/null)
    local after; after=$(echo "$out" | jq '.ses | length')
    local size=${#out}
    [ "$size" -le "$PAYLOAD_MAX" ] || { echo "  FAIL: size $size > $PAYLOAD_MAX" >&2; __TEST_FAILS=$((__TEST_FAILS+1)); }
    [ "$after" -lt "$before" ] || { echo "  FAIL: expected fewer than $before sessions, got $after" >&2; __TEST_FAILS=$((__TEST_FAILS+1)); }
    [ "$after" -gt 0 ] || { echo "  FAIL: expected some sessions kept, got 0" >&2; __TEST_FAILS=$((__TEST_FAILS+1)); }
}

test_empty_ses_array_passes_through() {
    local out; out=$(compose_ble_payload 0 0 0 0 "ok" "true" "[]")
    local n; n=$(echo "$out" | jq '.ses | length')
    assert_eq "$n" "0" "empty ses array preserved"
}

# Boundary check for the tighter 480-byte cap. A 7-session × 22-char-name
# payload comes in at ~485B raw — the trim should fire and drop at least
# one session so the result lands ≤ PAYLOAD_MAX. Failing this test before
# PAYLOAD_MAX is dropped to 480 proves the bound is binding (catches the
# regression direction we care about — a future loosening would reopen
# the silent-truncation window the firmware NACK now backstops).
test_trims_below_480_cap() {
    local ses
    ses=$(jq -nc --argjson n 7 '[range(0;$n) | {
        id: ("id" + (.|tostring)),
        n:  "verylongnamehere2222xx",
        c:  100,
        st: "t",
        a:  .
    }]')
    local before; before=$(jq 'length' <<< "$ses")
    local out; out=$(compose_ble_payload 99 99 99 99 "allowed" "true" "$ses" 2>/dev/null)
    local size=${#out}
    local after; after=$(echo "$out" | jq '.ses | length')
    [ "$size" -le 480 ] || { echo "  FAIL: size $size > 480" >&2; __TEST_FAILS=$((__TEST_FAILS+1)); }
    [ "$after" -lt "$before" ] || { echo "  FAIL: expected trim from $before, got $after" >&2; __TEST_FAILS=$((__TEST_FAILS+1)); }
}
