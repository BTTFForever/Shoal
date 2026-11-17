# Shoal for Android TV

The 2D Shoal boids screensaver as an Android TV **screensaver (DreamService)** plus a launcher app. It starts
after the idle timeout, and any remote key ends it and returns to whatever was on screen before.

Validated on two quite different devices, an HY320 Mini projector (Mali-G31, Android 11) and a TCL G10 4K TV
(PowerVR, Android 12), so it should run on other Android TV boxes, TVs and projectors too.

## Requirements

- Android 7.0 (API 24) or newer, with **OpenGL ES 3.1** (compute shaders). Most TV devices from the last several years qualify.
- To install from source: the build toolchain below, and `adb` access to the device (USB debugging or network debugging).
- Device quirks found on the two test devices are handled by the installer and are harmless elsewhere: the projector's
  lamp-off property (needs root, otherwise a warning) and TCL's `APP_AUTO_START` permission. Other brands may have
  similar "auto start" or battery restrictions on third-party screensavers. See the TCL section for the pattern.

## Target device 1: HY320 Mini projector (measured 2026-11-08, `adb connect <projector-ip>:5555`)

| | |
|---|---|
| SoC | Allwinner H713 (`sun50iw12p1`), 4x Cortex-A53, **32-bit userland only** (`armeabi-v7a`) |
| GPU | Mali-G31, OpenGL ES 3.2 (compute shaders OK) but **0 vertex-shader SSBOs**, no implicit-conversion extension |
| RAM | 1 GB (`low_ram`), ~390 MB available |
| Display | 1024x600 @ 60 Hz, 240 dpi |
| OS | Android 11 TV (leanback), API 30, rooted (Magisk), stock screensaver `com.htc.screensaver` |

Result: steady **60 fps** (V-Sync) at any allowed fish count, 100 to 1,000. At the 1,000 maximum, the unlocked
frame rate is 95 fps (see Benchmark).

Rocks are not built. The desktop rule needs a screen at least 900 px tall, so they would never appear on this
panel. `rock.comp` is skipped and the rock code is gone. The shared `sim.comp` still declares the rock buffer,
so a tiny unused buffer is bound with `rockCount = 0`.

## Target device 2: TCL G10 4K TV (measured 2026-11-15, `adb connect <tv-ip>:5555`)

| | |
|---|---|
| SoC | Realtek RTD6748 (`rtd6748`), **32-bit userland** (`armeabi-v7a`), build `G10-user` |
| GPU | PowerVR B-Series BXE-4-32, OpenGL ES 3.2, 35 vertex-shader SSBOs |
| RAM | 1.8 GB, ~630 MB available |
| Display | 3840x2160 panel, but the system runs at an **1920x1080** override, 320 dpi, 60 Hz |
| OS | Android 12 TV, API 31, not rooted (no `su`), stock screensaver `com.google.android.apps.tv.dreamx/.service.Backdrop` |

Result: **60 fps** (V-Sync) at the default 100 fish, 678 fps unlocked, so there is plenty of headroom. The GPU
computes more slowly per fish than the Mali (one 700-fish shoal would run at about 59 fps unlocked instead of 95),
which is why the default count is lower here. No root is needed, unlike the projector.

### TCL blocks third-party services: `APP_AUTO_START`

The TV is fine, but its own `TclAppBoot` layer refuses to start the dream when it comes from the idle
timeout:

```
TclAppBoot: forbid bind service ComponentInfo{com.shoal.screensaver/com.shoal.screensaver.ShoalDream}
  reason = 'callee_does't_have_OP_AUTO_START_permission.'
DreamController: Unable to bind dream service
```

Starting it by hand (`Somnambulator`) works, so the app itself is fine. Grant the app-op once, and it
persists across reboots:

```sh
adb shell appops set com.shoal.screensaver APP_AUTO_START allow
```

`./build.sh install` now does this as well. Everything else is the same as on the projector, and the lamp-off
`persist.sys.screenSaver` step is skipped when there is no root.

### TCL benchmark (2026-11-15, current settings: 1920x1080, fish length 49 px, size variation 150, depth off)

| Fish | Unlocked fps | Simulation ms | Draw ms |
|---:|---:|---:|---:|
| 250 | 248 | 2.8 | 3.8 |
| 500 | 101 | 7.5 | 5.0 |
| 700 | 59 | 14.0 | 6.2 |
| 1000 | 34 | 24.5 | 7.2 |
| 1500 | 16.8 | 51.7 | 9.9 |
| 2000 | 9.9 | 90.2 | 12.5 |
| 2500 | 6.5 | 143 | 15.3 |
| 3000 | 4.7 | 200 | 17.8 |

- At the default 100 fish: **678 fps** unlocked, so 60 fps with a lot of headroom.
- 60 fps holds up to about 800 fish. The simulation is O(n²) and dominates above that; the draw is cheap.
- The settings screen, the palette picker and "Start after" all work with the TCL remote.

### Frame rate

Both panels run at 60 Hz. **Frame rate** in the "Other" section picks how hard the app drives it:

| Setting | Requested | TCL, 100 fish | Projector, 100 fish | Projector, 700 fish |
|---|---|---|---|---|
| V-Sync (panel rate) | one frame per vblank | **60.2 fps** | **60.1 fps** | **60.5 fps** |
| Half rate | 30 fps | **30.1 fps** | **30.1 fps** | **30.1 fps** |
| Double rate | 120 fps | **119.6-120.0 fps** | **120.1 fps** | 86.2 fps |
| Triple rate | 180 fps | **178.6 fps** | **177.5 fps** | 87.8 fps |
| Unlocked | as fast as possible | 678 fps | 287.6 fps | 88.9 fps |

Only 60 Hz is actually shown, so the rates above the panel refresh buy nothing visible: they make the motion
simulation run with more, smaller steps and cost more power. Half rate halves the GPU work, but the pacer still
keeps one core busy waiting, so the saving is modest; unlocked is mainly for benchmarking. The default is V-Sync.

The pacer never makes things worse when the target is out of reach: the projector's 700-fish shoal can only do
about 89 fps, so double and triple settle there instead of 120/180 (a missed deadline just restarts the schedule
each frame), and only reachable rates are actually held. On that shoal, double and triple behave like unlocked.

Half, double and triple all free-run with a swap interval of 0 and are paced in code
(`ShoalRenderer.paceFrame`, a running deadline that restarts if a frame overruns), because the TV's driver cannot
be asked to do it:

- `eglSwapInterval` there is clamped to 1: a swap interval of 2 (`half rate`) still presents at 60 fps. The
  projector's Mali does honour swap intervals, but one pacing path is used everywhere so both devices behave
  the same.
- `Thread.sleep` there is bimodal: a 120 fps pacer that slept produced 120 fps on some runs and 21 fps on others
  (timer slack), so the pacer spins on `Thread.yield()` until the deadline instead. That is exact, but it keeps
  one core busy while waiting, which is why V-Sync is still the cheapest setting.

**Native 4K is not available.** The panel is 3840x2160, but the firmware pins the Android drawing surface to
1920x1080 and the TV upscales:

```
dumpsys window displays -> init=3840x2160 320dpi base=1920x1080 320dpi cur=1920x1080
```

`wm size 3840x2160` and `settings put global display_size_forced 3840x2160` are both ignored, and there is no
root on this set. Drawing at 4K into an offscreen target and scaling it down was measured and then removed: the
driver rejects a scaled `GL_LINEAR` blit (`GL_INVALID_OPERATION`), so it has to use `GL_NEAREST` and does not
improve the image, while drawing costs about 3.5 times more (at 100 fish, 84 fps instead of 678).

To revert the TV to the stock ambient screensaver:

```sh
adb shell settings put secure screensaver_components com.google.android.apps.tv.dreamx/.service.Backdrop
adb shell settings put secure screensaver_default_component com.google.android.apps.tv.dreamx/.service.Backdrop
adb shell pm uninstall com.shoal.screensaver
```

## Build and install

Toolchain is user-local (no root, no Gradle): JDK 17 in `~/Android/jdk17`, SDK in `~/Android/Sdk`
(`platforms;android-30`, `build-tools;34.0.0`), plus `glslangValidator` from the system.

```sh
./build.sh            # -> build/Shoal.apk (~53 KB)
./build.sh install    # + install on the one connected device, set it as the screensaver
ANDROID_SERIAL=<device-ip>:5555 ./build.sh install   # a device on network adb (connected first)
```

The build converts `../ShoalCore/shaders` (GLSL 4.30, the single source for every build) to GLSL ES 3.10 with
`tools/es_shaders.py` and validates them. The conversion adds explicit `0u` and `uint(...)`. It also makes
`fish.vert` read the boid buffer as per-instance vertex attributes, because the Mali has no vertex SSBOs.
`ShoalRenderer.java` binds the same GPU buffer as an array buffer, so nothing is copied.
If a desktop shader edit breaks the converter, it stops with a message naming the pattern.

## Using it

Open **Shoal** from the launcher. It opens the settings, which work with the remote:

| Entry | What it does |
|---|---|
| Preview | Shows the fish now; any key comes back |
| Screensaver | Shows whether Shoal is the screensaver; select it to switch between Shoal and the stock one (this firmware has no screensaver page in Android Settings) |
| Start after | Idle time before the screensaver starts (1 min to 2 h); writes the system `screen_off_timeout` |
| How many fish | 100 to 1,000 in steps of 100 (default 100). A fixed count, unlike the desktop's screen coverage. 1,000 is the maximum, chosen from the benchmark |
| Fish / Swimming | Size, size variation, tail, depth and shadows, speed, speed variation, schooling |
| Species / Species mix | Which of the 10 fish shapes swim, and whether they mix randomly or each colour is one species |
| Palette | 10 palettes of 5 colours (Shoal, Reef, Tropical, Ocean, Goldfish, Koi, Sunset, Neon, Pastel, Silver), shown as swatches. "Number of colours" uses the first N |
| Shoal by colour / Avoid other colours | Colour-sorted shoaling, as on the desktop |
| Frame rate | V-Sync, half, double or triple the panel refresh, or unlocked (see Frame rate above) |
| Reset to defaults | Restores the defaults below |

Defaults follow the desktop build, except for the values chosen on the TCL G10 TV on 2026-11-15: How many fish =
100, Size = Large (45), Size variation = Moderate (150), Speed = Normal (42), Speed variation = Maximum (10),
Depth and shadows = off, Number of colours = 3, Palette = Ocean, Shoal by colour = on, Frame rate = V-Sync, Show
frame rate = on. They live in `ShoalSettings.java` and `res/xml/preferences.xml`, which must agree. (The
projector's 2026-11-08 values were 700 fish, size variation 300, speed 25.)

Installing over an older build keeps the stored settings; the defaults above only apply to a fresh install or
"Reset to defaults". So the projector keeps its 700 fish, Koi palette and depth after an update, while the TV's
values stay at 100 fish and Ocean.

Changes apply the next time the screensaver or preview starts. "Screensaver" and "Start after" need
`WRITE_SECURE_SETTINGS` and `WRITE_SETTINGS`, which `./build.sh install` grants over adb.
The dream also declares this screen as its `settingsActivity`, so a launcher with a screensaver menu can open it.

The screensaver doesn't start while something keeps the screen on, such as a playing video.

For adb: the preview is `adb shell am start -n com.shoal.screensaver/.ShoalActivity`. Extras on that command
are saved as settings, using the desktop option names: `--ei speed 60 --ez showfps true --es palette reef`.
`--ei framerate N` sets the frame rate (0 V-Sync, 1 half, 2 double, 3 triple, 4 unlocked).
Setting `--es colour0 '#ff0000'` (colour0 to colour9) switches to a custom palette.
`--ez reset true` restores the defaults. `adb shell am start -n com.android.systemui/.Somnambulator`
starts the real screensaver now. Logs: `adb logcat -s Shoal` (frame rate every 10 s).

## Benchmark

The current fish (outline, species, spine chain), measured on 2026-11-08 at 1000 fish: 79 fps unlocked (simulation
7.8 ms, drawing 8.1 ms), and a steady 60 fps with V-Sync. Android uses a lighter mesh, 24 vertices per fish, drawn
indexed. The spine chain reaches the fish shader as five per-instance attributes, because the Mali has no
vertex-shader SSBOs, and through a `joint()` accessor, because local arrays are slow on it. Earlier steps:
the full desktop mesh 40 fps, indexed 63, spine with a copied array 41, accessor 45, Catmull-Rom and 6 + 2
segments 79. The table below is for the old oval fish.

`adb shell am start -n com.shoal.screensaver/.ShoalActivity --ez bench true` unlocks the frame rate (`eglSwapInterval 0`)
and steps the fish count up: 250, 500, 700, 1000, 1500 and so on, up to 10,000. The run stops once it drops below
5 fps. At each count it measures the unlocked frame rate over 4 s. It then measures the GPU time for the simulation
and for the draw, timed with `glFinish`. The results are in `adb logcat -s Shoal` (`BENCH` lines and a table at the end).
It uses the current settings apart from the fish count, and it doesn't save anything. It always runs unlocked,
whatever the frame rate setting says.

Result on 2026-11-08 (1024x600, fish length 14 px, size variation 300, depth and shadows on):

| Fish | Unlocked fps | Simulation ms | Draw ms |
|---:|---:|---:|---:|
| 250 | 286 | 1.3 | 4.7 |
| 500 | 207 | 2.6 | 5.6 |
| 700 | 149 | 4.2 | 6.2 |
| 1000 | 95 | 7.3 | 7.8 |
| 1500 | 52 | 15.3 | 8.1 |
| 2000 | 34 | 26.4 | 8.8 |
| 2500 | 23 | 40.0 | 9.5 |
| 3000 | 17 | 56.1 | 10.4 |
| 4000 | 9.8 | 96.5 | 12.1 |
| 5000 | 6.4 | 149 | 14.7 |
| 6000 | 4.6 | 222 | 16.5 |

- 60 fps holds up to about **1,300 fish**, so the settings stop at **1,000** (95 fps unlocked, about 1.6 times
  what 60 fps needs). The TCL default of 100 runs at about 678 fps unlocked.
- The simulation is O(n²): every fish checks every other fish, as in the desktop shader. Its cost grows about 4 times
  when the fish count doubles, and it dominates beyond 1,000 fish. Drawing grows only linearly. Most of the draw time
  is a fixed cost of about 4 ms (full-screen clears and the shadow pass).
- More fish would need a spatial grid in `sim.comp`, so that each fish checks only its neighbours.

Normal running keeps V-Sync (60 fps, less heat). `--ei framerate 4` on the preview command makes the screensaver
and preview run unlocked too, and `--ei framerate 0` restores V-Sync.

## Projector firmware quirk: lamp off and keys swallowed during dreams

Found in `services.jar` (from a dump of the projector's stock system image), all controlled by
**`persist.sys.screenSaver`** (factory value `1`):

- `PowerManagerService.handleSandman`: when a dream starts and the property is `1`, it sets
  `persist.screensave.running=1` and calls `PrjScreen.set_led_power(3, 0)`, which **switches the lamp off**.
  The factory "screensaver" is a lamp-off power saver.
- `PhoneWindowManager.interceptKeyBeforeQueueing`: while `persist.screensave.running=1`, every key except POWER
  is dropped ("dreaming only power key can wake up"). POWER switches the lamp back on and calls `awakenDreams()`.

`./build.sh install` sets `persist.sys.screenSaver=0` as root. It survives reboots, but a factory reset or a
reflash brings back `1`. Then the dream runs on a dark lamp again, so run the install step again.
`PhoneWindowManager.init` only resets the screensaver component at boot when the default is still
`com.android.dreams.web`, so the Shoal setting persists.

Revert to stock:
```sh
adb shell settings put secure screensaver_components com.htc.screensaver/com.htc.screensaver.ScreenSaverService
adb shell settings put secure screensaver_default_component com.htc.screensaver/com.htc.screensaver.ScreenSaverService
adb shell "su -c 'setprop persist.sys.screenSaver 1'"
adb uninstall com.shoal.screensaver
```

## Files

| Path | Purpose |
|---|---|
| `src/.../ShoalRenderer.java` | Port of `shoal.cpp`: world setup, compute step, instanced draw, FPS overlay, frame-rate pacing |
| `src/.../ShoalDream.java` | The DreamService (screensaver) |
| `src/.../ShoalSettingsActivity.java`, `res/xml/preferences.xml` | Settings screen (launcher tile) |
| `src/.../ShoalActivity.java` | Full-screen preview, saves intent-extra options |
| `src/.../ShoalSettings.java` | Options and defaults (SharedPreferences) |
| `src/.../ShoalPalettes.java`, `PalettePreference.java` | Palettes and the swatch picker |
| `src/.../ShoalView.java` | GLES 3 surface with depth and stencil |
| `tools/es_shaders.py` | Desktop GLSL to GLSL ES converter |
| `build.sh` | aapt2, javac, d8, zipalign, apksigner (debug key `~/.android/debug.keystore`) |
