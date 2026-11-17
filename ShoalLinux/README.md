# Shoal for Linux

The main Shoal build: SDL2 windows (one per display) around the shared engine in `../ShoalCore`, which does the world
setup, the OpenGL 4.3 compute-shader simulation and the drawing. This folder only has the settings, command line,
windows and frame pacing (`shoal.cpp`) and the settings dialog.

Build: `make` (needs SDL2, SDL2_image, OpenGL 4.3 through glvnd's libOpenGL, EGL, python3).
Install: `make install` (to `~/.local/bin/shoal`).

Run: `./shoal` for a full-screen screensaver on every display (any key, click or mouse movement exits after 1 s).
`./shoal --windowed` runs in a 1280x720 window (Esc quits).

Options use the same names as the Windows README and accept `/speed:50`, `--speed=50` or `--speed 50`:
`speed speedvar coverage boids rocks size sizevar sizebell tail shade coloursort colouravoid depth restrict showfps vsync fpsmode framerate randomness`,
plus the background and GPU options below. `--seconds=N` exits after N seconds and prints the frame rate.
Persistent settings live in `~/.config/shoal/settings.conf` (created on first run, same value names as the registry values).

Background (same names as the Plasma wallpaper's settings):
- `BackgroundMode=0|1`: plain water colour, or a picture under the water. Fish shadows fall on the picture.
- `BackgroundColor=#rrggbb`: the water colour.
- `BackgroundImage=/path`: the picture. Empty means Plasma's default wallpaper for your theme, the dark variant
  for dark schemes. `--picture /path` sets it and switches to picture mode.
- `BackgroundDim=0..100`: darkens the picture.

Species (shapes in `../docs/fish_gallery.png`): `Species=1,2,...` picks which swim, by number or name
(`--species=koi,tuna`, `all`). `SpeciesMode=0` gives a random mix and `1` one species per colour (`--speciesmode=colour`).
This is separate from `ColourSort`, which makes fish shoal with their own colour.

GPU: `Gpu=` empty (the default) renders on the dedicated GPU if there is one: here the RTX 2060, through NVIDIA's
PRIME render offload. A PCI address picks a GPU (`--gpu 0000:05:00.0` is the Vega), and `primary` means no offload.
The log says which GPU it got (`OpenGL ... on ...`).

Development options for working on the animation:
- `--follow` keeps fish 0 in the middle of the window, and `--zoom=N` magnifies it (default 4).
- `--capture=DIR` saves window 0's frames as PNGs after a 1.5 s warm-up, and `--frames=N` sets how many (default 60).

For example, `./shoal --windowed --follow --zoom=5 --boids=8 --speed=60`.

Settings dialog: `./shoal --settings` (or `/c`) opens a Qt window that edits `~/.config/shoal/settings.conf`; its
Preview button launches the screensaver. A 3D version is in `../ShoalLinux3D` (separate code, not on the engine).

Not ported: presets, Control Panel preview, desktop capture, debug logging.
To use as a lock/idle screensaver, point your idle tool at the `shoal` binary (for example xscreensaver, or swayidle/KDE
"Run command" idle actions).
