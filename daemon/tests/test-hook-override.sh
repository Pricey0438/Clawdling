#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../session-scanner.sh"

test_fresh_hook_overrides_inference() {
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local f="$DIR/fixtures/thinking_user_last.jsonl"  # would infer "thinking"
    STATE_DIR=$(mktemp -d)
    printf 'waiting\n%s\n' "$(date +%s)" > "$STATE_DIR/$sid"
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"
    assert_eq "$state" "waiting" "fresh hook override wins"
}

test_aging_hook_with_fresh_jsonl_still_trusted() {
    # Under the new sticky-state design, an aging hook ( > HOOK_STALE_SEC )
    # is still trusted as long as the jsonl is fresh ( < STALE_OVERRIDE_SEC ).
    # Only when BOTH are stale do we declare process-death and return idle.
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local f="$DIR/fixtures/thinking_user_last.jsonl"
    touch "$f"  # ensure fixture jsonl mtime is fresh
    STATE_DIR=$(mktemp -d)
    printf 'waiting\n%s\n' "$(( $(date +%s) - 9999 ))" > "$STATE_DIR/$sid"
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"
    assert_eq "$state" "waiting" "aging hook (jsonl fresh) returns hook state, not inference"
}

test_no_hook_file_uses_inference() {
    local sid="zzzz1111-2222-3333-4444-555555555555"
    local f="$DIR/fixtures/waiting_assistant_endturn.jsonl"
    touch "$f"  # ensure fixture mtime is fresh (git checkout doesn't preserve mtimes)
    STATE_DIR=$(mktemp -d)
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"
    assert_eq "$state" "waiting" "missing hook file uses inference"
}

# Sticky compacting marker keeps "compacting" visible until its deadline,
# even if the regular state file was overwritten to "waiting" by a fast
# PostCompact (the whole point: a 1-2s compact must still register on a
# 5s daemon poll).
test_sticky_compacting_marker_wins_over_waiting() {
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local f="$DIR/fixtures/waiting_assistant_endturn.jsonl"
    touch "$f"
    STATE_DIR=$(mktemp -d)
    # Regular file says waiting (fresh), but the sticky marker is in-window
    printf 'waiting\n%s\n' "$(date +%s)" > "$STATE_DIR/$sid"
    echo $(( $(date +%s) + 10 )) > "$STATE_DIR/$sid.compacting"
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"
    assert_eq "$state" "compacting" "sticky marker overrides fresh waiting state"
}

test_expired_compacting_marker_ignored() {
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local f="$DIR/fixtures/waiting_assistant_endturn.jsonl"
    touch "$f"
    STATE_DIR=$(mktemp -d)
    printf 'waiting\n%s\n' "$(date +%s)" > "$STATE_DIR/$sid"
    echo $(( $(date +%s) - 5 )) > "$STATE_DIR/$sid.compacting"  # already expired
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"
    assert_eq "$state" "waiting" "expired sticky marker falls through to normal state file"
}

test_aging_hook_overridden_to_idle_when_jsonl_also_stale() {
    # When BOTH the hook and the jsonl mtime exceed STALE_OVERRIDE_SEC,
    # the session is presumed dead (Claude Code crashed, SSH dropped
    # without tmux, etc). Override to idle.
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local f; f=$(mktemp)
    cp "$DIR/fixtures/thinking_user_last.jsonl" "$f"
    touch -t 202605200000 "$f"  # ancient jsonl mtime
    STATE_DIR=$(mktemp -d)
    printf 'thinking\n%s\n' "$(( $(date +%s) - 9999 ))" > "$STATE_DIR/$sid"
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"; rm "$f"
    assert_eq "$state" "idle" "stale hook + stale jsonl → idle (process death override)"
}

test_compacting_hook_no_marker_still_compacting() {
    # When state_for_session sees a fresh hook saying "compacting" but the
    # sticky marker file is absent (e.g. marker was deleted, or this is a
    # mid-compact poll where PreCompact wrote both files but the marker
    # was raced away), Rule 2 still returns compacting.
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local f="$DIR/fixtures/thinking_user_last.jsonl"
    touch "$f"
    STATE_DIR=$(mktemp -d)
    printf 'compacting\n%s\n' "$(date +%s)" > "$STATE_DIR/$sid"
    # no .compacting marker
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"
    assert_eq "$state" "compacting" "fresh hook saying compacting wins even without marker"
}

test_hook_at_stale_boundary_still_trusted() {
    # Hook timestamp exactly at HOOK_STALE_SEC age — the <= boundary
    # check trusts it. (Off-by-one regression guard.)
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local f="$DIR/fixtures/thinking_user_last.jsonl"
    touch "$f"
    STATE_DIR=$(mktemp -d)
    printf 'waiting\n%s\n' "$(( $(date +%s) - 300 ))" > "$STATE_DIR/$sid"
    local state; state=$(state_for_session "$f" "$sid")
    rm -rf "$STATE_DIR"
    assert_eq "$state" "waiting" "hook at exactly HOOK_STALE_SEC age still trusted (rule 2 boundary)"
}
