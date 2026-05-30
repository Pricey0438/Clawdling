#!/bin/bash
# Tests for daemon/usage_trace.py — the extracted trace→payload module.

DIR="$(cd "$(dirname "$0")" && pwd)"
DAEMON_DIR="$(cd "$DIR/.." && pwd)"

test_usage_trace_module_imports() {
    PYTHONPATH="$DAEMON_DIR" python3 -c "import usage_trace" 2>/dev/null
    local rc=$?
    assert_eq "$rc" "0" "import daemon.usage_trace"
}

test_scan_sessions_returns_list() {
    local out
    out=$(PYTHONPATH="$DAEMON_DIR" python3 -c "
import usage_trace
result = usage_trace.scan_sessions()
assert isinstance(result, list), f'expected list, got {type(result)}'
print('ok')
" 2>&1)
    assert_eq "$out" "ok" "scan_sessions returns a list (got: $out)"
}

test_compose_wire_payload_shape() {
    local out
    out=$(PYTHONPATH="$DAEMON_DIR" python3 -c "
import usage_trace, json
wire = usage_trace.compose_wire_payload(
    ts=12345, tz_off=3600,
    usage={'session_pct': 42.0, 'session_reset_mins': 12,
           'weekly_pct': 7.5, 'weekly_reset_mins': 240,
           'status': 'ok'},
    sessions=[],
)
expected_keys = {'ts','tz','s','sr','w','wr','st','ok','ses'}
missing = expected_keys - set(wire.keys())
assert not missing, f'missing keys: {missing}'
assert wire['s'] == 42.0
assert wire['tz'] == 3600
assert wire['ses'] == []
print('ok')
" 2>&1)
    assert_eq "$out" "ok" "compose_wire_payload shape (got: $out)"
}
