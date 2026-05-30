#!/bin/bash
# Minimal bash test runner: each test file in this dir prefixed test-* is sourced,
# and any function prefixed test_ inside it is invoked. PASS/FAIL counted.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
PASS=0; FAIL=0; FAILED=()

assert_eq() {
    local got="$1" want="$2" msg="${3:-}"
    if [ "$got" = "$want" ]; then return 0; fi
    echo "  FAIL: $msg" >&2
    echo "    want: <$want>" >&2
    echo "    got:  <$got>" >&2
    __TEST_FAILS=$((__TEST_FAILS + 1))
    return 1
}

run_tests_in_file() {
    local file="$1"
    # Capture pre-existing test_* functions so we only run the ones this
    # file defines (sourced functions persist in the parent shell and
    # would otherwise re-run on every subsequent file).
    local before after newfns
    before=$(declare -F | awk '$3 ~ /^test_/ {print $3}' | sort)
    # shellcheck disable=SC1090
    source "$file"
    after=$(declare -F | awk '$3 ~ /^test_/ {print $3}' | sort)
    newfns=$(comm -13 <(echo "$before") <(echo "$after"))
    while IFS= read -r fn; do
        [ -z "$fn" ] && continue
        __TEST_FAILS=0
        "$fn" || true
        if [ "$__TEST_FAILS" -eq 0 ]; then
            PASS=$((PASS+1)); echo "  PASS $fn"
        else
            FAIL=$((FAIL+1)); FAILED+=("$(basename "$file")::$fn")
            echo "  FAIL $fn ($__TEST_FAILS assertion(s))"
        fi
    done <<< "$newfns"
}

for f in "$DIR"/test-*.sh; do
    [ -f "$f" ] || continue
    echo "== $(basename "$f") =="
    run_tests_in_file "$f"
done

echo
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    printf '  - %s\n' "${FAILED[@]}"
    exit 1
fi
