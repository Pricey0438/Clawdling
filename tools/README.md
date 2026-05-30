# Splash animation tools

Two-step pipeline for getting third-party 20×20 pixel animations onto the device.

## 1. Scrape

```bash
node scrape_claudepix.js
```

Fetches the manifest from `claudepix.vercel.app/app.js`, then each animation's
HTML file, evaluates the embedded JS in a Node VM context (loading the same
`creature-engine.js` the site uses), and writes resolved frame data to
`tools/claudepix_data/*.json`.

Each output file looks like:
```json
{
  "filename": "idle_breathe.html",
  "name": "idle breathe",
  "category": "Idle",
  "description": "...",
  "frame_count": 17,
  "frames": [{ "hold": 500, "grid": [[0,0,...],[0,1,1,...],...] }, ...]
}
```

Override URL or output dir with `--base` and `--out`.

## 2. Convert to C

```bash
node convert_to_c.js
```

Reads `tools/claudepix_data/*.json` and emits a single
`firmware/src/splash_animations.h` with:
- `splash_<ident>_frames[N][400]` — per-frame cell codes (0 = empty, 1 = body, 2 = eye)
- `splash_<ident>_holds[N]` — per-frame hold time in ms
- `splash_anims[]` — master table with name, category, frame count, pointers
- `SPLASH_ANIM_COUNT`

The firmware (`splash.cpp`) consumes this header to render and animate.

## Re-running

The scraper is idempotent — re-run any time the source library updates. The
converter overwrites the header. Rebuild firmware after running both.

## Icons (Lucide → LVGL RGB565A8)

```bash
cd tools && npm install     # one-time: installs pngjs
node png_to_lvgl.js icons-src/heart.png icon_heart_data ICON_HEART_W ICON_HEART_H --tint=FFFFFF
```

`png_to_lvgl.js` converts a PNG with alpha to LVGL's planar RGB565A8
format (`w*h` RGB565 pixels + `w*h` alpha bytes). Default `--tint=FFFFFF`
whitens black-on-transparent Lucide icons so they read against the dark
UI background.

Source SVGs/PNGs live under `tools/icons-src/`. Workflow to add one:

1. `curl https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/<name>.svg -o tools/icons-src/<name>.svg`
2. `sed -i 's/currentColor/#000000/g' tools/icons-src/<name>.svg` (so the stroke rasterises)
3. `rsvg-convert -w 24 -h 24 tools/icons-src/<name>.svg -o tools/icons-src/<name>.png`
4. Pipe through `png_to_lvgl.js` (above) and splice into `firmware/src/icons.h`.

## License note

The scraper hits a public site without a stated license. Confirm reuse is
appropriate for your case before redistributing the output.
