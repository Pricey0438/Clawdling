#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../session-scanner.sh"

# thinking_user_last.jsonl has assistant usage:
#   input_tokens=12, cache_creation=5000, cache_read=120000, model=claude-opus-4-7
# Total = 125012 / 200000 = 62.5% -> rounds to 63
test_opus_context_pct() {
    local f="$DIR/fixtures/thinking_user_last.jsonl"
    STATE_DIR=$(mktemp -d)
    local pct; pct=$(context_pct_from_jsonl "$f" "test-opus")
    rm -rf "$STATE_DIR"
    assert_eq "$pct" "63" "opus session ctx %"
}

# waiting fixture: 8 + 2000 + 40000 = 42008 / 200000 = 21% (rounds)
test_sonnet_context_pct() {
    local f="$DIR/fixtures/waiting_assistant_endturn.jsonl"
    STATE_DIR=$(mktemp -d)
    local pct; pct=$(context_pct_from_jsonl "$f" "test-sonnet")
    rm -rf "$STATE_DIR"
    assert_eq "$pct" "21" "sonnet session ctx %"
}

test_no_assistant_entry_is_zero() {
    local tmp; tmp=$(mktemp)
    cat > "$tmp" <<'JSONL'
{"type":"user","message":{"role":"user","content":"hi"},"sessionId":"x","timestamp":"2026-05-22T00:00:00Z"}
JSONL
    STATE_DIR=$(mktemp -d)
    local pct; pct=$(context_pct_from_jsonl "$tmp" "test-empty")
    rm -rf "$STATE_DIR"; rm "$tmp"
    assert_eq "$pct" "0" "no assistant entry yet"
}

# Post-compact transitional state: an isCompactSummary user msg appears
# AFTER the latest assistant entry. The pre-compact tokens (88k = 44%)
# are stale; scanner must estimate from the summary's content size
# instead (~4 chars/token).
test_post_compact_uses_summary_estimate() {
    local tmp; tmp=$(mktemp)
    # Build a ~12000-char summary (well-formed JSON line)
    local big; big=$(printf 'word %.0s' $(seq 1 2400))  # 12000 chars
    {
        echo '{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":12000,"cache_read_input_tokens":76000,"output_tokens":500}},"sessionId":"s","timestamp":"2026-05-22T00:01:00Z"}'
        jq -nc --arg c "$big" '{type:"user",isCompactSummary:true,message:{role:"user",content:$c},sessionId:"s",timestamp:"2026-05-22T00:02:00Z"}'
    } > "$tmp"
    STATE_DIR=$(mktemp -d)
    local pct; pct=$(context_pct_from_jsonl "$tmp" "test-post-compact")
    rm -rf "$STATE_DIR"; rm "$tmp"
    # 12000 chars / 4 = 3000 tokens → 3000*100/200000 = 1.5 → rounds to 2
    assert_eq "$pct" "2" "post-compact uses summary-size estimate, not stale pre-compact tokens"
}

# Once a new assistant turn happens after compact, its real usage takes
# over (the walk hits it before the compact summary).
test_post_compact_then_new_turn_uses_real_tokens() {
    local tmp; tmp=$(mktemp)
    local big; big=$(printf 'word %.0s' $(seq 1 2400))
    {
        echo '{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":12000,"cache_read_input_tokens":76000,"output_tokens":500}},"sessionId":"s","timestamp":"2026-05-22T00:01:00Z"}'
        jq -nc --arg c "$big" '{type:"user",isCompactSummary:true,message:{role:"user",content:$c},sessionId:"s",timestamp:"2026-05-22T00:02:00Z"}'
        echo '{"type":"user","message":{"role":"user","content":"hi again"},"sessionId":"s","timestamp":"2026-05-22T00:03:00Z"}'
        echo '{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":2,"cache_creation_input_tokens":4000,"cache_read_input_tokens":20000,"output_tokens":100}},"sessionId":"s","timestamp":"2026-05-22T00:04:00Z"}'
    } > "$tmp"
    STATE_DIR=$(mktemp -d)
    local pct; pct=$(context_pct_from_jsonl "$tmp" "test-post-compact-2")
    rm -rf "$STATE_DIR"; rm "$tmp"
    # 24002 / 200000 = 12% (rounds)
    assert_eq "$pct" "12" "post-compact new assistant turn overrides summary estimate"
}

test_detect_tier_default_is_200k() {
    # Sessions with no assistant turn exceeding 200K stay on the default
    # 200K tier and do NOT write a marker (to allow later promotion).
    local tmp; tmp=$(mktemp)
    cat > "$tmp" <<'JSONL'
{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":500,"cache_read_input_tokens":80000}},"sessionId":"s1","timestamp":"2026-05-22T00:00:00Z"}
JSONL
    STATE_DIR=$(mktemp -d)
    local limit; limit=$(detect_tier_for_session "s1" "$tmp")
    local has_marker="no"; [ -f "$STATE_DIR/s1.tier" ] && has_marker="yes"
    rm -rf "$STATE_DIR"; rm "$tmp"
    assert_eq "$limit" "200000" "under-200K turn → 200K tier"
    assert_eq "$has_marker" "no" "no marker written for 200K-tier session"
}

test_detect_tier_writes_1m_marker() {
    # A turn exceeding 200K promotes the session to 1M tier and caches it.
    local tmp; tmp=$(mktemp)
    cat > "$tmp" <<'JSONL'
{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":50000,"cache_read_input_tokens":200000}},"sessionId":"s2","timestamp":"2026-05-22T00:00:00Z"}
JSONL
    STATE_DIR=$(mktemp -d)
    local limit; limit=$(detect_tier_for_session "s2" "$tmp")
    local marker_val=""; [ -f "$STATE_DIR/s2.tier" ] && marker_val=$(head -1 "$STATE_DIR/s2.tier")
    rm -rf "$STATE_DIR"; rm "$tmp"
    assert_eq "$limit" "1000000" "250001-token turn → 1M tier"
    assert_eq "$marker_val" "1m" "marker file written with '1m'"
}

test_detect_tier_reads_cached_marker() {
    # When the marker already exists, return 1M without re-scanning the
    # jsonl. (Test by passing /dev/null as the jsonl — if the function
    # tried to scan it, it would find no assistant entries and return 200K.)
    STATE_DIR=$(mktemp -d)
    printf '1m\n' > "$STATE_DIR/s3.tier"
    local limit; limit=$(detect_tier_for_session "s3" /dev/null)
    rm -rf "$STATE_DIR"
    assert_eq "$limit" "1000000" "cached marker honored without rescanning jsonl"
}

test_running_max_above_last_turn() {
    # Three assistant turns: 80K, 120K, 60K. The latest is 60K but the max
    # mid-walk is 120K. Running max returns 120K / 200K = 60%.
    local tmp; tmp=$(mktemp)
    cat > "$tmp" <<'JSONL'
{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":80000,"cache_read_input_tokens":0}},"sessionId":"s","timestamp":"2026-05-22T00:00:00Z"}
{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":20000,"cache_read_input_tokens":100000}},"sessionId":"s","timestamp":"2026-05-22T00:01:00Z"}
{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":0,"cache_read_input_tokens":60000}},"sessionId":"s","timestamp":"2026-05-22T00:02:00Z"}
JSONL
    STATE_DIR=$(mktemp -d)
    local pct; pct=$(context_pct_from_jsonl "$tmp" "test-rmx")
    rm -rf "$STATE_DIR"; rm "$tmp"
    # 120001 / 200000 = 60.0% → 60
    assert_eq "$pct" "60" "running max picks the highest assistant turn, not the latest"
}

test_tier_aware_pct_for_1m_session() {
    # A 408K-token turn (the scenario from the spec — user's real session)
    # would render >200% with the old hardcoded limit. With tier auto-detect,
    # it renders 41% of 1M.
    local tmp; tmp=$(mktemp)
    cat > "$tmp" <<'JSONL'
{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":2,"cache_creation_input_tokens":8000,"cache_read_input_tokens":400000}},"sessionId":"s","timestamp":"2026-05-22T00:00:00Z"}
JSONL
    STATE_DIR=$(mktemp -d)
    local pct; pct=$(context_pct_from_jsonl "$tmp" "test-1m")
    rm -rf "$STATE_DIR"; rm "$tmp"
    # 408002 / 1000000 = 40.8% → rounds to 41
    assert_eq "$pct" "41" "1M-tier session uses 1M denominator, not 200K"
}

test_compact_summary_resets_running_max_keeps_tier() {
    # Pre-compact: 400K turn (locks in 1M tier marker).
    # Compact-summary: ~24000 chars (≈ 6000 token floor).
    # Post-compact: 85K turn (running max from compact boundary onward).
    # Walk stops at compact-summary; pre-compact 400K is NOT counted in
    # running_max for this scan. But the .tier marker (written by an
    # earlier scan or by detect_tier_for_session) keeps the limit at 1M.
    local tmp; tmp=$(mktemp)
    local big; big=$(printf 'word %.0s' $(seq 1 4800))  # 24000 chars
    {
        echo '{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":1,"cache_creation_input_tokens":8000,"cache_read_input_tokens":392000}},"sessionId":"s","timestamp":"2026-05-22T00:00:00Z"}'
        jq -nc --arg c "$big" '{type:"user",isCompactSummary:true,message:{role:"user",content:$c},sessionId:"s",timestamp:"2026-05-22T00:01:00Z"}'
        echo '{"type":"user","message":{"role":"user","content":"keep going"},"sessionId":"s","timestamp":"2026-05-22T00:02:00Z"}'
        echo '{"type":"assistant","message":{"model":"claude-opus-4-7","usage":{"input_tokens":2,"cache_creation_input_tokens":5000,"cache_read_input_tokens":80000}},"sessionId":"s","timestamp":"2026-05-22T00:03:00Z"}'
    } > "$tmp"
    STATE_DIR=$(mktemp -d)
    # detect_tier_for_session will scan the file top-down and find the 400K
    # turn → write the 1M marker → return 1M. Then context_pct walks
    # bottom-up: hits 85K assistant turn (running_max=85002), then user
    # ("keep going" — skip), then compact-summary (floor = 24000/4 = 6000,
    # less than 85002, no change), STOP. Result: 85002 / 1M = 8.5% → 9.
    local pct; pct=$(context_pct_from_jsonl "$tmp" "test-compact-1m")
    rm -rf "$STATE_DIR"; rm "$tmp"
    assert_eq "$pct" "9" "post-compact running max with 1M tier locked in"
}
