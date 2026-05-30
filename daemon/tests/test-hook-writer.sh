#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
HOOK="$DIR/../hook-state.sh"

test_writes_state_file() {
    local tmp; tmp=$(mktemp -d)
    HOME="$tmp" "$HOOK" thinking <<<'{"session_id":"abc"}'
    local got; got=$(head -1 "$tmp/.cache/clawdmeter/state/abc")
    rm -rf "$tmp"
    assert_eq "$got" "thinking" "state line written"
}

test_silent_on_missing_session_id() {
    local tmp; tmp=$(mktemp -d)
    HOME="$tmp" "$HOOK" thinking <<<'{}'
    local exit=$?
    if [ -d "$tmp/.cache/clawdmeter/state" ]; then
        rm -rf "$tmp"
        assert_eq "exists" "" "no state dir should be created when session_id missing"
        return
    fi
    rm -rf "$tmp"
    assert_eq "$exit" "0" "exit code must be 0 even with bad input"
}

test_compacting_writes_sticky_marker() {
    local tmp; tmp=$(mktemp -d)
    HOME="$tmp" "$HOOK" compacting <<<'{"session_id":"abc"}'
    local now; now=$(date +%s)
    local until; until=$(head -1 "$tmp/.cache/clawdmeter/state/abc.compacting" 2>/dev/null)
    rm -rf "$tmp"
    if [ -z "$until" ]; then
        assert_eq "" "<deadline>" "sticky compacting marker missing"
        return
    fi
    # Marker should be in the future (≈ now + 5s)
    if [ "$until" -gt "$now" ] && [ "$until" -le "$((now + 15))" ]; then
        assert_eq "ok" "ok" "sticky marker has reasonable deadline"
    else
        assert_eq "$until" "<now..now+15>" "sticky deadline out of range"
    fi
}

test_non_compacting_state_does_not_write_marker() {
    local tmp; tmp=$(mktemp -d)
    HOME="$tmp" "$HOOK" waiting <<<'{"session_id":"xyz"}'
    local exists="no"
    [ -f "$tmp/.cache/clawdmeter/state/xyz.compacting" ] && exists="yes"
    rm -rf "$tmp"
    assert_eq "$exists" "no" "waiting state must not create sticky marker"
}

test_notification_writes_waiting_state() {
    local tmp; tmp=$(mktemp -d)
    HOME="$tmp" "$HOOK" waiting <<<'{"session_id":"notif-abc"}'
    local got; got=$(head -1 "$tmp/.cache/clawdmeter/state/notif-abc")
    rm -rf "$tmp"
    assert_eq "$got" "waiting" "Notification hook arg ('waiting') is what gets written"
}
