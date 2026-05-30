#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="claude-usage-daemon"
SERVICE_FILE="$SCRIPT_DIR/daemon/$SERVICE_NAME.service"
USER_SERVICE_DIR="$HOME/.config/systemd/user"
CC_SETTINGS="$HOME/.claude/settings.json"
HOOK_SCRIPT="$SCRIPT_DIR/daemon/hook-state.sh"
HOOK_TAG_PREFIX="$SCRIPT_DIR/daemon/hook-state.sh"  # match by abs path prefix

uninstall_hooks() {
    [ -f "$CC_SETTINGS" ] || return 0
    command -v jq >/dev/null || { echo "  jq not installed, skipping hook removal"; return 0; }
    local tmp; tmp=$(mktemp)
    jq --arg p "$HOOK_TAG_PREFIX" '
        .hooks //= {} |
        (.hooks.UserPromptSubmit // []) as $up |
        (.hooks.PostToolUse // []) as $pt |
        (.hooks.Notification // []) as $nf |
        (.hooks.Stop // []) as $st |
        (.hooks.PreCompact // []) as $pc |
        (.hooks.PostCompact // []) as $oc |
        .hooks.UserPromptSubmit = ($up | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.PostToolUse = ($pt | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.Notification = ($nf | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.Stop = ($st | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.PreCompact = ($pc | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.PostCompact = ($oc | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0)))
    ' "$CC_SETTINGS" > "$tmp" && mv "$tmp" "$CC_SETTINGS"
    echo "  Hooks removed from $CC_SETTINGS"
}

install_hooks() {
    if ! command -v jq >/dev/null; then
        echo "  jq not installed — skipping hook install (polling fallback still works)"
        return 0
    fi
    if [ ! -f "$CC_SETTINGS" ]; then
        echo '{}' > "$CC_SETTINGS"
    fi
    cp "$CC_SETTINGS" "$CC_SETTINGS.clawdmeter.bak.$(date +%s)" || true
    local tmp; tmp=$(mktemp)
    jq --arg p "$HOOK_TAG_PREFIX" --arg thinking_cmd "$HOOK_SCRIPT thinking" \
       --arg waiting_cmd "$HOOK_SCRIPT waiting" \
       --arg compact_cmd "$HOOK_SCRIPT compacting" \
       --arg done_cmd "$HOOK_SCRIPT waiting" \
       --arg notify_cmd "$HOOK_SCRIPT waiting" '
        .hooks //= {} |
        .hooks.UserPromptSubmit //= [] |
        .hooks.PostToolUse //= [] |
        .hooks.Notification //= [] |
        .hooks.Stop //= [] |
        .hooks.PreCompact //= [] |
        .hooks.PostCompact //= [] |
        # remove any prior clawdmeter entries first (idempotent)
        .hooks.UserPromptSubmit = (.hooks.UserPromptSubmit | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.PostToolUse = (.hooks.PostToolUse | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.Notification = (.hooks.Notification | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.Stop = (.hooks.Stop | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.PreCompact = (.hooks.PreCompact | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        .hooks.PostCompact = (.hooks.PostCompact | map(
            .hooks = (.hooks // [] | map(select((.command // "") | startswith($p) | not)))
        ) | map(select(.hooks | length > 0))) |
        # append ours (PostToolUse = thinking heartbeat; Notification = waiting on permission prompt)
        .hooks.UserPromptSubmit += [{ "hooks": [{ "type": "command", "command": $thinking_cmd }] }] |
        .hooks.PostToolUse      += [{ "hooks": [{ "type": "command", "command": $thinking_cmd }] }] |
        .hooks.Notification     += [{ "hooks": [{ "type": "command", "command": $notify_cmd }] }] |
        .hooks.Stop             += [{ "hooks": [{ "type": "command", "command": $waiting_cmd }] }] |
        .hooks.PreCompact       += [{ "hooks": [{ "type": "command", "command": $compact_cmd }] }] |
        .hooks.PostCompact      += [{ "hooks": [{ "type": "command", "command": $done_cmd }] }]
    ' "$CC_SETTINGS" > "$tmp" && mv "$tmp" "$CC_SETTINGS"
    echo "  Hooks installed into $CC_SETTINGS"
}

install_network_daemon() {
    command -v openssl >/dev/null || { echo "Error: openssl is required for --network"; exit 1; }
    local svc="clawdmeter-net"
    local unit_src="$SCRIPT_DIR/daemon/$svc.service"
    local unit_dst="$USER_SERVICE_DIR/$svc.service"
    local daemon_py="$SCRIPT_DIR/daemon/clawdmeter-net.py"
    local cfg_dir="$HOME/.config/clawdmeter"
    local token_file="$cfg_dir/token"
    local env_file="$cfg_dir/env"

    mkdir -p "$cfg_dir" "$USER_SERVICE_DIR"
    chmod 700 "$cfg_dir"

    if [ ! -f "$token_file" ]; then
        (umask 077; openssl rand -base64 32 | tr -d '\n' > "$token_file")
        echo "  Generated bearer token at $token_file"
    else
        echo "  Reusing existing token at $token_file"
    fi
    (umask 077; printf 'TOKEN=%s\n' "$(cat "$token_file")" > "$env_file")

    sed "s|DAEMON_PATH|$daemon_py|" "$unit_src" > "$unit_dst"
    chmod +x "$daemon_py"

    systemctl --user daemon-reload
    systemctl --user enable --now "$svc.service"
    echo
    echo "Network daemon installed."
    echo "  Token: $(cat "$token_file")"
    echo "  Local URL: http://127.0.0.1:8443"
    echo
    echo "To expose over Tailscale Funnel (one-time per host):"
    echo "  tailscale funnel --bg --https=443 http://127.0.0.1:8443"
    echo "Then your public URL is:  https://\$(tailscale status --json | jq -r .Self.DNSName | sed 's/\\.$//')"
}

uninstall_network_daemon() {
    local svc="clawdmeter-net"
    systemctl --user disable --now "$svc.service" 2>/dev/null || true
    rm -f "$USER_SERVICE_DIR/$svc.service"
    systemctl --user daemon-reload 2>/dev/null || true
    echo "  Network daemon unit removed"
    # Do NOT delete the token file — re-install should keep it stable so the device doesn't need re-provisioning
}

rotate_network_token() {
    command -v openssl >/dev/null || { echo "Error: openssl is required for --rotate-token"; exit 1; }
    local cfg_dir="$HOME/.config/clawdmeter"
    local token_file="$cfg_dir/token"
    local env_file="$cfg_dir/env"
    mkdir -p "$cfg_dir"
    chmod 700 "$cfg_dir"
    (umask 077; openssl rand -base64 32 | tr -d '\n' > "$token_file")
    (umask 077; printf 'TOKEN=%s\n' "$(cat "$token_file")" > "$env_file")
    systemctl --user restart clawdmeter-net.service 2>/dev/null || true
    echo "Token rotated. New token:"
    cat "$token_file"; echo
    echo "Re-enter via the device's captive portal."
}

if [ "${1:-}" = "--uninstall" ]; then
    echo "=== Claude Usage Tracker - Uninstall ==="
    uninstall_hooks
    systemctl --user disable --now "$SERVICE_NAME" 2>/dev/null || true
    rm -f "$USER_SERVICE_DIR/$SERVICE_NAME.service"
    systemctl --user daemon-reload || true
    uninstall_network_daemon
    echo "Done."
    exit 0
fi

if [ "${1:-}" = "--network" ]; then
    echo "=== Clawdmeter Network Daemon - Install ==="
    install_network_daemon
    exit 0
fi

if [ "${1:-}" = "--rotate-token" ]; then
    rotate_network_token
    exit 0
fi

echo "=== Claude Usage Tracker - Install ==="
echo ""

echo "[1/4] Checking dependencies..."
for cmd in curl awk bluetoothctl busctl; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd is required but not installed"; exit 1; }
done
echo "  All required dependencies found"
command -v jq >/dev/null || echo "  WARN: jq not installed — hooks will be skipped"
echo ""

echo "[2/4] Installing systemd user service..."
mkdir -p "$USER_SERVICE_DIR"
DAEMON_BIN="$SCRIPT_DIR/daemon/$SERVICE_NAME.sh"
sed "s|DAEMON_PATH|${DAEMON_BIN}|g" "$SERVICE_FILE" > "$USER_SERVICE_DIR/$SERVICE_NAME.service"
systemctl --user daemon-reload

echo "[3/4] Installing Claude Code hooks..."
install_hooks

echo "[4/4] Enabling service..."
systemctl --user enable "$SERVICE_NAME"

echo ""
echo "=== Done! ==="
echo ""
echo "Useful commands:"
echo "  systemctl --user status $SERVICE_NAME       # check status"
echo "  journalctl --user -u $SERVICE_NAME -f       # view logs"
echo "  systemctl --user restart $SERVICE_NAME      # restart"
echo "  ./install.sh --uninstall                    # remove hooks + service"
