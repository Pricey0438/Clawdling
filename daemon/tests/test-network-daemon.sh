#!/bin/bash
# Integration test: boot clawdmeter-net.py, hit endpoints with curl, check shape + auth.
# Sourced by run-tests.sh; defines test_* functions.
set -u

NETD_PORT=18443  # non-default, avoid colliding with a live daemon
NETD_TOKEN="test-token-abc123"
NETD_PID=""
NETD_LOG=$(mktemp)

_netd_start() {
    PORT="$NETD_PORT" TOKEN="$NETD_TOKEN" python3 "$DIR/../clawdmeter-net.py" >"$NETD_LOG" 2>&1 &
    NETD_PID=$!
    # Wait for "listening" line or 3s
    for _ in $(seq 1 30); do
        grep -q "listening" "$NETD_LOG" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "daemon did not start: $(cat "$NETD_LOG")" >&2
    return 1
}

_netd_stop() {
    if [ -n "$NETD_PID" ]; then
        kill "$NETD_PID" 2>/dev/null
        wait "$NETD_PID" 2>/dev/null
    fi
    NETD_PID=""
    rm -f "$NETD_LOG"
}

test_state_requires_auth() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$NETD_PORT/state")
    assert_eq "$code" "401" "GET /state without auth must be 401"
    _netd_stop
}

test_state_with_valid_token_returns_json() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local body
    body=$(curl -s -H "Authorization: Bearer $NETD_TOKEN" "http://127.0.0.1:$NETD_PORT/state")
    echo "$body" | jq -e '.ts and has("s") and has("w") and (.ses | type=="array")' >/dev/null
    assert_eq "$?" "0" "GET /state with valid token must return BLE wire format {ts,s,w,ses,...}"
    _netd_stop
}

test_state_wrong_token_is_401() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer wrong" "http://127.0.0.1:$NETD_PORT/state")
    assert_eq "$code" "401" "GET /state with wrong token must be 401"
    _netd_stop
}

test_refresh_requires_auth() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$NETD_PORT/refresh")
    assert_eq "$code" "401" "POST /refresh without auth must be 401"
    _netd_stop
}

test_refresh_wrong_token_is_401() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
        -H "Authorization: Bearer wrong" "http://127.0.0.1:$NETD_PORT/refresh")
    assert_eq "$code" "401" "POST /refresh with wrong token must be 401"
    _netd_stop
}

test_refresh_with_token_returns_ok() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local body
    body=$(curl -s -X POST -H "Authorization: Bearer $NETD_TOKEN" "http://127.0.0.1:$NETD_PORT/refresh")
    assert_eq "$body" '{"ok":true}' "POST /refresh with valid token must return {ok:true}"
    _netd_stop
}

test_state_includes_sessions_from_scanner() {
    local fix; fix=$(mktemp -d)
    mkdir -p "$fix/.claude/projects/proj-a"
    cat > "$fix/.claude/projects/proj-a/abcd1234.jsonl" <<'EOF'
{"type":"user","message":{"role":"user","content":"hi"},"timestamp":"2026-05-24T10:00:00Z"}
{"type":"assistant","message":{"role":"assistant","content":"hi","stop_reason":"end_turn","usage":{"input_tokens":10,"cache_creation_input_tokens":0,"cache_read_input_tokens":0}}}
EOF

    HOME="$fix" _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); rm -rf "$fix"; return; }
    # Poll up to 6s for the scanner thread to populate the snapshot —
    # resilient to slow CI hosts where the first source+jq cycle can
    # exceed 1s.
    local count=0
    for _ in $(seq 1 60); do
        count=$(curl -s -H "Authorization: Bearer $NETD_TOKEN" \
            "http://127.0.0.1:$NETD_PORT/state" | jq '.ses | length')
        [ "$count" -ge 1 ] && break
        sleep 0.1
    done
    assert_eq "$count" "1" "snapshot must include one session from the fixture"
    _netd_stop
    rm -rf "$fix"
}

test_state_includes_tz_offset() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local tz
    tz=$(curl -s -H "Authorization: Bearer $NETD_TOKEN" "http://127.0.0.1:$NETD_PORT/state" | jq '.tz')
    # tz must be an integer between -50400 and +50400 (UTC±14h)
    echo "$tz" | grep -qE '^-?[0-9]+$' && [ "$tz" -ge -50400 ] && [ "$tz" -le 50400 ]
    assert_eq "$?" "0" "/state must include a tz field in [-50400, 50400] seconds (P1-8)"
    _netd_stop
}

# F12 — wave-3 punch-list P1-14: cover the bio-card endpoints that previously
# had zero test coverage despite being daemon-exposed.

test_graduate_svg_requires_auth() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$NETD_PORT/graduate.svg")
    assert_eq "$code" "401" "GET /graduate.svg without auth must be 401"
    _netd_stop
}

test_graduate_sample_with_auth_returns_svg() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local content_type body
    content_type=$(curl -s -o /dev/null -w '%{content_type}' \
        -H "Authorization: Bearer $NETD_TOKEN" "http://127.0.0.1:$NETD_PORT/graduate.svg/sample")
    case "$content_type" in
        image/svg+xml*) ;;
        *) __TEST_FAILS=$((__TEST_FAILS+1)); echo "FAIL: sample bio Content-Type was '$content_type', wanted image/svg+xml"; _netd_stop; return ;;
    esac
    body=$(curl -s -H "Authorization: Bearer $NETD_TOKEN" "http://127.0.0.1:$NETD_PORT/graduate.svg/sample")
    echo "$body" | head -c 5 | grep -q '<svg' || echo "$body" | head -c 50 | grep -q '<svg'
    assert_eq "$?" "0" "GET /graduate.svg/sample body must start with <svg"
    _netd_stop
}

test_graduate_svg_escapes_xml() {
    _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); return; }
    local body
    # Submit a name with characters that would be invalid in an SVG text node
    # if not escaped. html.escape (default quote=True) handles all five.
    body=$(curl -s -H "Authorization: Bearer $NETD_TOKEN" \
        "http://127.0.0.1:$NETD_PORT/graduate.svg?name=A%3C%3E%26%22%27B&species=Cronfox")
    # The raw < > & must not appear in the rendered name text — only their
    # entity-encoded forms. Look for the encoded angle brackets to prove the
    # path actually rendered the user-controlled name.
    echo "$body" | grep -q '&lt;' && echo "$body" | grep -q '&gt;' && echo "$body" | grep -q '&amp;'
    assert_eq "$?" "0" "GET /graduate.svg must XML-escape <, >, & in user-controlled name"
    # And no raw <script> if someone tried injection
    echo "$body" | grep -qE '<script' && {
        echo "FAIL: raw <script> tag found in graduate.svg output"
        __TEST_FAILS=$((__TEST_FAILS+1))
    }
    _netd_stop
}

test_state_uses_ble_wire_format() {
    local fix; fix=$(mktemp -d)
    mkdir -p "$fix/.claude/projects/proj-a"
    cat > "$fix/.claude/projects/proj-a/abcd1234.jsonl" <<'EOF'
{"type":"user","message":{"role":"user","content":"hi"},"timestamp":"2026-05-24T10:00:00Z"}
{"type":"assistant","message":{"role":"assistant","content":"hi","stop_reason":"end_turn","usage":{"input_tokens":10,"cache_creation_input_tokens":0,"cache_read_input_tokens":0}}}
EOF

    HOME="$fix" _netd_start || { __TEST_FAILS=$((__TEST_FAILS+1)); rm -rf "$fix"; return; }
    local body
    # Poll up to 6s for the snapshot
    for _ in $(seq 1 60); do
        body=$(curl -s -H "Authorization: Bearer $NETD_TOKEN" "http://127.0.0.1:$NETD_PORT/state")
        [ "$(echo "$body" | jq '.ses | length')" -ge 1 ] && break
        sleep 0.1
    done
    # Assert top-level BLE wire keys present (NOT nested usage/sessions)
    echo "$body" | jq -e 'has("s") and has("sr") and has("w") and has("wr") and has("st") and has("ok") and has("ses")' >/dev/null
    assert_eq "$?" "0" "/state must use BLE wire format (s/sr/w/wr/st/ok/ses) — firmware parser expects these"
    # Assert legacy nested keys are NOT present at top level
    echo "$body" | jq -e '(has("usage") | not) and (has("sessions") | not)' >/dev/null
    assert_eq "$?" "0" "/state must NOT have top-level 'usage' or 'sessions' keys (firmware ignores them)"
    # Assert session item has compact keys id/n/c/st/a
    echo "$body" | jq -e '.ses[0] | has("id") and has("n") and has("c") and has("st") and has("a")' >/dev/null
    assert_eq "$?" "0" "session items must use compact keys id/n/c/st/a"
    _netd_stop
    rm -rf "$fix"
}
