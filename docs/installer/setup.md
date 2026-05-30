# Clawdmeter Installer Setup

This guide is for the person installing the Clawdmeter daemon on a
Linux or macOS machine. For the end-user quickstart, see
`docs/buyer/quickstart.md`.

## Two daemon flavors

Clawdmeter has two daemons that read the same source-of-truth files
and serve usage data in different ways. Install whichever matches how
you want to connect Clawdmeter to your computer.

| Flavor | When to pick |
|--------|--------------|
| **BLE** (`daemon/claude-usage-daemon.sh`) | Clawdmeter is on the same desk as the computer running Claude Code. Talks over Bluetooth, no network exposure. |
| **Network** (`daemon/clawdmeter-net.py`) | Clawdmeter is on WiFi, possibly across the house or remote. Talks over HTTPS, front with Tailscale Funnel for remote access. |

You can install both — Clawdmeter prefers fresh network data when
available, with BLE as a fallback.

## Install the BLE daemon

```bash
cd /path/to/Clawdmeter
./install.sh
systemctl --user start claude-usage-daemon
systemctl --user status claude-usage-daemon
```

The installer adds Claude Code hooks to `~/.claude/settings.json` and
sets up the systemd-user unit. First connect can take a minute as the
daemon scans for the device by name (`Claude Controller`).

## Install the network daemon

```bash
cd /path/to/Clawdmeter
./install.sh --network
```

This installs `daemon/clawdmeter-net.py` as a systemd-user service
listening on `127.0.0.1:8443`. To expose it remotely:

```bash
tailscale funnel --bg --https=443 http://127.0.0.1:8443
```

The Funnel URL is what you give Clawdmeter in the captive-portal setup
form. The bearer token shown by `install.sh --network` goes in the
"token" field of the same form.

## Token rotation

If you ever need to rotate the bearer token:

```bash
./install.sh --rotate-token
```

Then re-enter the new token on Clawdmeter via captive portal (factory-
reset first to surface the form again, if the device is already
provisioned).

## Verifying the daemon is reachable

**BLE daemon:**
```bash
journalctl --user -u claude-usage-daemon -n 50
```
Look for `Connected` and `payload sent` lines. Also check
`~/.cache/clawdmeter/firmware-version` exists after a successful
connect (Wave 6a daemon reads the device's version characteristic and
caches it here).

**Network daemon:**
```bash
curl -sk -H "Authorization: Bearer $TOKEN" \
    https://your.tailnet.ts.net/state | jq .
```
Expect a JSON blob with `s`, `sr`, `w`, `wr`, `st`, `ok`, `ses` fields.
With the Wave 6a updates, you'll also see `fw`, `fws`, and `d` fields
identifying the running firmware version, firmware SHA, and daemon SHA
respectively.

## Factory reset

Sometimes you need to wipe Clawdmeter's stored WiFi credentials or
daemon URL. The gesture is the same on both supported boards:

1. Unplug Clawdmeter from USB power.
2. Press and hold the **BOOT** button (the larger of the two side buttons).
3. While still holding, plug USB power back in.
4. Continue holding for at least 5 seconds.

The serial log (if you have USB connected to a computer) will show:
```
setup: BOOT held 5s — wiping config
setup: no config — entering provisioning
```

The device will reboot into provisioning mode and the
`Clawdmeter-XXXX` AP will be available within ~10 seconds (the XXXX is
4 hex digits of the device's MAC, unique per device).

## Updating the daemon

```bash
cd /path/to/Clawdmeter
git pull
./install.sh           # or --network, matching what you installed
systemctl --user restart claude-usage-daemon
```

The installer is idempotent. Settings are preserved.

## Updating the firmware

USB re-flash is currently the only path (OTA is Wave 6b, not yet
shipped):

```bash
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0
# or for the 1.8 board:
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/ttyACM0
```

On macOS the port is typically `/dev/cu.usbmodem*`; on Linux it's
`/dev/ttyACM0`.

Confirm the running firmware version via the menu screen (cycle BOOT
to reach "MENU" — the Version row shows e.g. `v0.6.0 8243017`).

## Help

Issues: https://github.com/Pricey0438/Clawdling/issues
