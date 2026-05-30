#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../session-scanner.sh"

test_single_session_record() {
    local f="$DIR/fixtures/thinking_user_last.jsonl"
    touch "$f"  # ensure fixture mtime is fresh (git checkout doesn't preserve mtimes)
    local sid="aaaa1111-2222-3333-4444-555555555555"
    local rec; rec=$(session_record "$f")
    # Expect: {"id":"aaaa1111","n":"Refactor checkout flow","c":63,"st":"t","a":<num>}
    local id; id=$(echo "$rec" | jq -r .id)
    local n;  n=$(echo "$rec" | jq -r .n)
    local c;  c=$(echo "$rec" | jq -r .c)
    local st; st=$(echo "$rec" | jq -r .st)
    assert_eq "$id" "aaaa1111" "8-char id"
    assert_eq "$n"  "Refactor checkout flow" "name"
    assert_eq "$c"  "63" "ctx pct"
    assert_eq "$st" "t" "state code (thinking)"
}

test_state_code_mapping() {
    # waiting fixture
    touch "$DIR/fixtures/waiting_assistant_endturn.jsonl"  # ensure fresh mtime
    local rec; rec=$(session_record "$DIR/fixtures/waiting_assistant_endturn.jsonl")
    local st; st=$(echo "$rec" | jq -r .st)
    assert_eq "$st" "w" "state code (waiting)"
}

test_idle_state_code() {
    # Re-touch idle fixture to keep mtime stale (git doesn't preserve mtimes)
    touch -t 202605200000 "$DIR/fixtures/idle_old.jsonl"
    local rec; rec=$(session_record "$DIR/fixtures/idle_old.jsonl")
    local st; st=$(echo "$rec" | jq -r .st)
    assert_eq "$st" "i" "state code (idle)"
}
