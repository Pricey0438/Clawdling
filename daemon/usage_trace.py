"""Extracted trace→payload composition.

Re-export of the logic currently embedded in clawdmeter-net.py so that the
balance simulator (tools/balance_sim/) can import it without spinning up
the HTTP server. The shapes are exactly what the firmware's parse_json
expects on the wire.
"""
import datetime
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

SCANNER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "session-scanner.sh")
CREDS_PATH = os.path.expanduser("~/.claude/.credentials.json")


def scan_sessions():
    """Shell into session-scanner.sh and return the parsed sessions list.

    Errors degrade silently to an empty list.
    """
    if not os.path.exists(SCANNER):
        return []
    try:
        result = subprocess.run(
            ["bash", "-c", f'source "{SCANNER}" && scan_active_sessions'],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            return []
        return json.loads(result.stdout.strip() or "[]")
    except (subprocess.SubprocessError, json.JSONDecodeError):
        return []


def read_oauth_token():
    """Read OAuth access token from ~/.claude/.credentials.json. Returns '' on failure."""
    try:
        with open(CREDS_PATH) as f:
            data = json.load(f)
        return data.get("claudeAiOauth", {}).get("accessToken", "") or data.get("accessToken", "")
    except (OSError, json.JSONDecodeError):
        return ""


def poll_anthropic_usage(token=None):
    """Probe /v1/messages for ratelimit headers. Returns a usage dict or {} on failure."""
    if token is None:
        token = read_oauth_token()
    if not token:
        return {}
    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=json.dumps({
            "model": "claude-haiku-4-5-20251001",
            "max_tokens": 1,
            "messages": [{"role": "user", "content": "."}],
        }).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "content-type": "application/json",
            "user-agent": "clawdmeter-net/1.0",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            headers = dict(resp.getheaders())
    except urllib.error.HTTPError as e:
        headers = dict(e.headers)
    except (urllib.error.URLError, TimeoutError) as e:
        print(f"[anthropic-poll] network error: {e}", file=sys.stderr)
        return {}

    def _h(name, default=""):
        lowered = {k.lower(): v for k, v in headers.items()}
        return lowered.get(name.lower(), default)

    s5h_util  = _h("anthropic-ratelimit-unified-5h-utilization", "")
    s5h_reset = _h("anthropic-ratelimit-unified-5h-reset", "")
    s7d_util  = _h("anthropic-ratelimit-unified-7d-utilization", "")
    s7d_reset = _h("anthropic-ratelimit-unified-7d-reset", "")
    status    = _h("anthropic-ratelimit-unified-5h-status", "")

    if not s5h_util and not s7d_util:
        print("[anthropic-poll] no ratelimit headers (token expired / API change?)",
              file=sys.stderr)
        return {}

    now = int(time.time())

    def _util_pct(raw):
        try:
            return float(raw) * 100.0
        except (ValueError, TypeError):
            return 0.0

    def _reset_mins(raw):
        try:
            secs = int(raw) - now
            return max(0, secs // 60)
        except (ValueError, TypeError):
            return 0

    return {
        "session_pct": _util_pct(s5h_util),
        "session_reset_mins": _reset_mins(s5h_reset),
        "weekly_pct": _util_pct(s7d_util),
        "weekly_reset_mins": _reset_mins(s7d_reset),
        "status": status or "unknown",
    }


def local_tz_offset_seconds(when=None):
    """Signed seconds-from-UTC for the local zone at `when` (default: now)."""
    when = when or datetime.datetime.now()
    return int(when.astimezone().utcoffset().total_seconds())


def compose_wire_payload(ts, tz_off, usage, sessions):
    """Produce the BLE/HTTP wire-format usage dict the firmware's parse_json expects."""
    return {
        "ts":  ts,
        "tz":  tz_off,
        "s":   usage.get("session_pct", 0.0),
        "sr":  usage.get("session_reset_mins", -1),
        "w":   usage.get("weekly_pct", 0.0),
        "wr":  usage.get("weekly_reset_mins", -1),
        "st":  usage.get("status", "unknown"),
        "ok":  bool(usage),
        "ses": sessions or [],
    }
