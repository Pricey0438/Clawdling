#!/bin/bash
# Sourceable library: scans ~/.claude/projects/ jsonls and produces a
# session JSON array for the BLE payload. State inference may be
# overridden by hook-written files in ~/.cache/clawdmeter/state/.

STATE_DIR="$HOME/.cache/clawdmeter/state"
ACTIVE_WINDOW_SEC=1800   # 30 min: jsonls modified within this window count as active
IDLE_AFTER_SEC=60        # last entry older than this overrides to idle (used only by infer_state_from_jsonl fallback for no-hook installs)
HOOK_STALE_SEC=300       # hook timestamps within this window are trusted absolutely (5 min)
STALE_OVERRIDE_SEC=300   # if both hook AND jsonl are this stale → process probably died → idle (5 min)
MAX_SESSIONS=6
MAX_NAME=22

# Echo state ("thinking" | "waiting" | "idle") for a session jsonl file.
# Args: $1 = jsonl path
infer_state_from_jsonl() {
    local file="$1"
    [ -r "$file" ] || { echo "idle"; return; }

    local now mtime age
    now=$(date +%s)
    mtime=$(stat -c %Y "$file" 2>/dev/null || echo 0)
    age=$(( now - mtime ))
    if [ "$age" -gt "$IDLE_AFTER_SEC" ]; then
        echo "idle"; return
    fi

    # Tail last 8KB and walk lines bottom-up. First well-formed line wins.
    local last_type last_stop
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        last_type=$(echo "$line" | jq -r '.type // empty' 2>/dev/null) || continue
        [ -z "$last_type" ] && continue
        if [ "$last_type" = "assistant" ]; then
            last_stop=$(echo "$line" | jq -r '.message.stop_reason // empty' 2>/dev/null)
        fi
        break
    done < <(tail -c 8192 "$file" 2>/dev/null | tac)

    case "$last_type" in
        user|tool_result) echo "thinking" ;;
        assistant)
            case "$last_stop" in
                tool_use) echo "thinking" ;;
                end_turn|"") echo "waiting" ;;
                *) echo "waiting" ;;
            esac ;;
        *) echo "idle" ;;
    esac
}

# Returns the context-window limit (200000 or 1000000) for a session.
# Caches the 1M-tier result at $STATE_DIR/$sid.tier so subsequent scans
# don't re-walk the jsonl. The cache is a one-way ratchet: once a session
# is observed to have crossed 200K, it stays at 1M-tier for the rest of
# that session's lifetime.
#
# Args: $1 = sid, $2 = jsonl path
# Echoes: 200000 or 1000000
detect_tier_for_session() {
    local sid="$1" file="$2"
    local line in_t cc_t cr_t total

    # 1. Cached marker
    local marker="$STATE_DIR/$sid.tier"
    if [ -r "$marker" ]; then
        local cached; { read -r cached; } < "$marker" || true
        if [ "$cached" = "1m" ]; then
            echo 1000000
            return
        fi
    fi

    # 2. Scan jsonl line-by-line for an assistant entry > 200K tokens.
    # First match wins, write the marker, return 1M.
    [ -r "$file" ] || { echo 200000; return; }
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$line" in
            *'"type":"assistant"'*) ;;
            *) continue ;;
        esac
        in_t=$(echo "$line" | jq -r '.message.usage.input_tokens // 0' 2>/dev/null) || continue
        cc_t=$(echo "$line" | jq -r '.message.usage.cache_creation_input_tokens // 0' 2>/dev/null)
        cr_t=$(echo "$line" | jq -r '.message.usage.cache_read_input_tokens // 0' 2>/dev/null)
        total=$(( in_t + cc_t + cr_t ))
        if [ "$total" -gt 200000 ]; then
            mkdir -p "$STATE_DIR"
            printf '1m\n' > "$marker"
            echo 1000000
            return
        fi
    done < "$file"

    # 3. No turn exceeded 200K — default tier, do not cache.
    echo 200000
}

# Echo integer 0..100 representing context-window fill % for a session.
# Echoes 0 if no assistant entry exists.
#
# Walks the last 32KB of the jsonl bottom-up, tracking the running max of
# `input + cache_creation + cache_read` across assistant entries. If a
# `user` entry with `isCompactSummary: true` is encountered first, its
# content length (chars / 4) is used as a *floor* for the running max AND
# the walk stops — everything before the compact summary was discarded
# from the live context.
#
# The denominator is determined by detect_tier_for_session (200K or 1M)
# so 1M-tier sessions don't render at >100% when crossing 200K tokens.
#
# Args: $1 = jsonl path, $2 = sessionId (passed to detect_tier_for_session)
context_pct_from_jsonl() {
    local file="$1" sid="$2"
    [ -r "$file" ] || { echo 0; return; }

    local line t in_t cc_t cr_t total chars is_compact floor
    local running_max=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        t=$(echo "$line" | jq -r '.type // empty' 2>/dev/null) || continue
        if [ "$t" = "assistant" ]; then
            in_t=$(echo "$line" | jq -r '.message.usage.input_tokens // 0' 2>/dev/null)
            cc_t=$(echo "$line" | jq -r '.message.usage.cache_creation_input_tokens // 0' 2>/dev/null)
            cr_t=$(echo "$line" | jq -r '.message.usage.cache_read_input_tokens // 0' 2>/dev/null)
            total=$(( in_t + cc_t + cr_t ))
            [ "$total" -gt "$running_max" ] && running_max=$total
        elif [ "$t" = "user" ]; then
            is_compact=$(echo "$line" | jq -r '.isCompactSummary // false' 2>/dev/null)
            if [ "$is_compact" = "true" ]; then
                chars=$(echo "$line" | jq -r '(.message.content // "" | if type == "array" then (map(.text // "") | join(" ")) else tostring end | length)' 2>/dev/null)
                chars=${chars:-0}
                floor=$(( chars / 4 ))
                [ "$floor" -gt "$running_max" ] && running_max=$floor
                break  # everything older was discarded by /compact
            fi
        fi
    done < <(tail -c 32768 "$file" 2>/dev/null | tac)

    [ "$running_max" -le 0 ] && { echo 0; return; }

    local limit; limit=$(detect_tier_for_session "$sid" "$file")
    echo $(( (running_max * 100 + limit / 2) / limit ))
}

# Decode "-home-user-projects-Project-Name" back to "Project Name"
# (Claude Code encodes / as -). We just take the final dash-delimited segment.
_decode_project_basename() {
    local encoded="$1"
    echo "${encoded##*-}"
}

# Pick the display name for a session. Priority:
#   1. latest `ai-title` value
#   2. project dir basename (decoded)
#   3. first 30 chars of first user prompt
#   4. sessionId prefix
# Result is capped to $MAX_NAME chars.
name_from_jsonl() {
    local file="$1"
    [ -r "$file" ] || { echo "unknown"; return; }

    local name
    # 1) latest ai-title (tac reads bottom-up; -m1 stops at first match from the end)
    name=$(tac "$file" 2>/dev/null | grep -m1 '"type":"ai-title"' | jq -r '.title // empty' 2>/dev/null)
    if [ -z "$name" ]; then
        # 2) project dir basename
        local dir; dir=$(basename "$(dirname "$file")")
        name=$(_decode_project_basename "$dir")
    fi
    if [ -z "$name" ]; then
        # 3) first user prompt (first 30 chars)
        name=$(grep -m1 '"type":"user"' "$file" 2>/dev/null \
            | jq -r '.message.content // empty' 2>/dev/null | head -c 30)
    fi
    if [ -z "$name" ]; then
        # 4) sessionId prefix
        name=$(basename "$file" .jsonl | head -c 8)
    fi

    # Cap to MAX_NAME chars (byte-wise; acceptable for ASCII names)
    echo "${name:0:$MAX_NAME}"
}

# Returns final state for a session using a 5-rule precedence chain.
# Args: $1 = jsonl path, $2 = sessionId
#
# Rule 1: sticky compacting marker (minimum-display for fast compacts)
# Rule 2: fresh hook (age <= HOOK_STALE_SEC) — trust absolutely
# Rule 3: hook + jsonl both stale (>= STALE_OVERRIDE_SEC) — process death → idle
# Rule 4: aging hook with fresh jsonl — still trust the hook
# Rule 5: no hook file at all — fall back to jsonl-tail inference (legacy
#         no-install case). This is the only remaining caller of
#         infer_state_from_jsonl().
state_for_session() {
    local file="$1" sid="$2"
    local now; now=$(date +%s)

    # Rule 1: sticky compacting marker
    local mark="$STATE_DIR/$sid.compacting"
    if [ -r "$mark" ]; then
        local until; { read -r until; } < "$mark" || true
        if [ -n "${until:-}" ] && [ "$now" -lt "$until" ]; then
            echo "compacting"
            return
        fi
    fi

    local hook_file="$STATE_DIR/$sid"
    if [ -r "$hook_file" ]; then
        local hstate hts hage
        { read -r hstate; read -r hts; } < "$hook_file" || true
        hage=$(( now - ${hts:-0} ))
        [ "$hage" -lt 0 ] && hage=0

        # Rule 2: fresh hook
        if [ "$hage" -le "$HOOK_STALE_SEC" ]; then
            echo "$hstate"
            return
        fi

        # Rule 3: hook stale AND jsonl stale → process-death override → idle
        local jmtime jage
        jmtime=$(stat -c %Y "$file" 2>/dev/null || echo 0)
        jage=$(( now - jmtime ))
        if [ "$jage" -ge "$STALE_OVERRIDE_SEC" ]; then
            echo "idle"
            return
        fi

        # Rule 4: hook aging but jsonl fresh → trust the hook anyway
        echo "$hstate"
        return
    fi

    # Rule 5: no hook file → fall back to inference (no-install case)
    infer_state_from_jsonl "$file"
}

_state_code() {
    case "$1" in
        thinking)   echo "t" ;;
        waiting)    echo "w" ;;
        compacting) echo "c" ;;
        *)          echo "i" ;;
    esac
}

# Emit one JSON object for a session jsonl (no trailing newline).
session_record() {
    local file="$1"
    # sessionId: prefer the value embedded in the JSON (this lets test
    # fixtures use scenario names instead of UUIDs), else fall back to
    # the filename (which in production IS the sessionId).
    local sid; sid=$(grep -m1 '"sessionId"' "$file" 2>/dev/null | jq -r '.sessionId // empty' 2>/dev/null)
    [ -z "$sid" ] && sid=$(basename "$file" .jsonl)
    local id; id="${sid:0:8}"
    local name; name=$(name_from_jsonl "$file")
    local pct; pct=$(context_pct_from_jsonl "$file" "$sid")
    local state; state=$(state_for_session "$file" "$sid")
    local code; code=$(_state_code "$state")
    local now mtime age
    now=$(date +%s); mtime=$(stat -c %Y "$file" 2>/dev/null || echo "$now")
    age=$(( now - mtime ))
    [ "$age" -lt 0 ] && age=0
    [ "$age" -gt 65535 ] && age=65535

    jq -nc \
        --arg id "$id" --arg n "$name" \
        --argjson c "$pct" --arg st "$code" --argjson a "$age" \
        '{id:$id, n:$n, c:$c, st:$st, a:$a}'
}

# Emit a JSON array of up to MAX_SESSIONS active sessions, most-recent first.
# Args: $1 = projects root (defaults to ~/.claude/projects)
scan_active_sessions() {
    local root="${1:-$HOME/.claude/projects}"
    local now; now=$(date +%s)
    local cutoff=$(( now - ACTIVE_WINDOW_SEC ))

    # Find candidate jsonls modified within the active window. Use stat for
    # mtime so we can sort newest-first.
    local sorted_files=()
    while IFS= read -r -d '' f; do
        local m; m=$(stat -c %Y "$f" 2>/dev/null || echo 0)
        [ "$m" -ge "$cutoff" ] || continue
        sorted_files+=("$m $f")
    done < <(find "$root" -maxdepth 2 -name '*.jsonl' -print0 2>/dev/null)

    # Newest first, capped. Skip blanks — `printf '%s\n'` on an empty array
    # still emits one newline, which would otherwise become a bogus record.
    local picked=()
    if [ "${#sorted_files[@]}" -gt 0 ]; then
        while IFS= read -r line; do
            [ -z "$line" ] && continue
            picked+=("${line#* }")
            [ "${#picked[@]}" -ge "$MAX_SESSIONS" ] && break
        done < <(printf '%s\n' "${sorted_files[@]}" | sort -rn)
    fi

    # Build records, track name collisions
    local records=() names=() ids=()
    for f in "${picked[@]}"; do
        local rec; rec=$(session_record "$f")
        records+=("$rec")
        names+=("$(echo "$rec" | jq -r .n)")
        ids+=("$(echo "$rec" | jq -r .id)")
    done

    # Disambiguate same names by appending " (<id4>)"
    local out=()
    for i in "${!records[@]}"; do
        local rec="${records[$i]}" nm="${names[$i]}" id="${ids[$i]}"
        local dup=0
        for j in "${!names[@]}"; do
            [ "$i" = "$j" ] && continue
            [ "${names[$j]}" = "$nm" ] && dup=1 && break
        done
        if [ "$dup" = "1" ]; then
            local newname; newname="${nm:0:$((MAX_NAME-7))} (${id:0:4})"
            rec=$(echo "$rec" | jq --arg n "$newname" '.n=$n')
        fi
        out+=("$rec")
    done

    # Compose array
    if [ "${#out[@]}" -eq 0 ]; then
        echo '[]'
    else
        printf '%s\n' "${out[@]}" | jq -sc '.'
    fi
}

# Firmware's BLE_BUF_SIZE is 512. Leave a small margin so a near-MTU
# payload doesn't get truncated by the GATT write path.
PAYLOAD_MAX=480

# Compose the full BLE payload from cached usage + session array.
# Drops the oldest sessions (tail of the most-recent-first array) until
# the result fits in $PAYLOAD_MAX bytes.
# Args: s sr w wr st_string ok_bool ses_json_array [ts_epoch_sec]
# Emits tz (signed seconds from UTC, e.g. PST=-28800, AEST=+36000) so firmware
# can compute local-midnight day boundaries (P1-7 / P1-8).
compose_ble_payload() {
    local s="$1" sr="$2" w="$3" wr="$4" st="$5" ok="$6" ses="$7" ts="${8:-$(date +%s)}"
    # `date +%z` returns ±HHMM; convert to signed seconds.
    local zstr=$(date +%z)
    local zsign="${zstr:0:1}" zh="${zstr:1:2}" zm="${zstr:3:2}"
    # Strip any leading zeros so arithmetic doesn't go octal on "08"/"09".
    local zhi=$((10#$zh)) zmi=$((10#$zm))
    local tz=$(( zhi * 3600 + zmi * 60 ))
    [ "$zsign" = "-" ] && tz=$(( -tz ))
    local payload trimmed=0 n
    while :; do
        payload=$(jq -nc \
            --argjson s  "$s"  --argjson sr "$sr" \
            --argjson w  "$w"  --argjson wr "$wr" \
            --arg     st "$st" --argjson ok "$ok" \
            --argjson ses "$ses" --argjson ts "$ts" \
            --argjson tz "$tz" \
            '{s:$s, sr:$sr, w:$w, wr:$wr, st:$st, ok:$ok, ses:$ses, ts:$ts, tz:$tz}')
        [ "${#payload}" -le "$PAYLOAD_MAX" ] && break
        n=$(echo "$ses" | jq 'length')
        [ "$n" -le 0 ] && break
        ses=$(echo "$ses" | jq -c 'del(.[-1])')
        trimmed=$((trimmed + 1))
    done
    if [ "$trimmed" -gt 0 ] && [ "$(type -t log)" = "function" ]; then
        log "Payload trimmed: dropped $trimmed oldest session(s) to fit ${PAYLOAD_MAX}B" >&2
    fi
    printf '%s' "$payload"
}
