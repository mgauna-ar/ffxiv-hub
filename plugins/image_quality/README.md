# 🖼️ Image Quality

Lets you pick the resolution DLSS renders at and keeps it there, makes the camera
jitter match what DLSS is told, so it can smooth edges that are standing still, and keeps
textures as detailed as at the screen's resolution. It changes nothing unless the game's
upscaler is DLSS, and hands every value back to the game when it is switched off.

Part of [FFXIV Hub](../../README.md). Enable or disable it from its page in the desktop
app, or from its card on the dashboard.

![The Image Quality page: DLSS held at Quality, 1440 of 2160 px, with full camera jitter and a −0.58 texture bias](../../docs/images/image-quality-view.png)

## At a glance

- **A DLSS mode that holds.** With DLSS the game picks its render resolution by GPU load,
  anywhere from half the screen to all of it, and moves it 9 pixels every half second
  while the camera moves.
  The plugin holds the mode you choose instead: DLAA, Ultra Quality, Quality, Balanced or
  Performance.
- **Full camera jitter.** DLSS smooths edges by nudging the camera a fraction of a pixel
  every frame, and the game tells DLSS each nudge. It then moves the camera by less than
  it said: the amount shrinks with its 3D resolution slider and with the render height,
  to 0.16 of what it told DLSS at 77% and 4K Quality. DLSS lays every frame's pixels a
  little off, and edges that stay still stay jagged. The plugin moves the camera by what
  DLSS is told.
- **Sharper textures.** The game picks how detailed a texture to draw from the height it
  renders at, so under DLSS textures come out as blurry as at that lower resolution,
  however sharp DLSS makes the edges. The plugin moves the game's texture detail bias by
  how far the render height is below the screen's, as NVIDIA recommends for DLSS, with an
  offset for taste.
- **Visible.** The desktop view shows the upscaler, the height the game renders at within
  its range, the jitter in use, the texture bias, and the game's GPU and CPU times.
- **Reversible.** Switching it off, closing the Hub, or picking FSR puts the game's own
  values back within 50 ms.

---

## How it works

### What the game does with DLSS

With DLSS selected, the game asks DLSS for the range of render resolutions it accepts,
and runs a resolution controller inside it, whatever the *Enable dynamic resolution*
checkbox says. The range depends on DLSS's mode: at 4K, 1080 to 2160 lines in Quality,
and 2138 to 2160 in DLAA, which leaves the controller nowhere to go. The controller:

- It lowers the height when the GPU time per frame passes 90% of the longer of two: the
  CPU time per frame, and the frame time your refresh rate (with vsync) or frame cap
  allows. It raises it below 80%.
- It moves 9 pixels at a time (18 above 1440), once every half second, and only while
  the camera moves. Standing still, the height stays where it is.

So in Quality DLSS renders at anything between Performance and DLAA, and changes with
the scene. Going from the bottom of the range to the top takes about half a minute of
moving around.

The camera jitter has its own catch. Each frame the game picks where in the pixel to
nudge the camera and tells DLSS. It then nudges the camera by less: 0.6 of that, scaled
by the square of its 3D resolution slider times the render height's share of the
range's top. At 77% and 4K the camera moves 0.36 of what DLSS is told at native, 0.16 at
Quality and 0.09 at the bottom of the range, so DLSS lays each frame's pixels slightly
off.

Textures have one more. The GPU picks a texture's detail level (its mip) from how many
pixels the surface covers, and with DLSS that is the render height's pixels, not the
screen's. Nothing in the game makes up for it: its texture bias, the nudge toward a finer
level, comes from a small table per texture filtering level (0 down to -2), whatever the
resolution.

### What gets changed

Three values in the game's own render settings and its texture bias table, from the
payload, at most every 50 ms and only when they differ from what the plugin wants:

| Setting | Game's value | With the plugin |
|---|---|---|
| The resolution controller | On | Off, under DLSS with *Fixed render resolution* |
| The height a frame renders at | Wherever the controller left it | The chosen mode's share of the screen, kept within the game's range |
| The camera jitter | 0.6 of what DLSS is told, scaled down by the slider and the render height | What DLSS is told, under DLSS with *Full camera jitter* |
| The texture bias | The game's per filtering level, the same at any resolution | Lowered by log2 of the held height over the screen's (−0.58 at Quality), plus *Texture sharpness*, while the height is held and *Sharper textures* is on |

The plugin remembers the game's values before its first change and writes them back when
it lets go. The controller then resumes from where the height is.

### What is left alone

- **FSR.** Nothing is changed while the upscaler is FSR, including *TSCMAA + camera
  jitter*, which keeps the game's jitter.
- **The range.** The game's own lowest and highest heights are never crossed: a mode
  outside them is held at the closest end, and the Live tab says so. In DLSS's DLAA mode
  every choice comes out at native.

---

## Desktop view

**Live** shows what the game reports now:

- **Upscaler**: under DLSS, that the camera jitter is on; under FSR, the edge smoothing
  setting.
- **Render height**: what the frame renders at, with the game's range.
- **Render scale**: that height as a share of the screen's.
- **Camera jitter**: how far the camera moves, as a share of what DLSS is told, and what
  the game's own would be.
- **GPU time**: the game's smoothed GPU time per frame, and its CPU time per frame
  without the wait to present. The controller acts on both.

Below them, *What the game is doing* says whether the height is held by the plugin or
moving with GPU load, whose jitter is in use, and how much the plugin adds to the texture
bias.

**Settings** holds:

| Setting | What it does |
|---|---|
| Fixed render resolution | Under DLSS, stop the controller and render at the mode below. |
| Render resolution | DLAA (100%), Ultra Quality (77%), Quality (67%), Balanced (58%) or Performance (50%). |
| Full camera jitter | Under DLSS, move the camera by what DLSS is told instead of the game's smaller amount. |
| Sharper textures | While the render resolution is held, draw textures as detailed as at the screen's resolution. |
| Texture sharpness | Added to that bias, −1 to +1 in steps of 1/8. Lower is sharper and can shimmer; higher is softer and a little cheaper. |
| Reset to defaults | Every switch on, Quality, texture sharpness 0. |

---

## Configuration

Stored in the `image_quality` section of `%APPDATA%/ffxiv-hub/config.json`. Everything
here is editable from the plugin's page in the desktop app; the file is the persistence
format, not the intended interface.

| Key | Type | Default | Description |
|---|---|---|---|
| `plugin_enabled` | bool | `true` | Master switch. Off hands every value back to the game and streams nothing. |
| `fixed_dlss_resolution` | bool | `true` | Under DLSS, stop the resolution controller and render at `dlss_render_scale`. |
| `dlss_render_scale` | float | `0.667` | Render height as a share of the screen's, `0.5`–`1.0`, kept within the game's range. |
| `override_dlss_jitter` | bool | `true` | Under DLSS, move the camera by what DLSS is told instead of the game's smaller amount. |
| `dlss_mip_bias` | bool | `true` | While the DLSS render height is held, add log2 of it over the screen's height to the game's texture bias. |
| `dlss_mip_bias_offset` | float | `0.0` | Mip levels added on top, `-1.0`–`1.0` in steps of `0.125`. Negative is sharper. |

---

## FAQ

<details>
<summary><b>Will my frame rate drop?</b></summary>

It depends on where the game had you. If its controller kept DLSS near the bottom of its
range, a fixed Quality renders more pixels and costs frames; if it kept you near the top,
Quality is cheaper. The Live tab shows the height and the GPU time, so you can compare
with *Fixed render resolution* on and off, and pick Balanced or Performance for more
frames.

</details>

<details>
<summary><b>Why is there no jitter amount to pick?</b></summary>

The game tells DLSS where in the pixel it nudged the camera each frame. Only a camera
moved by exactly that much matches, so a larger or smaller nudge would leave DLSS
misplacing pixels again. The switch either matches it or leaves the game's own amount.

</details>

<details>
<summary><b>Will sharper textures cost frames?</b></summary>

A little GPU time: finer texture levels mean more memory for the GPU to read, most at 4K
and in texture-heavy scenes. The plugin itself does nothing per frame. Compare the Live
tab's GPU time with *Sharper textures* on and off in the same spot, or raise *Texture
sharpness* to trade some detail back.

</details>

<details>
<summary><b>Textures shimmer or look too sharp.</b></summary>

Raise *Texture sharpness* a step or two. The bias NVIDIA recommends is a starting point,
and the game already sharpens textures at its higher filtering levels.

</details>

<details>
<summary><b>Why does Sharper textures need Fixed render resolution?</b></summary>

The right bias depends on the render height. A height that moves with GPU load would
need a new bias at every step, and each one makes the game create texture samplers it
never frees.

</details>

<details>
<summary><b>The Live tab says "Render settings not found".</b></summary>

The payload could not find the game's render settings, usually after a game patch.
`hub_payload.log` names the one it missed. The plugin changes nothing until a release
fixes it.

</details>

<details>
<summary><b>The page shows "Idle".</b></summary>

The upscaler is not DLSS, so nothing is held. Pick NVIDIA DLSS in the game's graphics
settings.

</details>
