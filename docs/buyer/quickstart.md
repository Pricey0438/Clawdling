# Clawdmeter Quickstart

Welcome! This guide takes you from "just unboxed" to "watching your pet"
in about five minutes.

## What's in the box

- The Clawdmeter device (a small round screen on a board)
- A USB-C cable
- (Optional) a small battery, if you bought the battery kit

## Plug it in

Connect the USB-C cable to any USB power source — a computer, a phone
charger, or a power bank. Within a few seconds you should see the
screen light up with a Claude pet animation. This is the splash screen.

## Set me up

After about a minute, if Clawdmeter isn't talking to anything yet, the
screen will show a QR code under the headline "Hi! Set me up."

**Scan the QR with your phone camera.** It opens this guide on the web —
follow the section below that matches your setup.

You have three ways to use Clawdmeter:

### Option A — WiFi (recommended for most people)

Best when: you want to put Clawdmeter on your desk and use Claude Code
on a different machine, like a laptop you carry around.

1. On your phone, join the WiFi network named `Clawdmeter-XXXX`
   (the XXXX is a 4-character code unique to your device).
2. Your phone should auto-open a setup form. If not, browse to
   `http://192.168.4.1`.
3. Enter your home WiFi name and password, plus the URL and bearer
   token from your daemon install (see Option A's installer guide at
   `docs/installer/setup.md`).
4. Submit. Clawdmeter will join your home WiFi and start polling the
   daemon. Within 30 seconds you should see your pet appear.

### Option B — Bluetooth (advanced)

Best when: you're running the Clawdmeter daemon on the same Linux or
macOS computer you use Claude Code on.

1. Install the daemon following `docs/installer/setup.md`.
2. The daemon will discover Clawdmeter over Bluetooth and pair
   automatically.
3. Within a minute you should see your pet appear.

### Option C — Just looking (no daemon)

Best when: you just want to see the cute pet.

Do nothing. Clawdmeter shows the splash and a pet at zero usage. Cycle
screens with the BOOT button on the side.

## Success check

You should see, within five minutes of plugging in:

- The splash animation, then
- A "PET" screen showing a Claude pet with a name and a level number

If you don't, see the troubleshooting list below.

## Buttons

Clawdmeter has two physical buttons:

- **BOOT** (the larger one) — cycle through screens
- **PWR / middle** — also cycles screens; on the splash, cycles between
  pet animations

## Factory reset

If you ever want to wipe Clawdmeter's settings and start over:

1. Unplug the USB cable.
2. Hold down the BOOT button.
3. While still holding, plug the USB cable back in.
4. Keep holding for 5 seconds.

All WiFi settings and daemon credentials are wiped. The device boots
to the splash, then about a minute later returns to the "Hi! Set me up."
QR — unless a daemon connects over Bluetooth in that minute, in which
case the QR doesn't reappear and you can use the device as normal.

## Troubleshooting

**Screen stays black.** Try a different USB cable or power source — some
phone chargers are too weak to power Clawdmeter cleanly. The device
needs about 500mA.

**Splash forever, never shows OOBE.** The OOBE only shows when no data
source is configured. If you already provisioned WiFi or paired
Bluetooth and want to see the QR again, factory-reset (see above).

**QR doesn't scan.** Move closer; the cells are small. If your phone
can't decode it, the URL in the small text below the QR is the same
link — type it in.

**Pet never appears.** Check that the daemon on your computer is
running and reachable. See `docs/installer/setup.md` "Verifying the
daemon is reachable."

## Get help

Issues: https://github.com/Pricey0438/Clawdling/issues
