# ShoalCore: the shared engine

One copy of everything that makes the fish, used by every build:

| Path | What |
|---|---|
| `shaders/` | GLSL 4.30 compute and render shaders: `sim.comp` (flocking), `rock.comp`, `fish.vert/.frag`, `overlay.*` (rock outlines, FPS digits), `bg.*` (background picture) |
| `shoal_engine.h/.cpp` | World setup (fish, rocks), simulation step, drawing one or more views, background picture. Standard C++ and OpenGL 4.3 only |
| `shoal_gl.h` | Where the GL functions come from: glvnd's libOpenGL on Linux, `shoal_gl_win.cpp` (loader) on Windows |
| `shoal_fishshape.h/.cpp` | CPU fish outlines from the species table in the embedded `fish.vert`, for previews |
| `include/` | Khronos GL headers for the Windows build |
| `shoal_linux.h/.cpp` | Linux helpers: GPU list (sysfs), PRIME render offload for a standalone window, Plasma's default wallpaper |
| `embed_shaders.py` | Compiles `shaders/` into a C++ file (`ShoalShaderSource()`), run by each build |
| `tools/fish_gallery.py` | Draws every species from the table in `fish.vert` into `docs/fish_gallery.png` |

## The fish

`fish.vert` draws each fish from above as one outline: a body strip, a tail fin forked or rounded in `fish.frag`, and
two side fins. The centre line is a chain of 10 points per fish (the "spine", buffer binding 3), which `sim.comp`
(`updateSpine`) keeps like train carriages. The head is rigid, and the simulated position is the pivot, 0.3 lengths
behind the nose. Every point after the head is pulled along behind the one in front at a fixed distance. So the
body follows the path the head took and never stretches. `fish.vert` draws a Catmull-Rom curve through the chain and
adds the swimming wave across it. `sim.comp` advances the wave by distance travelled: one beat per 0.665 lengths,
which is about 0.7 slip. A fish drawn indexed shares each outline point between triangles: 40 vertices
on desktop (12 body and 4 fin segments), 24 on Android (6 and 2). Turns: a sharp turn drives a stronger tail stroke and
faster beat (`finishSwim`), braking stops at half speed, and the screen-edge clamp turns the heading gradually.

Species: the table `SPECIES` in `fish.vert` holds 10 shapes (Classic, Sardine, Goldfish, Koi, Tuna, Trout, Pike, Tetra,
Fantail, Mackerel), 11 numbers each, which are explained next to it. To change or add one, edit the table, raise
`SPECIES_COUNT`, add the name to `kShoalSpeciesNames` (`shoal_engine.cpp`) and the settings lists, then run
`python3 tools/fish_gallery.py ../docs/fish_gallery.png` to see it. Settings: `speciesMask` (which species swim) and
`speciesMode` (0 = random mix, 1 = one species per colour). Both go to the shader in the `SpeciesSet` uniform block
(binding 2).

Users:
- `../ShoalLinux`: standalone screensaver, SDL2.
- `../ShoalWallpaper`: Plasma live wallpaper. Adds desktop obstacles, a see-through mode over a QML picture, and the
  offload thread.
- `../ShoalAndroid`: a Java port of the engine. `tools/es_shaders.py` converts `shaders/` to GLSL ES 3.10.
- `../ShoalWindows`: the Windows screensaver (WGL). GL functions come from `shoal_gl_win.cpp`, and the dialog preview
  from `shoal_fishshape.cpp`. `../ShoalNative`, the old D3D11 build with its own HLSL, is kept only for reference.

API in short: `init()` with a GL 4.3 context current, then each frame `setSettings`/`setMonitors`/`setObstacles` as
needed, `step(dt)` once, and `draw(view)` per window. The world is in pixels: monitors are rectangles in one shared
space, and a view shows one rectangle of it. Setting changes that reshape the shoal (amount, size, rocks) rebuild it.

A backup of the sources from before this engine existed is in `../backups/shoal-before-core-2026-11-08.tar.gz`.
