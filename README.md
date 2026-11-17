# Shoal

Shoal draws a shoal of fish-like boids that flock, avoid walls and rocks, and swim as bending fish outlines in 10 species. Flocking and rendering run on the GPU through compute shaders. It began as a Windows screensaver and now runs on a shared engine (`ShoalCore`) as:
- a Windows screensaver and live wallpaper (`ShoalWindows`)
- a Linux full-screen app (`ShoalLinux`)
- a KDE Plasma live wallpaper (`ShoalWallpaper`)
- an Android TV screensaver (`ShoalAndroid`)

The sections from **Layout** onward describe the original Windows screensaver (`ShoalNative`: a single Win32/Direct3D 11 C++ file, which needs a Direct3D 11 feature level 11_0 GPU). Its settings, design notes and test results still apply to every port. Each port's own `README.md` covers how it differs.

## Ports and experiments in this folder

| Folder | What it is |
|--------|------------|
| `ShoalNative/`, `build/`, `tools/`, `docs/` | The original Windows screensaver (Win32 + Direct3D 11), unchanged, kept for reference. `ShoalWindows/` replaces it. |
| `ShoalWindows/` | **The Windows screensaver on the shared engine** (OpenGL 4.3 through WGL), cross-compiled from Linux. It keeps the original settings, registry values, presets and dialog, and adds species and per-monitor V-Sync. It replaces `ShoalNative` as the installed `Shoal.scr`. See its `README.md`. |
| `ShoalCore/` | **The shared engine and shaders** used by the Linux, wallpaper, Windows and (as a Java port) Android builds: world setup, GLSL 4.30 compute simulation, drawing, background picture. See its `README.md`. |
| `ShoalLinux/` | The main Linux build: SDL2 windows around `ShoalCore`, full-screen on every display, options as in the table below, plus a background colour or picture, a GPU choice and a Qt settings dialog (`./shoal --settings`). Not ported: presets, Control Panel preview, desktop capture, debug logging. See its `README.md`. |
| `ShoalWallpaper/` | The 2D Linux port as a KDE Plasma 6 live wallpaper (works on Wayland). It runs the `ShoalCore` engine inside plasmashell, and fish avoid desktop icons, widgets and the mouse pointer. It pauses while a window is maximised or full screen. Settings are in Plasma's Desktop and Wallpaper dialog. Install with `ShoalWallpaper/install.sh`. See its `README.md`. |
| `ShoalAndroid/` | The 2D version as an Android TV screensaver (DreamService) and launcher app for Android 7+ devices with OpenGL ES 3.1, validated on the HY320 Mini projector (Mali-G31, GLES 3.1 compute) and the TCL G10 4K TV (PowerVR BXE-4-32, Android 12). It uses the `ShoalCore` shaders, converted to GLSL ES at build time, and adds a frame-rate setting (V-Sync, half, double or triple the panel refresh, or unlocked). Build and install with `ShoalAndroid/build.sh install`, or `ANDROID_SERIAL=<device-ip>:5555 ./build.sh install` for a device on network adb. See its `README.md` for the projector's lamp-off quirk, the TCL `APP_AUTO_START` grant, the frame-pacing measurements, and why the TCL cannot draw at its panel's 4K. |
| `ShoalLinux3D/` | Experimental 3D version of the Linux port: spheroid fish in a perspective box whose depth equals its width, depth fog, ray-cast room, real occlusion. Shares `~/.config/shoal/settings.conf` and the options of `ShoalLinux`, plus `--camera` and `--depthscale`. See its `README.md`. |

Linux builds: `cd ShoalLinux && make && ./shoal` (needs SDL2, SDL2_image, OpenGL 4.3, python3), or `ShoalLinux3D` (`./shoal3d`, separate code, needs GLEW and `xxd`).

## Layout

| Path | Purpose |
|------|---------|
| [ShoalNative/Main.cpp](ShoalNative/Main.cpp) | The entire application: settings, simulation, HLSL shaders (embedded strings compiled at runtime), rendering, settings dialog and debug logging |
| [ShoalNative/build.bat](ShoalNative/build.bat) | Release build script (VS2017 x64, `/O2`) |
| [ShoalNative/Shoal.rc](ShoalNative/Shoal.rc) | Resources: the display name "Shoal" (string 1, read by Screen Saver Settings) and version info |
| `build/ShoalNative.exe` | Build output, used for testing |
| `build/Shoal.scr` | The same binary with the screensaver extension, for installing |
| `build/logs/` | CSV logs written by `/debuglog` runs |
| [tools/analyze_boid_log.py](tools/analyze_boid_log.py) | Analyzes debug logs and reports wall and rock stalls |
| [tools/analyze_grouping.py](tools/analyze_grouping.py) | Measures how strongly boids group, for example across randomness levels |
| [tools/analyze_frames.py](tools/analyze_frames.py) | Frame-timing report: FPS, interval percentiles, hitches, simulation and Present cost |
| [tools/analyze_size.py](tools/analyze_size.py) | Size check: body overlap, wall clipping, fish noses inside rocks, speed and stalls |
| [tools/analyze_walls.py](tools/analyze_walls.py) | Wall-distance profile: finds lanes where fish pile up along screen edges |
| [docs/](docs/) | Design notes, investigation reports and [test results](docs/test-results.md) |

## Building

You need Visual Studio 2017 with the C++ x64 tools installed at the default Community path.

```bat
ShoalNative\build.bat
```

For a debug build, run `vcvarsall.bat x64` and then compile in the same `cmd` session:

```bat
cl /EHsc /MDd /DUNICODE /D_UNICODE /D_DEBUG /Od /Zi /W4 Main.cpp ...
```

Use `/MDd` with `_DEBUG`. Otherwise, the link fails on `_CrtDbgReport`.

### Installing

Copy `build\Shoal.scr` to `C:\Windows\System32` from an elevated prompt. It is a 64-bit binary, so it belongs in System32, not SysWOW64. "Shoal" then appears in the Screen Saver Settings drop-down.

```bat
copy /y build\Shoal.scr C:\Windows\System32\
```

Notes:
- Re-copy after every build.
- The Control Panel passes `/p <hwnd>` (with a space) for the preview. Shoal accepts both this and the `/p:<hwnd>` form.
- `/debuglog` writes `logs\` next to the executable, which System32 does not allow, so run debug logs from `build\`.
- `Start-Process`/ShellExecute on a `.scr` uses the `scrfile` open verb (`"%1" /S`), which drops any extra arguments. Launch `ShoalNative.exe` instead, or start the `.scr` with `CreateProcess`.

The shaders are compiled at runtime. A shader error therefore does not cause a build failure. Instead, Shoal shows the "D3D11 GPU Not Found" error at startup, and a `/debuglog` file ends with `# error D3D11 GPU Not Found`. After changing HLSL, launch it once to confirm the shaders still compile.

## Running

| Command line | Mode |
|--------------|------|
| *(none)* | Full-screen screensaver on all monitors |
| `/c`, `/c <hwnd>` or `/c:<hwnd>` | Settings dialog. The Control Panel's Settings... button passes `/c:<hwnd>` |
| `/p <hwnd>` or `/p:<hwnd>` | Preview inside the Windows screensaver control panel. The preview simulates an area 3× the size of the small monitor image and draws it scaled down, so fish are big enough to see. The fish count follows Fish Amount for that area. Rocks and the FPS counter are never shown in this small preview. The dialog's **Preview** button runs the normal full-screen mode with your settings |
| `/randomness:N` | Overrides the saved Randomness value (0–100) for this run only. Useful with `/debuglog` |
| `/speed:N` | Overrides the saved Speed value (1–1000) for this run only |
| `/rocks:N` | Overrides RockCount (0–10) for this run only |
| `/boids:N` | Absolute fish count (2–10000) for this run only. Bypasses FishCoverage |
| `/coverage:N` | Overrides FishCoverage (0–100) for this run only |
| `/speedvar:N` | Overrides SpeedVariability (0–10) for this run only |
| `/tail:N` | Overrides TailPercent (10–100) for this run only |
| `/shade:N` | Overrides the tail shade (−100 to 100; negative = darker toward the tail) for this run only |
| `/coloursort:0` or `/coloursort:1` | Overrides ColourSort for this run only |
| `/colouravoid:0` or `/colouravoid:1` | Overrides ColourAvoid for this run only |
| `/size:P` | Overrides the fish size as a percentage of screen height (1–10, decimals allowed, e.g. `/size:1.9`) for this run only |
| `/sizevar:N` | Overrides SizeVariation (100–300) for this run only |
| `/sizebell:0` or `/sizebell:1` | Overrides SizeBell for this run only |
| `/depth:0` or `/depth:1` | Overrides DepthEffect for this run only |
| `/desktop:0` or `/desktop:1` | Overrides DesktopBackground for this run only |
| `/randompreset:0` or `/randompreset:1` | Overrides RandomPreset for this run only (other overrides still apply on top of the picked preset) |
| `/restrict:0\|1` | Overrides RestrictToMonitor for this run only |
| `/showfps:0\|1` | Overrides ShowFps for this run only |
| `/vsync:0\|1` | Overrides VSync for this run only |
| `/fpsmode:N` | Overrides FrameRateMode (0–5) for this run only |
| `/framerate:N` | Fixed frame-rate cap (1–100000 fps) for every monitor, for this run only. Turns off vsync and ignores FrameRateMode. Intended for tests |
| `/noboidlog` | With `/debuglog`, records only frame timing. Boid logging reads back the GPU buffer and writes rows on the frame thread, which skews timing: at 10000 fish it costs about 25 ms per sample (180 instead of 377 fps uncapped). Use it for all performance runs |
| `/debuglog[:seconds]` | Full-screen run that logs boid positions and speeds to `logs\` next to the executable. With `:seconds`, it exits automatically; without it, logging continues until input |

The mouse cursor is hidden while the full-screen screensaver runs (not in the Control Panel preview). Any mouse or keyboard input exits the screensaver after a one-second grace period. Do not touch input during a `/debuglog` run.

## Settings

Settings are stored as DWORDs under `HKCU\Software\Shoal`.

**Presets:** the dropdown at the top of the settings dialog saves every setting under a name. Type a name and click the save icon to add it, or to replace an existing preset after confirming. Pick a preset from the list to load it into the dialog; it takes effect when you click OK. The delete icon removes the selected preset after confirming. Each preset is stored as a subkey `HKCU\Software\Shoal\Presets\<name>` with the same values as below. Names can't contain `\`.

**Random preset:** with **Use a random saved preset each time the screensaver starts** ticked, each run (full screen and Control Panel preview) loads a random saved preset instead of the main settings. With two or more presets it never repeats the one used last time, which is remembered in `LastPreset` (REG_SZ). The checkbox is greyed out when there are no presets. `RandomPreset` itself is only stored in the main settings, not in presets. Debug logs show the picked preset as `# preset <name>`.

| Value | Range | Meaning |
|-------|-------|---------|
| `Speed` | 1–1000 | Swim speed in pixels per second. With speed variability, this is the speed most fish cruise at |
| `SpeedVariability` | 0–10 | Default 5. 0 = every fish always swims at exactly `Speed`. Higher values let fish speed up and slow down at will, up to the full 1–1000 px/s range at 10. See [docs/speed-variability.md](docs/speed-variability.md) |
| `FishCoverage` | 0–100 | Default 50. Fish Amount slider position. The count is relative: the slider sets the share of free screen area covered by fish. Free area is all monitors minus rocks. The share runs from 0.14% at 0 to 7.29% at 100 on a log scale. Each fish counts as `length × length/4`, so count = `coverage × free area / footprint`, clamped to 2–10000. On this 2×1080p desktop, the slider gives exactly 2 fish at maximum size and 0, and 10000 at minimum size and 100. The dialog shows the resulting count live. Replaces the old absolute `BoidCount`, which is converted to the matching coverage on load and deleted on the next save |
| `Randomness` | 0–100 | How often fish break away from the shoal. 0 = pure flocking, 100 = almost no groups. See [docs/randomness.md](docs/randomness.md) |
| `SizePercentTenths` | 10–100 | Fish length as tenths of a percent of the primary screen height (10 = 1%, 100 = 10%; 1.9% = 21 px on 1080p). Fish are drawn `length × length/4`. The settings dialog shows an actual-size preview. Replaces the old pixel-based `Size` value, which is converted on load and deleted on the next save |
| `SizeVariation` | 100–300 | Default 100 (all fish the set size). Size Variability: each fish's length is the set length × `V^u` with `V = value / 100` and `u` in −1…1, so 300 gives fish from 3× smaller to 3× larger (at least 4 px). Fish Amount accounts for the spread: the count is divided by the mean of `(length / set length)²` (2.02 for an even spread at 300, 1.31 for the bell curve), so the covered area stays the same |
| `SizeBell` | 0/1 | Default 1. 1 = bell curve (`u` is the mean of three uniform values, so most fish are near the set size); 0 = even spread (`u` uniform, log-even between smallest and largest). The checkbox is greyed out when SizeVariation is 100 |
| `TailPercent` | 10–100 | Default 33. Tail size as a percentage of the body (both length and width). The body shrinks as the tail grows, so body plus tail always span the fish length |
| `ColourCount`, `Colour0`–`Colour9` | 1–10, `0x00BBGGRR` | Fish palette (head colours). Each fish is randomly assigned one for life. Default: an ocean palette of silver, yellow, orange, blue and teal. Unused `ColourN` values are deleted on save |
| `DepthEffect` | 0/1 | Default 1. 3D depth: larger fish always swim above smaller ones (equal sizes in random order), deeper fish are drawn smaller and darker, and higher fish cast shadows on them |
| `DesktopBackground` | 0/1 | Default 0. 1 = capture each monitor's screen image when the screensaver starts and draw it behind the fish instead of black; with 3D depth on, fish shadows fall on it. Not used in the Control Panel preview |
| `TailShade` | 0–200 | Head-to-tail lightness change, stored +100 (100 = none). Below 100 darkens toward the tail (0 = black tip), above 100 lightens toward white. Default 60 (40% darker) |
| `ColourSort` | 0/1 | Sort fish by colour: fish ignore other colours when schooling (they still keep their spacing). Disabled with one colour. Replaces the older 0–10 `ColourBias` (any value above 0 is read as on; deleted on save). Default 0 |
| `ColourAvoid` | 0/1 | With ColourSort on, differently coloured fish also actively steer away from each other. Ignored when ColourSort is off. Default 0 |
| `ShowFps` | 0/1 | Draw the FPS counter |
| `RandomPreset` | 0/1 | Default 0. 1 = each run uses a random saved preset (not the one used last time when there are two or more) |
| `VSync` | 0/1 | Default 1. Present each monitor at its own vblank, so there is no tearing and a 60 Hz and a 75 Hz monitor each run at their native rate |
| `FrameRateMode` | 0–5 | Used only when VSync is off. Frame rate as a multiple of each monitor's refresh rate: 0 = ½×, 1 = 1×, 2 = 2×, 3 = 3×, 4 = 4×, 5 = unlimited. Tearing is possible |
| `RockCount` | 0–10 | Obstacles the fish avoid |
| `RestrictToMonitor` | 0/1 | Keep fish on their original monitor instead of letting them cross between adjacent monitors |

The obsolete `BoidCount`, `Size`, `FrameRate` and `Shape` values are deleted the next time settings are saved.

## How it works

- **Frame loop:** `RunFrameLoop` is a `PeekMessage` loop that paces each monitor window independently.
  - **VSync on:** Each swap chain is created with a frame-latency waitable object (maximum latency 1), which signals at that monitor's vblank. The loop waits on all of them and draws whichever monitor is due, using `Present(1, 0)`.
  - **VSync off:** Each window has its own deadline at `refresh × multiplier`, using the exact rational refresh rate from `QueryDisplayConfig` (for example 71.925 Hz). Deadlines are met with a high-resolution waitable timer plus a short spin, then `Present(0, ALLOW_TEARING)`.
  - Whenever any windows are due, the loop runs one simulation step using the real elapsed time, then draws those windows.
  - It replaced a `WM_TIMER` loop that was stuck at about 60 fps and caused stutter. See [docs/frame-timing.md](docs/frame-timing.md).
  - Flocking strengths were tuned at 60 steps per second and are scaled by `delta * 60`, so behavior does not change with frame rate.
- **Simulation:**
  - The first compute shader handles flocking: cohesion, alignment and separation.
  - It also adds a gentle random turning drift and wall avoidance.
  - **Seeking:** a fish with fewer than 4 neighbors steers gently toward the nearest fish outside its group, so small shoals still find each other. The pull fades as the group grows (none at 4+ neighbors), is off while a fish is broken away, scales with `1 − randomness/100`, and with `RestrictToMonitor` only targets fish on the same monitor.
  - **Speed variability:** every 1.5–5 s, each fish picks a speed "mood". Most moods are a small jitter (up to ±12% at 10) around `Speed`. Some are excursions toward 1 or 1000 px/s, usually modest and occasionally extreme. Lonely (seeking) fish have more excursions, lean toward fast ones, and keep them moderate so they don't overshoot. Schools blend 75% toward their neighbors' cruise speed, so they speed up and slow down together. Fish slow down near walls and rocks. At 0, every fish swims at exactly `Speed`. See [docs/speed-variability.md](docs/speed-variability.md).
  - **Sort by colour (`ColourSort`):** different-coloured neighbors are left out of cohesion, alignment, neighbor speed sharing and seeking. Separation always applies to every fish. The fish's colour is the same index hash as the vertex shader. With one colour, sorting is off. Internally the shader takes a 0–1 `colourBias` weight (other colours count `1 − colourBias`); the setting drives it to 0 or 1, because values in between made little visible difference.
  - **Colour avoidance (`ColourAvoid`, needs `ColourSort`):** each different-coloured fish within the rock look-ahead is treated as an obstacle: `wallTerm` adds an outward normal from its body (distance minus half a length), using the same approach-dependent reach. The sum is capped at 2 and fed into the wall/rock steering (tangent-plus-outward turn). It does not trigger the wall rebound or wall braking, so fish keep schooling while they steer around other colours.
  - Rocks use the same obstacle steering as walls: the nearest point on each rock's outline adds a weighted normal within a look-ahead of `max(Speed, current speed) * 0.3` px (40–120).
  - A second compute shader is only a safety net: it pushes out any fish that ends up inside a rock and slides it along the edge.
  - Edges shared with an adjacent monitor are open unless `RestrictToMonitor` is set.
- **Size scaling:** Spacing and avoidance were tuned for 20 px fish and scale with `k = length / 20`, using each fish's own length (Size Variability gives every fish its own fixed length). See [docs/size-scaling.md](docs/size-scaling.md).
- **Size and speed:** with Size Variability, a fish's size ratio `r = length / set length` changes how it swims. Its cruise speed is multiplied by `r^-0.2` (3× larger is 0.8× the speed, 3× smaller is 1.25×). Its speed-up and slow-down rates are multiplied by `r^-0.5` (3× larger changes speed at 0.58× the rate, 3× smaller at 1.73×). So big fish are a little slower and steadier, and small fish are quick and nimble. When matching speed with neighbours, each neighbour's size factor is taken out first, so a big fish doesn't just copy small fish. Emergency wall braking isn't scaled. With Size Variability at 100% every fish is the set size, so nothing changes.
  - Neighbor radius `90k`, separation radius `46k`, with `k` from the average length of the two fish so mixed-size fish school and space symmetrically. Cohesion is divided by `k` and separation (for `k > 1`) multiplied by `k`, so large fish keep their spacing instead of piling up.
  - Wall and rock distances are measured from the body, not the center: they are reduced by `(length − 20) / 2`. The look-ahead minimums grow by `√k` for large fish.
  - The hard wall clamp and the rock safety net keep the whole body (its extent along the wall normal) out of the wall or rock.
- **Wall avoidance:**
  - Each blocking wall within the look-ahead distance (`max(Speed, current speed) * 0.3` px, at least 48 and at most 20% of the monitor's shorter side) adds a weighted outward normal. Corners combine both normals.
  - Each fish's look-ahead is scaled by a personal comfort factor (0.55–1.15) that drifts smoothly every 3–7 s. A wall's reach also shrinks to half for a fish swimming parallel to it and stays full for a fish heading straight at it. Together these remove the single shared "invisible wall" line that groups used to run along.
  - Fish steer toward the wall tangent, with an outward bias. They turn harder as they get closer.
  - **Wall rebound:** a fish that meets a wall or rock while heading into it, or that has been swimming along one (about 1.2 chance per second), turns back toward open water at a random 20–60° angle for 0.6–1.4 s. This uses the same breakaway state as Randomness, so cohesion is paused for that moment. See [docs/wall-lane.md](docs/wall-lane.md).
  - Fish brake only while heading toward the wall, and never below 30% of their cruise speed.
  - A hard clamp is a final safety net: it keeps fish on screen and slides them along the wall.
  - See [docs/wall-stall-investigation.md](docs/wall-stall-investigation.md).
- **Rendering:** Fish are drawn procedurally in a vertex shader from the boid buffer.
  - Each fish is an oval body plus an oval tail. The tail is the body shape scaled by `TailPercent` (10–100%) and overlaps it by 20% of its own length. Together they span exactly `length`; the body is `length / (1 + 0.8 × tail ratio)` long and 0.3125 × that wide (`length × length/4` at the default 33%).
  - The simulation keeps a per-fish `swimPhase` and a smoothed turn rate `yawRate`. The beat frequency is `1.5 × speed / length` Hz (0.8–5 Hz, ±10% per fish), so fast or small fish beat faster.
  - The body sways side to side and the tail sweeps the other way. The tail's sweep grows with speed (about 14–34°), and it also swings opposite to the body's turn, like a real fish pushing through the water.
  - **Thrust:** each fish keeps a smoothed `thrust` value from its acceleration: about 1 while cruising, up to 1.6 while speeding up, and close to 0 while slowing down. Beat rate (25–145%), tail sweep and body sway all scale with it, so an accelerating fish beats harder and faster and a slowing fish coasts with a still tail. The turn term is separate, so a coasting fish still steers with its tail like a rudder.
  - The settings dialog's actual-size preview is animated with the same tail motion at the current Speed.
  - **Colour:** each fish gets one palette colour (a hash of its index) as its head colour. The pixel shader shades it along the body by `t × shade`, where `t` is 0 at the head tip and 1 at the tail tip: a negative shade scales toward black and a positive one blends toward white.
  - The settings dialog's **Sort fish by colour** checkbox is greyed out when only one colour is set. **Different colours actively avoid each other** is also greyed out unless sorting is ticked.
  - The settings dialog shows the palette as swatches (click to change with the Windows colour picker, right-click to remove, **Add...** for up to 10) and a **Tail Shade** slider. The preview draws one fish per colour with the same gradient, cycling through the palette when they don't all fit.
  - The sway and tail are visual only, so they cannot drift the simulation.
  - **3D depth** (`DepthEffect`): each fish has a fixed `depth` from 0 (deepest) to 1 (highest), set at start-up from its rank by length: a smaller fish is always below a larger one, and fish of equal length (for example at Size Variability 100%) are ordered randomly.
    - Deeper fish are drawn up to 20% smaller and 40% darker, and are hidden by higher fish through a depth buffer.
    - After all fish are drawn, a second pass draws each fish's shadow, offset down and right by `length × (0.04 + 0.08 × depth)`. The shadow multiplies the pixels below it by 0.2, only where a lower fish is (the depth test), and only once per pixel (the stencil).
    - Depth is visual only, so fish still school in 2D. The cost at 10,000 fish was within noise (325 vs 327 fps).

## Debug logging

```powershell
build\ShoalNative.exe /debuglog:60
python tools\analyze_boid_log.py                     # newest log in build\logs
python tools\analyze_boid_log.py <log> --threshold 0.2 --min-duration 0.5 --trace 3
python tools\analyze_grouping.py <log> [<log> ...]       # grouping metrics, one line per log
python tools\analyze_size.py <log> [<log> ...]           # overlap, wall clip, rock nose per log
python tools\analyze_walls.py [log]                       # distance-to-wall profile (lanes along edges)
```

Logs sample every 0.05 s. Boids 0–7 are always logged. Other boids are logged when they are within 80 px of a wall or rock, or below 25% speed. Every 0.5 s, a full snapshot of all boids is written (`snapshot=1`).

The header records the settings, the monitor layout and each rock's outline (`# rock <i> x,y x,y ...` in absolute desktop pixels). Columns are:

```
time_s, boid, x, y, vx, vy, heading_deg, current_speed, speed_fraction, stopped,
monitor, wall_side, wall_distance, facing_wall, rock_distance, independent, snapshot,
cruise_fraction
```

`speed_fraction` and `cruise_fraction` are the fish's actual and intended speed divided by `Speed`. With speed variability, a fish may choose to swim slowly, so the analyzers count stalls and slow rows relative to `cruise_fraction`.

Each window writes a `# window` header line with its bounds, refresh rate, vsync and target fps. At exit, frame timing is appended as `#F,window,end_ms,interval_ms,sim_delta_ms,sim_cpu_ms,render_cpu_ms,present_ms,present_count,present_refresh,sync_refresh,sync_qpc_ms` lines. The last four columns are DXGI frame statistics, which the analyzer uses to measure each monitor's vblank rate and repeated frames. Analyze them with:

```powershell
build\ShoalNative.exe /debuglog:15 /noboidlog /vsync:1 /showfps:1
python tools\analyze_frames.py <log>
```

`independent` is 1 while a boid is broken away from the shoal. `facing_wall` is +1 when the fish heads straight into its nearest wall and −1 when it heads straight away.

## Licence

Shoal is free software under the GNU General Public License v3.0 (see `LICENSE`).
`ShoalCore/include/GL/glcorearb.h` is the Khronos OpenGL header, under its own MIT-style licence.
