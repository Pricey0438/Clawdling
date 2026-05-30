#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../session-scanner.sh"

test_picks_ai_title_when_present() {
    local f="$DIR/fixtures/thinking_user_last.jsonl"
    local name; name=$(name_from_jsonl "$f")
    assert_eq "$name" "Refactor checkout flow" "should pick ai-title"
}

test_truncates_to_22_chars() {
    local tmp; tmp=$(mktemp)
    echo '{"type":"ai-title","title":"This title is definitely longer than 22 characters","sessionId":"x"}' > "$tmp"
    local name; name=$(name_from_jsonl "$tmp")
    rm "$tmp"
    assert_eq "${#name}" "22" "name capped at 22 chars"
}

test_falls_back_to_dir_basename() {
    local tmp; tmp=$(mktemp -d)/-home-dev-projects-Clawdmeter
    mkdir -p "$tmp"
    local fixture="$tmp/aaa.jsonl"
    cat > "$fixture" <<'JSONL'
{"type":"user","message":{"role":"user","content":"hi"},"sessionId":"x"}
JSONL
    local name; name=$(name_from_jsonl "$fixture")
    rm -rf "$(dirname "$tmp")"
    assert_eq "$name" "Clawdmeter" "should decode path-encoded dir to basename"
}
