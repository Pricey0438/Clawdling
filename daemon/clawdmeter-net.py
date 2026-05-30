#!/usr/bin/env python3
"""Clawdmeter network daemon — HTTP service for the firmware over WiFi.

Binds to 127.0.0.1 in plain HTTP. Tailscale Funnel terminates TLS at the
edge and proxies https://<host>.ts.net -> http://127.0.0.1:<port>.

Env:
    PORT   default 8443
    TOKEN  required; bearer token clients must present
"""
import hmac
import html
import json
import os
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from usage_trace import (
    scan_sessions, read_oauth_token, poll_anthropic_usage,
    local_tz_offset_seconds, compose_wire_payload,
)

PORT = int(os.environ.get("PORT", "8443"))
TOKEN = os.environ.get("TOKEN", "")

SCAN_INTERVAL = 5  # seconds between session re-scans
POLL_INTERVAL = 60  # seconds between Anthropic usage polls

# In-memory snapshot — sessions populated by background scanner thread.
# Internal long keys kept for readability; wire format is composed in do_GET.
_snapshot = {"ts": 0, "usage": {}, "sessions": []}


def _scan_loop():
    """Background thread: refresh the snapshot every SCAN_INTERVAL seconds."""
    while True:
        _snapshot["sessions"] = scan_sessions()
        _snapshot["ts"] = int(time.time())
        time.sleep(SCAN_INTERVAL)


_refresh_now = threading.Event()


def _anthropic_loop():
    """Background thread: poll every POLL_INTERVAL or whenever _refresh_now is set."""
    while True:
        usage = poll_anthropic_usage()
        if usage:
            _snapshot["usage"] = usage
            _snapshot["ts"] = int(time.time())
        _refresh_now.wait(POLL_INTERVAL)
        _refresh_now.clear()


def _check_auth(handler):
    """Return True iff Authorization header has a constant-time-matching bearer token."""
    if not TOKEN:
        return False
    hdr = handler.headers.get("Authorization", "")
    if not hdr.startswith("Bearer "):
        return False
    return hmac.compare_digest(hdr[7:], TOKEN)


def render_graduate_svg(params):
    """Render a graduate bio card as an SVG document (stdlib only — no Pillow).

    Query parameters (all optional, defaults shown):
        name      — pet name           (default: "(unnamed)")
        species   — species name       (default: "(unknown)")
        born      — ISO date born      (default: "?")
        grad      — ISO date graduated (default: "?")
        peak_xp   — peak XP integer    (default: "0")
        quote     — signature quote    (default: "")
        shiny     — "1" for shiny badge (default: "0")

    Returns the SVG as a UTF-8 encoded bytes object.
    """
    def _p(key, default):
        return params.get(key, [default])[0]

    name     = _p("name",    "(unnamed)")
    species  = _p("species", "(unknown)")
    born     = _p("born",    "?")
    grad     = _p("grad",    "?")
    peak_xp  = _p("peak_xp", "0")
    quote    = _p("quote",   "")
    shiny    = _p("shiny",   "0") == "1"

    # XML-escape every user-supplied string before embedding in SVG.
    name_s    = html.escape(name)
    species_s = html.escape(species)
    born_s    = html.escape(born)
    grad_s    = html.escape(grad)
    peak_xp_s = html.escape(str(peak_xp))
    quote_s   = html.escape(quote)

    bg_color   = "#1a1a1a"
    accent     = "#ff8e72" if shiny else "#e07b4a"
    text_color = "#f0f0f0"
    dim_color  = "#a0a0a0"
    stripe     = "#2a2a2a"

    shiny_badge = ""
    if shiny:
        shiny_badge = (
            f'<text x="760" y="58" text-anchor="end" '
            f'font-family="sans-serif" font-size="18" fill="{accent}">&#9733; shiny</text>'
        )

    # Decorative corner mark — small claw-like arc (always visible).
    corner_mark = (
        f'<path d="M 740,380 Q 760,360 780,380 Q 760,400 740,380 Z" '
        f'fill="none" stroke="{accent}" stroke-width="1.5" opacity="0.4"/>'
    )

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 420" width="800" height="420">
  <!-- background -->
  <rect width="800" height="420" fill="{bg_color}"/>
  <!-- subtle inner panel -->
  <rect x="20" y="20" width="760" height="380" rx="8" ry="8" fill="{stripe}" opacity="0.4"/>
  <!-- top accent bar -->
  <rect x="0" y="0" width="800" height="5" fill="{accent}"/>
  <!-- pet name -->
  <text x="40" y="100" font-family="Georgia,serif" font-size="58" font-weight="bold" fill="{text_color}">{name_s}</text>
  <!-- species -->
  <text x="40" y="138" font-family="Arial,sans-serif" font-size="24" fill="{dim_color}">{species_s}</text>
  {shiny_badge}
  <!-- divider -->
  <line x1="40" y1="168" x2="760" y2="168" stroke="{dim_color}" stroke-width="1" opacity="0.3"/>
  <!-- stats block -->
  <text x="40" y="212" font-family="Arial,sans-serif" font-size="20" fill="{text_color}">Born: <tspan font-weight="bold">{born_s}</tspan></text>
  <text x="40" y="248" font-family="Arial,sans-serif" font-size="20" fill="{text_color}">Graduated: <tspan font-weight="bold">{grad_s}</tspan></text>
  <text x="40" y="284" font-family="Arial,sans-serif" font-size="20" fill="{text_color}">Peak XP earned: <tspan font-weight="bold">{peak_xp_s}</tspan></text>
  <!-- quote speech bubble -->
  <rect x="36" y="308" width="{min(720, max(200, len(quote) * 11 + 32))}" height="46" rx="6" ry="6" fill="{stripe}" opacity="0.8"/>
  <text x="52" y="338" font-family="Georgia,serif" font-size="22" font-style="italic" fill="{accent}">&#8220;{quote_s}&#8221;</text>
  <!-- footer watermark -->
  <text x="760" y="400" text-anchor="end" font-family="Arial,sans-serif" font-size="14" fill="{dim_color}" opacity="0.6">clawdmeter</text>
  {corner_mark}
</svg>"""
    return svg.encode("utf-8")


_GRADUATE_SVG_SAMPLE_PARAMS = {
    "name":    ["Bytey"],
    "species": ["Cronfox"],
    "born":    ["2026-04-15"],
    "grad":    ["2026-05-25"],
    "peak_xp": ["12500"],
    "quote":   ["oh, you’re here"],
    "shiny":   ["1"],
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))

    def _deny(self):
        self.send_response(401)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _not_found(self):
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        if self.path != "/refresh":
            self._not_found()
            return
        if not _check_auth(self):
            self._deny()
            return
        _refresh_now.set()
        body = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path   = parsed.path

        if path == "/state":
            if not _check_auth(self):
                self._deny()
                return
            wire = compose_wire_payload(
                ts=_snapshot.get("ts", 0),
                tz_off=local_tz_offset_seconds(),
                usage=_snapshot.get("usage", {}),
                sessions=_snapshot.get("sessions", []),
            )
            body = json.dumps(wire).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif path == "/graduate.svg":
            if not _check_auth(self):
                self._deny()
                return
            params = urllib.parse.parse_qs(parsed.query)
            body = render_graduate_svg(params)
            self.send_response(200)
            self.send_header("Content-Type", "image/svg+xml; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)

        elif path == "/graduate.svg/sample":
            if not _check_auth(self):
                self._deny()
                return
            body = render_graduate_svg(_GRADUATE_SVG_SAMPLE_PARAMS)
            self.send_response(200)
            self.send_header("Content-Type", "image/svg+xml; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)

        else:
            self._not_found()


def main():
    if not TOKEN:
        print("ERROR: TOKEN env var is required", file=sys.stderr)
        sys.exit(2)
    threading.Thread(target=_scan_loop, daemon=True).start()
    threading.Thread(target=_anthropic_loop, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"listening on 127.0.0.1:{PORT}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
