#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
source "$DIR/../session-scanner.sh"

test_user_last_is_thinking() {
    local f="$DIR/fixtures/thinking_user_last.jsonl"
    touch "$f"  # Ensure fresh mtime
    local state; state=$(infer_state_from_jsonl "$f")
    assert_eq "$state" "thinking" "user last entry should be thinking"
}

test_assistant_endturn_is_waiting() {
    local f="$DIR/fixtures/waiting_assistant_endturn.jsonl"
    touch "$f"  # Ensure fresh mtime
    local state; state=$(infer_state_from_jsonl "$f")
    assert_eq "$state" "waiting" "assistant end_turn should be waiting"
}

test_assistant_tool_use_is_thinking() {
    local f="$DIR/fixtures/thinking_tool_use.jsonl"
    touch "$f"  # Ensure fresh mtime
    local state; state=$(infer_state_from_jsonl "$f")
    assert_eq "$state" "thinking" "assistant tool_use should be thinking"
}

test_old_mtime_is_idle() {
    local f="$DIR/fixtures/idle_old.jsonl"
    # Ensure stale mtime — git does not preserve mtimes across checkouts.
    touch -t 202605200000 "$f"
    local state; state=$(infer_state_from_jsonl "$f")
    assert_eq "$state" "idle" "old mtime should override to idle"
}

test_malformed_falls_back_to_idle() {
    local f="$DIR/fixtures/malformed.jsonl"
    local state; state=$(infer_state_from_jsonl "$f")
    assert_eq "$state" "idle" "malformed jsonl should fall back to idle"
}
