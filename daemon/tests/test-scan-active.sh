#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../session-scanner.sh"

test_active_window_filters_old_files() {
    local root; root=$(mktemp -d)
    mkdir -p "$root/-home-x-Project"
    # Fresh:
    cp "$DIR/fixtures/waiting_assistant_endturn.jsonl" "$root/-home-x-Project/bbbb1111-2222-3333-4444-555555555555.jsonl"
    # Old: copy fixture, then set mtime older than 30 min
    cp "$DIR/fixtures/idle_old.jsonl" "$root/-home-x-Project/dddd1111-2222-3333-4444-555555555555.jsonl"
    touch -d '2 hours ago' "$root/-home-x-Project/dddd1111-2222-3333-4444-555555555555.jsonl"

    local arr; arr=$(scan_active_sessions "$root")
    local n; n=$(echo "$arr" | jq 'length')
    rm -rf "$root"
    assert_eq "$n" "1" "old session filtered out"
}

test_cap_to_max_sessions() {
    local root; root=$(mktemp -d)
    mkdir -p "$root/-home-x-Project"
    for i in 1 2 3 4 5 6 7 8; do
        local f="$root/-home-x-Project/aaaa$i$i$i$i-2222-3333-4444-555555555555.jsonl"
        cp "$DIR/fixtures/waiting_assistant_endturn.jsonl" "$f"
        touch -d "$i seconds ago" "$f"
    done
    local arr; arr=$(scan_active_sessions "$root")
    local n; n=$(echo "$arr" | jq 'length')
    rm -rf "$root"
    assert_eq "$n" "$MAX_SESSIONS" "capped at MAX_SESSIONS"
}

test_empty_root_returns_empty_array() {
    local root; root=$(mktemp -d)
    local arr; arr=$(scan_active_sessions "$root")
    rm -rf "$root"
    assert_eq "$arr" "[]" "empty root returns []"
}

test_all_stale_returns_empty_array() {
    local root; root=$(mktemp -d)
    mkdir -p "$root/-home-x-Old"
    cp "$DIR/fixtures/idle_old.jsonl" "$root/-home-x-Old/aaaa1111-2222-3333-4444-555555555555.jsonl"
    touch -d '2 hours ago' "$root/-home-x-Old/aaaa1111-2222-3333-4444-555555555555.jsonl"
    local arr; arr=$(scan_active_sessions "$root")
    rm -rf "$root"
    assert_eq "$arr" "[]" "all-stale root returns []"
}

test_disambiguates_same_name() {
    local root; root=$(mktemp -d)
    mkdir -p "$root/-home-x-CRM"
    # Two sessions, no ai-title, same project dir → both pick "CRM" as name
    for sid in eeee1111-2222-3333-4444-555555555555 ffff1111-2222-3333-4444-555555555555; do
        cat > "$root/-home-x-CRM/$sid.jsonl" <<JSONL
{"type":"user","message":{"role":"user","content":"hi"},"sessionId":"$sid"}
JSONL
    done
    local arr; arr=$(scan_active_sessions "$root")
    local names; names=$(echo "$arr" | jq -r '.[].n' | sort)
    rm -rf "$root"
    local expected=$'CRM (eeee)\nCRM (ffff)'
    assert_eq "$names" "$expected" "same name disambiguated with id4 suffix"
}
