# 🖼️ Image Quality

Lets you pick the resolution DLSS renders at and keeps it there, and gives DLSS the camera
jitter it needs to smooth edges that are standing still. It changes nothing unless the
game's upscaler is DLSS, and hands every value back to the game when it is switched off.

Part of [FFXIV Hub](../../README.md). Enable or disable it from its page in the desktop
app, or from its card on the dashboard.

![The Image Quality page: DLSS held at Quality, 1440 of 2160 px, with full camera jitter](../../docs/images/image-quality-view.png)

## At a glance

- **A DLSS mode that holds.** With DLSS the game picks its render resolution by GPU load,
  anywhere from half the screen to all of it, and moves it 9 pixels every half second.
  The plugin holds the mode you choose instead: DLAA, Ultra Quality, Quality, Balanced or
  Performance.
- **Full camera jitter.** DLSS smooths edges by nudging the camera a fraction of a pixel
  every frame. The game's nudge shrinks with its 3D resolution slider and with the render
  height: at 77% and 4K it is 0.36 of a pixel at native, 0.16 at Quality and 0.09 at the
  bottom of DLSS's range. Edges that stay still never get sampled across the whole pixel
  and stay jagged. The plugin uses a full pixel.
- **Visible.** The desktop view shows the upscaler, the height the game renders at within
  its range, the jitter in use, and the game's frame and GPU times.
- **Reversible.** Switching it off, closing the Hub, or picking FSR puts the game's own
  values back within 50 ms.

---

## How it works

### What the game does with DLSS

With DLSS selected, the game asks DLSS for the range of render resolutions it accepts,
and runs a resolution controller inside it, whatever the *Enable dynamic resolution*
checkbox says. The range depends on DLSS's mode: at 4K, 1080 to 2160 lines in Quality,
and 2138 to 2160 in DLAA, which leaves the controller nowhere to go. The controller:

- It lowers the height when the GPU takes more than 90% of the frame, and raises it when
  it takes less than 80%.
- It moves 9 pixels at a time (18 above 1440), once every half second.

So in Quality DLSS renders at anything between Performance and DLAA, and changes with
the scene. Going from the bottom of the range to the top takes about half a minute.

The camera jitter has its own catch. The game starts from 0.6 of a pixel and scales it by
the square of its 3D resolution slider times the render height's share of the range's
top, even under DLSS, where the slider sets nothing else. At 77% and 4K that is 0.36 of
a pixel at native, 0.16 at Quality, and 0.09 when the controller sits at the bottom.

### What gets changed

Three values in the game's own render settings, from the payload, at most every 50 ms
and only when they differ from what the plugin wants:

| Setting | Game's value | With the plugin |
|---|---|---|
| The resolution controller | On | Off, under DLSS with *Fixed render resolution* |
| The height a frame renders at | Wherever the controller left it | The chosen mode's share of the screen, kept within the game's range |
| The camera jitter | 0.6, scaled down by the slider and the render height | The *Jitter amount*, 1.0 by default, under DLSS with *Full camera jitter* |

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
- **Camera jitter**: the amount in use, and what the game's own would be.
- **Frame time**: the game's smoothed frame and GPU times, which its controller acts on.

Below them, *What the game is doing* says whether the height is held by the plugin or
moving with GPU load, and whose jitter is in use.

**Settings** holds:

| Setting | What it does |
|---|---|
| Fixed render resolution | Under DLSS, stop the controller and render at the mode below. |
| Render resolution | DLAA (100%), Ultra Quality (77%), Quality (67%), Balanced (58%) or Performance (50%). |
| Full camera jitter | Under DLSS, jitter by *Jitter amount* instead of the game's amount. |
| Jitter amount | 0.5 to 1.5. 1.0 covers the whole pixel. |
| Reset to defaults | Both switches on, Quality, 1.0. |

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
| `override_dlss_jitter` | bool | `true` | Under DLSS, jitter by `dlss_jitter_amount` instead of the game's amount. |
| `dlss_jitter_amount` | float | `1.0` | Jitter amplitude, `0.5`–`1.5`. |

---

## FAQ

<details>
<summary><b>Will my frame rate drop?</b></summary>

It depends on where the game had you. If its controller kept DLSS near the bottom of its
range, a fixed Quality renders more pixels and costs frames; if it kept you near the top,
Quality is cheaper. The Live tab shows the height and the frame time, so you can compare
with *Fixed render resolution* on and off, and pick Balanced or Performance for more
frames.

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
