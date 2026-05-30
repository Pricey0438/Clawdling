#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_SH="$DIR/../../install.sh"

# Helper: run install.sh in a fake HOME and inspect the resulting settings.
_run_install() {
    local tmp="$1"
    mkdir -p "$tmp/.claude"
    [ -f "$tmp/.claude/settings.json" ] || echo '{}' > "$tmp/.claude/settings.json"
    # We want install_hooks() in isolation, not the full daemon install
    # (no systemd, no dependency checks). Source install.sh and call the
    # function directly. install.sh has `set -e` at the top which would
    # exit the test on any non-zero — wrap in a subshell with set +e for
    # the source step.
    (
        set +e
        HOME="$tmp"
        CC_SETTINGS="$tmp/.claude/settings.json"
        HOOK_SCRIPT="$DIR/../hook-state.sh"
        HOOK_TAG_PREFIX="$DIR/../hook-state.sh"
        # Source install.sh but suppress its top-level execution by sourcing
        # only the function definitions. Easiest: source it and rely on
        # the fact that the [ "${1:-}" = "--uninstall" ] check fails and
        # the "echo === Install ===" branch starts running, then fails on
        # missing curl/awk/bluetoothctl checks. To avoid that, just inline
        # the install_hooks call in a stripped harness:
        source <(sed -n '/^install_hooks()/,/^}/p' "$INSTALL_SH")
        source <(sed -n '/^uninstall_hooks()/,/^}/p' "$INSTALL_SH")
        install_hooks >/dev/null 2>&1
    )
}

_count_clawdmeter_entries() {
    local settings="$1" key="$2" prefix="$3"
    jq --arg p "$prefix" --arg k "$key" '
        (.hooks[$k] // []) | map(.hooks // [] | map(select((.command // "") | startswith($p)))) | flatten | length
    ' "$settings"
}

test_install_adds_all_six_hook_keys() {
    local tmp; tmp=$(mktemp -d)
    _run_install "$tmp"
    local prefix; prefix="$DIR/../hook-state.sh"
    local got_up; got_up=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" UserPromptSubmit "$prefix")
    local got_pt; got_pt=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" PostToolUse "$prefix")
    local got_nf; got_nf=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" Notification "$prefix")
    local got_st; got_st=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" Stop "$prefix")
    local got_pc; got_pc=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" PreCompact "$prefix")
    local got_oc; got_oc=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" PostCompact "$prefix")
    rm -rf "$tmp"
    assert_eq "$got_up" "1" "UserPromptSubmit has exactly 1 clawdmeter entry"
    assert_eq "$got_pt" "1" "PostToolUse has exactly 1 clawdmeter entry"
    assert_eq "$got_nf" "1" "Notification has exactly 1 clawdmeter entry"
    assert_eq "$got_st" "1" "Stop has exactly 1 clawdmeter entry"
    assert_eq "$got_pc" "1" "PreCompact has exactly 1 clawdmeter entry"
    assert_eq "$got_oc" "1" "PostCompact has exactly 1 clawdmeter entry"
}

test_install_is_idempotent() {
    local tmp; tmp=$(mktemp -d)
    _run_install "$tmp"
    _run_install "$tmp"  # second run must not duplicate
    local prefix; prefix="$DIR/../hook-state.sh"
    local got_pt; got_pt=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" PostToolUse "$prefix")
    rm -rf "$tmp"
    assert_eq "$got_pt" "1" "double-install leaves exactly 1 entry, not 2"
}

test_install_post_tool_use_command_is_thinking() {
    local tmp; tmp=$(mktemp -d)
    _run_install "$tmp"
    local cmd; cmd=$(jq -r '.hooks.PostToolUse[0].hooks[0].command' "$tmp/.claude/settings.json")
    rm -rf "$tmp"
    case "$cmd" in
        *"hook-state.sh thinking") assert_eq "ok" "ok" "PostToolUse command tail is ' thinking'" ;;
        *) assert_eq "$cmd" "<...hook-state.sh thinking>" "PostToolUse should invoke hook-state.sh with 'thinking'" ;;
    esac
}

test_install_notification_command_is_waiting() {
    local tmp; tmp=$(mktemp -d)
    _run_install "$tmp"
    local cmd; cmd=$(jq -r '.hooks.Notification[0].hooks[0].command' "$tmp/.claude/settings.json")
    rm -rf "$tmp"
    case "$cmd" in
        *"hook-state.sh waiting") assert_eq "ok" "ok" "Notification command tail is ' waiting'" ;;
        *) assert_eq "$cmd" "<...hook-state.sh waiting>" "Notification should invoke hook-state.sh with 'waiting'" ;;
    esac
}

test_uninstall_removes_all_six_hook_keys() {
    local tmp; tmp=$(mktemp -d)
    _run_install "$tmp"
    # Now run uninstall in a similar subshell
    (
        set +e
        HOME="$tmp"
        CC_SETTINGS="$tmp/.claude/settings.json"
        HOOK_SCRIPT="$DIR/../hook-state.sh"
        HOOK_TAG_PREFIX="$DIR/../hook-state.sh"
        source <(sed -n '/^install_hooks()/,/^}/p' "$INSTALL_SH")
        source <(sed -n '/^uninstall_hooks()/,/^}/p' "$INSTALL_SH")
        uninstall_hooks >/dev/null 2>&1
    )
    local prefix; prefix="$DIR/../hook-state.sh"
    local got_up; got_up=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" UserPromptSubmit "$prefix")
    local got_pt; got_pt=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" PostToolUse "$prefix")
    local got_nf; got_nf=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" Notification "$prefix")
    local got_st; got_st=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" Stop "$prefix")
    local got_pc; got_pc=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" PreCompact "$prefix")
    local got_oc; got_oc=$(_count_clawdmeter_entries "$tmp/.claude/settings.json" PostCompact "$prefix")
    rm -rf "$tmp"
    assert_eq "$got_up" "0" "UserPromptSubmit has 0 clawdmeter entries after uninstall"
    assert_eq "$got_pt" "0" "PostToolUse has 0 clawdmeter entries after uninstall"
    assert_eq "$got_nf" "0" "Notification has 0 clawdmeter entries after uninstall"
    assert_eq "$got_st" "0" "Stop has 0 clawdmeter entries after uninstall"
    assert_eq "$got_pc" "0" "PreCompact has 0 clawdmeter entries after uninstall"
    assert_eq "$got_oc" "0" "PostCompact has 0 clawdmeter entries after uninstall"
}
