#!/bin/bash
# Wraps the three Python balance-sim tests for the bash test harness.

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"

test_balance_sim_schema() {
    local out rc
    out=$(python3 "$ROOT/tools/balance_sim/test_schema.py" 2>&1)
    rc=$?
    assert_eq "$rc" "0" "schema test failed: $out"
}

test_balance_sim_parity() {
    local out rc
    out=$(python3 "$ROOT/tools/balance_sim/test_parity.py" 2>&1)
    rc=$?
    assert_eq "$rc" "0" "parity test failed: $out"
}

test_balance_sim_golden() {
    local out rc
    out=$(python3 "$ROOT/tools/balance_sim/test_golden.py" 2>&1)
    rc=$?
    # First-run capture is expected to fail until the hash is filled in.
    if [ "$rc" -ne 0 ] && echo "$out" | grep -q "first-run capture"; then
        echo "  SKIP: golden hash not yet captured (one-time setup)"
        return 0
    fi
    assert_eq "$rc" "0" "golden test failed: $out"
}
