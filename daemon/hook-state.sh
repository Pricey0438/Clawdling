#!/bin/bash
# Receives Claude Code hook event JSON on stdin; writes the session state
# atomically to ~/.cache/clawdmeter/state/<sid>. Always exits 0 so a
# malformed payload or missing jq never breaks Claude Code.
state="${1:-}"
payload=$(cat)
[ -z "$state" ] && exit 0
sid=$(printf '%s' "$payload" | jq -r '.session_id // empty' 2>/dev/null) || exit 0
[ -z "$sid" ] && exit 0
dir="$HOME/.cache/clawdmeter/state"
mkdir -p "$dir" || exit 0
now=$(date +%s)
tmp=$(mktemp "$dir/.tmp.XXXX") || exit 0
printf '%s\n%s\n' "$state" "$now" > "$tmp"
mv -f "$tmp" "$dir/$sid"

# Minimum-display marker for fast compacts: a /compact that completes in
# under one daemon tick (TICK=5s) would otherwise flash by between polls.
# The marker holds a deadline; the scanner returns "compacting" until then,
# even if PostCompact already overwrote the state file to "waiting".
# Long /compacts are handled by sticky state in session-scanner.sh, not by
# this marker.
COMPACT_STICKY_SEC=5
if [ "$state" = "compacting" ]; then
    echo $(( now + COMPACT_STICKY_SEC )) > "$dir/$sid.compacting"
fi
exit 0
