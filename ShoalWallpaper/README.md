# Shoal live wallpaper (KDE Plasma 6, Wayland)

The Shoal engine (`../ShoalCore`, shared with `../ShoalLinux`) as a Plasma wallpaper plugin. The fish swim behind
your desktop icons and widgets and steer around them. The engine and its shaders are compiled in unchanged; this folder
adds the Plasma item, the desktop obstacles and the offload thread.

## Install / uninstall

```sh
./install.sh      # builds, installs for this user only, restarts plasmashell
./uninstall.sh    # switches back to the image wallpaper, removes everything, restarts plasmashell
```

Needs `cmake`, `ninja`, Qt 6 (Quick, OpenGL, DBus) and Plasma 6. Root is not needed. What gets installed:

| Path | What |
|------|------|
| `~/.local/lib/qt6/qml/org/shoal/wallpaper/` | The QML/C++ plugin (`ShoalView` item) |
| `~/.local/share/plasma/wallpapers/org.shoal.wallpaper/` | The wallpaper package (`package/`) |
| `~/.config/environment.d/shoal-wallpaper.conf` | Adds the plugin directory to `QML_IMPORT_PATH` (read at login; `install.sh` also sets it for the running session) |

Choose it from the desktop's right-click menu: **Desktop and Wallpaper → Wallpaper type → Shoal**.

## Settings

All settings are on the same page, under the wallpaper type dropdown. They are stored per screen by Plasma
(`~/.config/plasma-org.kde.plasma.desktop-appletsrc`), not in `~/.config/shoal/settings.conf`.
**Copy settings from settings.conf** copies the standalone port's values into the page; click Apply to use them.

- **Fish:** amount, size, size variability with bell curve, tail size and shade, 3D depth and shadows.
- **Species:** which of the 10 fish shapes swim (Classic, Sardine, Goldfish, Koi, Tuna, Trout, Pike, Tetra, Fantail,
  Mackerel; shapes in `../docs/fish_gallery.png`), and the **Species mix**: a random mix, or one species per colour.
- **Swimming:** speed, speed variability, randomness.
- **Colours:** up to 10 fish colours, sort by colour and avoid other colours.
- **Background:** a picture (the default) or a plain water colour. With no picture chosen, it uses Plasma's default wallpaper for your Look-and-Feel theme, the same one the Image wallpaper shows (Breeze Dark: *Next*, dark variant). You can darken the picture. Fish shadows fall on it.
- **Desktop:** which things fish swim around (icons, widgets, application windows, mouse pointer), how far they keep away from them (keep-out margin), and rocks.
- **Performance:** frame rate as a fraction of the refresh rate (1×, ½, ⅓, ¼), which GPU renders the fish (default: the dedicated GPU, here the RTX 2060), whether to pause while a window is maximised or full screen, and a frame-rate counter.

## How it works

- `ShoalView` (`src/shoalitem.cpp`) is a `QQuickFramebufferObject`. Its renderer runs the engine (`src/shoaloffload.cpp`
  feeds it one frame, see `ShoalRunFrame`) with
  OpenGL 4.3 in plasmashell's own scene-graph context, in device pixels, so speeds and sizes match the standalone port.
  Each screen gets its own wallpaper and its own shoal.
- **Water area:** the desktop containment's `availableScreenRect` (the screen minus panels) is the simulation's single
  "monitor", so fish turn at the panel's edge instead of swimming under it.
- **Obstacles:** every 0.4 s the item searches its window's item tree for desktop icons (`FolderItemDelegate`,
  icon and label area) and widgets (`AppletContainer`). It sends them to the shaders as 4-point rocks. They are added
  to the rock buffer after the real rocks, and are not drawn. Application windows come from the task manager model in
  `main.qml` (the same one used for pausing). It covers windows that aren't minimised and are on this screen, virtual
  desktop and activity. The list updates within about 0.1 s of a window moving. A window larger than 85% of the
  screen is ignored, and the fish swim under it. Before that they go through a 6 px grid: gaps narrower
  than about two fish lengths (between obstacles, or between an obstacle and the edge of the water) are filled in
  with a morphological closing, the keep-out margin is added, and the grid is cut back into rectangles. On your
  desktop, 21 icons and a widget become 8 blocks. Icon and widget blocks are hard walls. Window blocks and the mouse pointer (a small hexagon, tracked through an
  event filter on the desktop window) are soft: they are sent with a negative radius, `rock.comp` never pushes fish out
  of them, and fish only steer around them. A fish that ends up inside a soft obstacle (a window was moved over it, or
  the pointer landed on it) turns towards the nearest edge, weighted by its direction of travel, and speeds up to 2.5×
  until it is out. Fish deep inside a hard block (more than one fish length, e.g. spawned before the first scan) do
  the same instead of being pushed. Nothing is ever respawned. The shared shaders behave as before for the
  standalone port, which never sends negative radii.
- **GPU:** plasmashell (and its scene graph) runs on the GPU that drives the desktop, here the Vega iGPU. The
  simulation and drawing (`../ShoalCore/shoal_engine.cpp`, no Qt GL wrappers; GL calls go through glvnd's libOpenGL to whichever
  vendor's context is current) run either right there, or, when the **GPU** setting names another GPU, in a worker
  thread with its own surfaceless EGL context on that GPU (`ShoalOffload`). Each frame the other GPU copies its
  framebuffer into system memory with DMA, the worker copies it (about 1 ms at 1080p) into a persistently mapped
  pixel-unpack buffer of the desktop GPU, and the desktop GPU uploads that into the wallpaper's texture. The
  scene-graph thread never waits for the other GPU, at the cost of one frame of latency. A zero-copy hand-over isn't
  possible: the NVIDIA GL driver accepts a dma-buf from another device but draws into a private copy of it. If the
  chosen GPU can't be used, the item logs why and renders on the desktop's GPU. The journal says which GPU it uses
  (`Shoal: rendering on ...`). An empty setting (the default) means the first GPU that isn't the firmware's primary
  display GPU (`boot_vga`). Keeping the dedicated GPU busy also keeps it powered up (about 10 W idle-ish, P8).
- **Picture mode:** a QML `Image` sits under the item, and the item clears to transparent. Shadows then add 80% black
  coverage, using a constant blend alpha, instead of multiplying an opaque background.
- **Frame pacing:** after each presented frame the item asks for the next one (1×) or waits for the n-th refresh
  (½ to ¼).
- **Pausing:** `main.qml` watches the task manager model for this screen, virtual desktop and activity. A window that
  is maximised, full screen or covers at least 95% of the screen sets `running: false`, which stops frame requests
  entirely. When the screen is off or locked, the compositor stops frame callbacks, which also stops it.

## Cost (Ryzen laptop, Vega 10, 1080p @ 144 Hz, 124 fish)

| Frame rate | Fish fps | Desktop redraws/s | plasmashell CPU |
|------------|----------|-------------------|-----------------|
| Refresh rate (1×) | 144 | 144 | ~41% |
| ½ | 72 | ~144 | ~41% |
| ¼ | 36 | ~85 | ~27% |
| Paused / image wallpaper | 0 | 0 | ~3% |
| 1×, fish on the RTX 2060 | 144 | 144 | ~50% |

In a plain Qt window at 144 fps, rendering on the RTX 2060 costs about 36% CPU against 15% on the Vega, almost all
of it the frame copy (DMA wait, memcpy, upload).

Most of this cost is plasmashell redrawing its whole desktop window, not the simulation. In a plain Qt window the
same item uses about 13%. Plasma also redraws the desktop window one extra time after every wallpaper change. A
plain QML rectangle animated at 36 Hz gives about 79 redraws per second, so ½ saves nothing on a 144 Hz screen, and
⅓ or ¼ are the useful power-saving settings.

## Debugging

Set `SHOAL_DEBUG=1` in plasmashell's environment and restart it. It then logs the fish frame rate once a second and
each settings change to the journal. `journalctl --user -f | grep Shoal` always shows the fish count, water area,
detected icons and widgets, and pause/resume.

```sh
systemctl --user set-environment SHOAL_DEBUG=1 && systemctl --user restart plasma-plasmashell.service
```
