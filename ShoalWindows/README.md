# Shoal for Windows (OpenGL, shared engine)

The Windows screensaver on the shared engine (`../ShoalCore`), with the same fish, species, simulation and shaders as
the Linux, Plasma and Android builds. It replaces the Direct3D 11 build in `../ShoalNative`, which is kept unchanged for
reference. The settings, registry values (`HKCU\Software\Shoal`), presets, command-line options and settings dialog
are carried over from it, so existing settings keep working.

Needs OpenGL 4.3, which any DirectX 11-class GPU from NVIDIA, AMD or Intel has.

## Build (on Linux) and install

```sh
./build.sh           # -> build/Shoal.scr (and build/Shoal.exe, the same file)
./build.sh deploy    # also copies it to the Windows PC over SSH and installs C:\Windows\System32\Shoal.scr
```

Cross-compiled with `clang-cl`, `lld-link` and `llvm-rc` against the Windows SDK and MSVC C runtime that `xwin` unpacked
into `~/xwin/sdk`. Accepting Microsoft's licence is part of that step:
`xwin --accept-license --arch x86_64 splat --output ~/xwin/sdk`. The C runtime is linked statically, so nothing else
has to be installed on the target PC. The deploy target is `$SHOAL_WIN`, an SSH target such as `user@windows-pc`; the
account must be an administrator to write to System32. Installing fails while the screensaver is running.

## What's new compared with the D3D11 build

- The new fish: an outline shape, a body that follows the head's path, a swimming wave locked to distance, and
  10 species (see `../ShoalCore/README.md`).
- Settings dialog: a **Fish Species** row (ten checkboxes) and **Species mix** (Random mix or One per colour).
  Registry values `SpeciesMask` (bits, species 1 = bit 0) and `SpeciesMode`; presets include both. The live preview
  draws the new outlines from the same species table (`shoal_fishshape`), with the tail beat tied to speed.
- Presentation through DXGI flip-model swap chains, one per monitor, as in the D3D11 build. OpenGL draws offscreen,
  and the frame is copied upright into a Direct3D texture shared through `WGL_NV_DX_interop2`. Direct3D copies that
  to the swap chain and presents it, with tearing allowed when V-Sync is off. With V-Sync, each swap chain's frame
  latency waitable object paces its own monitor. On the test PC that gives 75 fps on the 75 Hz monitor and 120 on
  the 120 Hz one; with V-Sync off and Frame Rate 2x, 150 and 240. The first version presented OpenGL directly with
  `SwapBuffers` from one context on two windows. Its frame-rate counter showed the shared simulation step rate (about
  195), and the driver's presentation mode could stall a monitor after about 30 seconds. Without the interop
  extension, that direct path is still the fallback, paced by a `D3DKMTWaitForVerticalBlankEvent` thread per monitor.
- The frame-rate counter shows each monitor's own frame rate.
- Show the desktop behind the fish: one capture of the whole virtual desktop, handed to the engine as a picture
  placed on the desktop. Each monitor shows its own part of it.
- Full-screen windows are per-monitor DPI aware (real pixels on scaled displays). The dialog keeps system scaling.
- `/seconds:N` exits after N seconds. `/species:1,3,9` and `/speciesmode:0|1` override the settings.
- A log at `%LOCALAPPDATA%\Shoal\shoal.log` records the GL renderer, the presentation path, each window's size and
  refresh rate, the fish count, and every 2 s each window's frame rate and draw-and-present time.

Not carried over: `/debuglog` (the boid CSV logs and `tools/analyze_*.py` input) and the "D3D11 GPU Not Found" text.
Without OpenGL 4.3 it says so instead, and the Control Panel preview stays blank.

## Files

| Path | What |
|---|---|
| `shoal_win.cpp` | The program: settings, dialog, monitor windows with one shared WGL 4.3 context, DXGI presentation, frame loop, preview, live wallpaper |
| `tools/desktop_probe.cpp` | Finds the desktop layer behind the icons and tests what shows there |
| `Shoal.rc` | Name for the Screen Saver Settings list, and version info |
| `build.sh` | Cross-build and deploy |
| `../ShoalCore/shoal_gl_win.h/.cpp` | Loads the 59 GL functions the engine uses (`wglGetProcAddress`, then `opengl32.dll` for GL 1.1) |
| `../ShoalCore/include/` | Khronos `glcorearb.h` and `khrplatform.h` |
| `../ShoalCore/shoal_fishshape.h/.cpp` | CPU fish outlines for the dialog preview |

## Testing on the Windows PC from Linux

Programs started over SSH run in a background session without a screen. To see Shoal on the desktop, start it
with a one-off scheduled task in the interactive session, for example
`schtasks /create /tn ShoalRun /tr "\"C:\Users\Charlton\Shoal.exe\" /s /seconds:8 /showfps:1" /sc once /st 23:59 /it /f`
and then `schtasks /run /tn ShoalRun`. Quote the program path, or the task fails with 0x80070002. Take screenshots
the same way, with a PowerShell `CopyFromScreen` script. **Delete the tasks afterwards**: "once at 23:59" really runs
at 23:59. Turn the screensaver off while testing (SystemParametersInfo SPI_SETSCREENSAVEACTIVE, run in the session),
or it starts on top after a minute. Screen grabs only see the fish with the DXGI path; directly presented OpenGL
windows come out black.

The previous D3D11 `Shoal.scr` is backed up on that PC as `%USERPROFILE%\Shoal-D3D11-backup.scr`.

## Live wallpaper

`Shoal.scr /wallpaper` (or **Start live wallpaper** in the settings dialog) puts the fish behind the desktop icons on
every monitor, like the Plasma wallpaper. It runs in the background with a fish icon in the notification area.
Right-click it for the options; double-click it to open the fish settings:

| Tray menu | What it does |
|---|---|
| Fish settings... | The screensaver's settings dialog. The wallpaper uses the same fish settings and picks up changes when you click OK |
| Pause | Stops drawing |
| Show my wallpaper under the water | Each monitor's own Windows wallpaper (spanned if Windows spans it); otherwise the water colour |
| Swim around desktop icons / windows | Icons are merged into a few hard blocks: gaps narrower than about two fish lengths are filled, plus a 12 px margin. Windows are soft: fish steer around them but swim out if one is moved over them. A window over 85% of a monitor is ignored, and the fish swim under it |
| Keep away from the mouse pointer | The pointer is a small soft obstacle |
| Pause a screen while a window fills it | A monitor stops while a window covers 95% or more of it |
| Frame rate | Every refresh, half, a third or a quarter, of each monitor's own refresh rate |
| Start with Windows | `HKCU\...\Run\Shoal Wallpaper` runs `Shoal.scr /wallpaper` at sign-in |
| Quit | Removes the fish and gives the desktop its normal wallpaper back |

It also pauses while a full-screen program or presentation runs (`SHQueryUserNotificationState`) and while the PC is
locked. It re-attaches after Explorer restarts (`TaskbarCreated`), restarts itself when monitors change, and follows a
slideshow (`WM_SETTINGCHANGE`, plus a check every 30 s). Options live in `HKCU\Software\Shoal\Wallpaper`. The log is
`%LOCALAPPDATA%\Shoal\wallpaper.log` (monitors, pictures, icon and window counts, pauses, frame rate every 2 s).
`/wallpaper:quit` stops a running wallpaper, and `/wallpaper:restart` restarts it.

How it attaches: each monitor gets a child window of the desktop's wallpaper `WorkerW` (see the probe below),
presented through DXGI. The compositor paces that whole desktop tree at the primary monitor's rate. So each window
draws just after its own monitor's vertical blank instead (a `D3DKMTWaitForVerticalBlankEvent` thread per monitor),
presents without waiting, and the compositor shows the frame at the next refresh. On the test PC that gave 75 and
120 fps with no late or doubled frames; the log counts both. An earlier version paced each window with a timer at
the monitor's nominal rate. That timer drifted against the real refresh, and the 75 Hz monitor stuttered.

Opening the settings (tray menu, dialog button, restart) uses `CreateProcess`, not `ShellExecute`: Windows' "open"
verb for a `.scr` file runs the screensaver (`"%1" /S`) and drops the arguments. Icon positions come from the
desktop's `IFolderView` (through `IShellWindows`). Pictures come from `IDesktopWallpaper`, decoded with WIC and
shrunk to cover the monitor.

## Live wallpaper: desktop layer probe (2026-11-08)

`tools/desktop_probe.cpp` is a small program that finds the desktop layer behind the icons and attaches a test
window to it. Run it as `desktop_probe.exe [worker|progman|layered] [dxgi]`; it logs to
`%LOCALAPPDATA%\Shoal\desktop_probe.log`. Findings on the test PC (Windows 11 build 26300):

- Layout from 24H2 on: `SHELLDLL_DefView` (the icons) and the wallpaper `WorkerW` are both children of `Progman`.
  The old top-level WorkerW pair is gone. Sending `0x052C` to Progman leaves this layout as it is.
- `Progman` has `WS_EX_NOREDIRECTIONBITMAP`: Explorer composes the desktop with DirectComposition. A child window that
  draws with GDI is never shown, whether it's a child of the WorkerW or of Progman below the icon view.
- A child window that presents through a DXGI flip-model swap chain **is** shown behind the icons, across both
  monitors, in both places: as a child of the wallpaper WorkerW, or of Progman just below `SHELLDLL_DefView`. Shoal
  already presents this way.
- A layered child window (`WS_EX_LAYERED`) fails without an application manifest that declares Windows 8+.
- Writing the current wallpaper back with `SPI_SETDESKWALLPAPER` afterwards restores the normal desktop.
