# Test results

## Speed × randomness matrix (2026-10-13)

Each case is a 40 s run:

```
build\ShoalNative.exe /debuglog:40 /speed:S /randomness:R
```

Settings:

- 503 boids
- size 20
- `RestrictToMonitor=1`
- two 1920×1080 monitors
- GPU simulation (`# simulation=gpu` in every log)

Tools:

- Grouping: `tools\analyze_grouping.py`
- Stalls: `tools\analyze_boid_log.py`

The other columns come from the raw log columns.

### Columns

| Column | Meaning |
|--------|---------|
| Stalls | Stalls below 20% speed lasting at least 0.5 s |
| Min speed | Lowest logged speed fraction |
| Speed < 10 px | Mean speed fraction within 10 px of a wall |
| Central ¼ | Share of boids in the central quarter of each monitor; 0.25 if spread evenly |
| Grouped | Share in clusters of 5+ |
| Neighbors | Mean boids within 90 px; uniform random is about 3.0 |
| Align | Local heading alignment; uniform random is about 0.47 |

### First pass: problem found

With the wall look-ahead at `max(48, speed * 0.75)`:

| Speed | Rand | Stalls | Min speed | Central ¼ | Grouped | Neighbors | Align |
|------:|-----:|-------:|----------:|----------:|--------:|----------:|------:|
| 1    | 0   | 0 | 0.85 | —    | 0.556 | 3.7  | 0.843 |
| 1    | 100 | 0 | 0.80 | —    | 0.383 | 3.2  | 0.535 |
| 500  | 0   | 0 | 0.80 | 0.58 | 0.979 | 59.4 | 0.936 |
| 500  | 100 | 0 | 0.76 | 0.54 | 0.753 | 6.4  | 0.397 |
| 1000 | 0   | 0 | 0.80 | 0.92 | 0.993 | 89.6 | 0.917 |
| 1000 | 100 | 0 | 0.80 | 0.93 | 0.952 | 28.6 | 0.564 |

At high speed, the avoidance zone covered most of the screen. At speed 1000, it was 750 px on a 1080 px tall monitor. Boids were squeezed into the middle, which also made randomness 100 look grouped.

### After the look-ahead fix

The look-ahead is now `clamp(speed * 0.3, 48, 20% of the monitor's shorter side)`. Speed 1 is unaffected; its look-ahead is 48 px either way.

| Speed | Rand | Stalls | Min speed | Speed < 10 px | Off-screen | Central ¼ | Grouped | Neighbors | Align |
|------:|-----:|-------:|----------:|--------------:|-----------:|----------:|--------:|----------:|------:|
| 81   | 0   | 0 | 0.85 | 0.97 | 0 | 0.12 | 0.968 | 22.6 | 0.975 |
| 81   | 100 | 0 | 0.85 | 0.99 | 0 | 0.23 | 0.376 | 3.5  | 0.534 |
| 500  | 0   | 0 | 0.76 | 0.96 | 0 | 0.31 | 0.931 | 27.2 | 0.918 |
| 500  | 100 | 0 | 0.74 | 0.87 | 0 | 0.28 | 0.434 | 3.8  | 0.490 |
| 1000 | 0   | 0 | 0.65 | 0.81 | 0 | 0.32 | 0.836 | 14.9 | 0.838 |
| 1000 | 100 | 0 | 0.67 | 0.83 | 0 | 0.31 | 0.421 | 3.8  | 0.482 |

### Observations

- **Walls:** There were no stalls and no boids off-screen at any speed.
  - At 1000 px/s, boids reach the wall more often: about 40 samples within 2 px in 40 s.
  - They slide along the wall at 65% speed or more, and brake only while closing on it.
- **Randomness 100:** Results are consistent across speeds and close to uniform random: 3.5–3.8 neighbors and 0.48–0.53 alignment.
- **Randomness 0:** Boids flock at every usable speed.
  - Grouping weakens a little at 1000 (0.84 grouped), because flocks are broken up by walls far more often.
- **Speed 1:** Boids move only about 40 px in 40 s, so groups never get a chance to form within the test. Grouping numbers at this speed mostly reflect the initial random scatter.
- **Speed 81:** The lower look-ahead (48 px instead of 61 px) matches the earlier runs within run-to-run variation. The central quarter held 12–16% of boids at randomness 0 and 23–25% at randomness 100.

## Crossing between monitors and rocks

These runs used `RestrictToMonitor=0` and lasted 30 s with 503 boids. They were made after the frame-loop fix (240 fps).

### Crossing between monitors

| Speed | Rand | Stalls | Off-screen | Crossings / 30 s |
|------:|-----:|-------:|-----------:|-----------------:|
| 81   | 0/100 | 0 | 0 | 165–215 |
| 1000 | 0/100 | 0 | 0 | 2,700–2,900 |

### Rocks (10 rocks)

| Speed | Version | Rows within 5 px of a rock | Mean speed there | Rows below 60% speed (after 2 s) | Inside a rock | Stalls |
|------:|---------|-----:|-----:|-----:|--:|--:|
| 81   | Before | 1,648 | 0.45 | 1,654 | 0 | 0 |
| 81   | After  | 0 (closest 14.7 px) | — | 0 | 0 | 0 |
| 1000 | Before | 1,859 | 0.92 | 0 | 0 | 0 |
| 1000 | After  | 521 | 0.78 (min 0.72) | 0 | 0 | 0 |

- **Before:** A separate pass turned fish toward the nearest rock edge's normal while flocking pulled them back, so fish crawled along rocks at the 30% braking floor.
- **After:** Rocks feed into the same weighted obstacle steering as walls in the main shader. The separate pass is now only a penetration safety net.
- **Walls:** The low wall speeds seen earlier in crossing runs (0.31 at speed 81) were also caused by flocking that was 4× too strong at 240 fps. With frame-rate-independent flocking, the minimum speed near walls is 1.00 at speed 81.

Grouping after the fix, with no rocks:

| Speed | Rand | Grouped | Neighbors | Align |
|---:|---:|---:|---:|---:|
| 81 | 0 (10 rocks) | 0.969 | 29.6 | 0.959 |
| 1000 | 0 | 0.871 | 18.6 | 0.873 |
| 81 | 100 | 0.392 | 3.5 | 0.542 |

## Crossing the monitor seam with V-Sync on and mixed refresh rates

These runs used `RestrictToMonitor=0` with V-Sync on. Monitor 0 ran at 71.925 Hz and monitor 1 at 60 Hz. Each run lasted 30 s with 503 boids, randomness 0 and no rocks. Crossings were checked between consecutive 0.05 s samples of the same boid.

| Speed | Crossings checked | Max step / expected step | Jumps > 1.5× | Speed at crossing (min / mean) | Max heading change | Slow (< 0.6) within 30 px of the seam | Off-screen | Stalls |
|------:|------------------:|---------------:|---:|---:|---:|--:|--:|--:|
| 81   | 21 | 1.02 | 0 | 1.00 / 1.00 | 12.8° | 0 | 0 | 0 |
| 1000 | 65 | 1.01 | 0 | 0.86 / 0.99 | 38.3° | 0 | 0 | 0 |

- **Seam behavior:** Boids cross the full height of the seam (y 23–1039) without teleporting, braking or turning back. The only slight slowdown (0.86 at speed 1000) is from ordinary flocking and turning, not from the seam.
- **Grouping:** Grouping was normal: 0.952 grouped at speed 81 and 0.877 at speed 1000.
- **Rendering:** Each window draws every boid and lets the viewport clip it. With the restriction off, there is no position clamp in the render shader. A screenshot of a 240 px band across the seam showed fish straddling the edge drawn whole, half on each monitor.

## Fish size (2026-10-13)

Each case is a 25 s run:

```
build\ShoalNative.exe /debuglog:25 /size:P /speed:S /restrict:0 /vsync:1 /rocks:10 /randomness:0
```

503 boids, 10 rocks, GPU simulation in every log, analyzed with `tools\analyze_size.py` (2 s warm-up).

- **nn/L:** median nearest-neighbor distance divided by fish length.
- **Overlap:** share of fish whose nearest neighbor is closer than L/4.
- **Wall clip:** share of fish whose nose or tail is past a blocking screen edge.
- **Rock nose:** share of fish whose nose is inside a rock (exact, from the `# rock` outlines).

### Before size scaling (pixel sizes, fixed spacing)

| Size | Speed | nn/L | Overlap | Wall clip | Rock nose* | Stalls |
|---:|---:|---:|---:|---:|---:|---:|
| 10 px | 81 | 1.71 | 1.6% | 0 | 0 | 0 |
| 20 px | 81 | 0.74 | 9% | 0 | 0 | 0 |
| 50 px | 81 | 0.30 | 41% | 0 | 0.2% | 0 |
| 100 px | 81 | 0.16 | 75% | 0.7% | 2.9% | 0 |
| 200 px | 81 | 0.07 | 96% | 3.9% | 15% | 0 |

\* Upper bound (center within L/2 of a rock); rock outlines were not logged yet.

### After size scaling (percent of screen height)

| Size | Length | Speed | nn/L | Overlap | Wall clip | Rock nose | Slow rows | Stalls |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1% | 11 px | 81 | 1.10 | 4.4% | 0 | 0 | 0 | 0 |
| 1.9% | 21 px | 81 | 0.83 | 6.6% | 0 | 0 | 0 | 0 |
| 5% | 54 px | 81 | 0.47 | 21% | 0 | 0 | 0 | 0 |
| 10% | 108 px | 81 | 0.29 | 41% | 0 | 0 | 5 | 0 |
| 1% | 11 px | 1000 | 1.91 | 2.2% | 0 | 0 | 0 | 0 |
| 10% | 108 px | 1000 | 0.28 | 43% | 0 | 0 | 0 | 0 |

- Mean speed stays at 1.00 (0.95–0.97 at speed 1000, which includes normal wall turns). Alignment stays 0.81–0.98 at speed 81.
- The current size (1.9%) behaves like the old 20 px baseline.
- At 10%, 503 fish fill about a third of two 1080p screens, so some overlap is physically unavoidable. Overlap fell from 75% (100 px before) to 41%.
- Found and fixed a spawn bug: `RandomFloat(left, right)` can return exactly `right`, which is outside every monitor, so the fish had no walls and swam off screen. Fish now spawn inset by one body length, and a fish outside every monitor is assigned to the nearest one so the clamp brings it back.
- Tuning steps that were rejected:
  - Separation ×√k: less overlap reduction (48%) and 631 slow rows at walls.
  - Look-ahead ×√k for every speed: at speed 1000 the wall zone covered almost the whole screen height and squeezed the school (84% overlap). Only the look-ahead minimum is scaled now.

## Fish count (2026-10-13)

### Behavior

Each case is a 25 s run:

```
build\ShoalNative.exe /debuglog:25 /boids:N /speed:S /size:P /restrict:0 /vsync:1 /rocks:10 /randomness:0
```

GPU simulation in every log. Stalls come from `analyze_boid_log.py`, grouping from `analyze_grouping.py`, and the rest from `analyze_size.py`.

| Fish | Speed | Size | Isolated | Grouped (5+) | Neighbors | Align | Overlap | Wall clip | Rock nose | Stalls |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 81 | 1.9% | 53% | 0% | 0.5 | 1.00 | 0 | 0 | 0 | 0 |
| 50 | 81 | 1.9% | 13% | 41% | 4.0 | 0.97 | 3.0% | 0 | 0 | 0 |
| 150 | 81 | 1.9% | 1.3% | 80% | 8.0 | 0.97 | 4.3% | 0 | 0 | 0 |
| 503 | 81 | 1.9% | 0.1% | 97% | 24.6 | 0.97 | 11% | 0 | 0 | 0 |
| 750 | 81 | 1.9% | 0.1% | 97% | 36.9 | 0.95 | 11% | 0 | 0 | 0 |
| 1000 | 81 | 1.9% | 0.1% | 98% | 38.8 | 0.97 | 13% | 0 | 0 | 0 |
| 10 | 1000 | 1.9% | 67% | 0% | 0.4 | 0.87 | 0 | 0 | 0 | 0 |
| 1000 | 1000 | 1.9% | 1.0% | 90% | 18.3 | 0.76 | 13% | 0 | 0 | 0 |
| 1000 | 81 | 10% | 0.6% | 93% | 9.9 | 0.79 | 57% | 0 | 0 | 0 |

- Wall and rock handling is independent of count: there are no stalls, clips or rock hits in any case.
- The 1000-fish, 10% case has 731 slow rows (fish briefly below 60% speed while packed against walls), but no stalls.
- With 10–50 fish on two 1080p screens, fish rarely come within the 90 px neighbor radius, so few groups form.

### Performance

Each case is a 12 s run at unlimited frame rate (`/vsync:0 /fpsmode:5 /noboidlog /rocks:10 /showfps:1`) with mixed 71.925 Hz and 60 Hz monitors.

| Fish | 10 | 50 | 150 | 503 | 750 | 1000 |
|---|---:|---:|---:|---:|---:|---:|
| FPS per monitor | 3221 | 3205 | 3185 | 2683 | 2335 | 2041 |
| Frame interval p50 (ms) | 0.29 | 0.29 | 0.30 | 0.36 | 0.41 | 0.48 |

- The GPU flocking pass is O(n²), but even 1000 fish cost under 0.5 ms per frame.
- The occasional 10–16 ms maximum intervals at unlimited are Present calls blocking in the compositor. They do not appear with vsync.
- With vsync and 1000 fish, the monitors run at 71.9 and 60.0 fps, with exactly 1.00 frame per vblank, 0 repeats and p99 intervals of 14.21 and 16.90 ms.

### Low-count seeking (2026-10-13)

With only a 90 px neighbor radius, 10–50 fish rarely met (table above). Fish with fewer than 4 neighbors now steer toward the nearest fish outside their group and sprint up to 15% faster.

10 fish, 60 s, size 1.9%, 10 rocks. Largest group / number of groups over time (groups linked within 90 px):

| Case | 0 s | 16 s | 32 s | 48 s | 58 s |
|---|---|---|---|---|---|
| Speed 81, before | 1/10 | 1–2 fish groups, 53% alone (25 s run) | | | |
| Speed 81, seek only | 1/10 | 4/5 | 4/3 | 8/2 | 10/1 |
| Speed 81, seek + sprint | 1/10 | 4/4 | 5/2 | 5/2 | 5/3 |
| Speed 81, restrict, seek only | 1/10 | 4/5 | 4/4 | 4/4 | 4/4 |
| Speed 81, restrict, seek + sprint | 1/10 | 3/5 | 5/4 | 5/2 | 5/2 |
| Speed 1000, seek + sprint | 1/10 | 3/7 | 3/6 | 2/8 | 3/8 |

- With seek only, lone fish chasing a shoal could never catch it because all fish swam at the same speed (restrict: a straggler stayed alone on each monitor). The sprint fixes this: each monitor's 5 fish form one shoal.
- At speed 1000, a fish crosses the 90 px neighbor radius in under 0.1 s and meets a wall every 2 s, so 10 fish still rarely group. 1000 fish at speed 1000 group fine (90%).

Regression, 30 s at speed 81:

| Fish | Randomness | Isolated | Grouped | Align | Stalls |
|---:|---:|---:|---:|---:|---:|
| 50 | 0 | 6% | 56% | 0.98 | 0 |
| 50 | 100 | 65% | 0% | 0.74 | 0 |
| 503 | 0 | 0% | 99% | 0.97 | 0 |
| 503 | 100 | 4% | 41% | 0.53 | 0 |
| 1000 | 0 | 0% | 99% | 0.97 | 0 |

Randomness 100 is unchanged (seek is scaled by `1 − randomness/100`). Maximum speed fraction is 1.15. No wall clips, rock hits or stalls.

### Stronger sprint: 150% when alone (2026-10-13)

The sprint boost was raised from 15% to 50%. The cruise speed is `speed × (1 + 0.5 × (1 − neighbors/4) × (1 − randomness/100))`:

| Neighbors | 0 | 1 | 2 | 3 | 4+ |
|---|---|---|---|---|---|
| Speed | 150% | 137.5% | 125% | 112.5% | 100% |

Fish accelerate at `speed × 1.5`/s and ease back down at `speed × 0.5`/s, so a fish slows from 150% to 100% over about 1 s.

10 fish, 60 s (largest group / number of groups):

| Case | 0 s | 16 s | 32 s | 48 s | 59 s | Mean speed |
|---|---|---|---|---|---|---|
| Speed 81 | 1/10 | 3/5 | 5/4 | 7/3 | 7/3 (9/2 at 53 s) | 1.25 |
| Speed 81, restrict | 1/10 | 4/4 | 4/4 | 4/4 | 5/3 | 1.25 |
| Speed 1000 | 1/10 | 1/10 | 2/8 | 1/10 | 1/10 | 1.44 |

30 s runs, 10 rocks:

| Fish | Speed | Size | Randomness | Isolated | Grouped | Mean speed | Time above 120% |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 50 | 81 | 1.9% | 0 | 2% | 62% | 1.12 | 34% |
| 503 | 81 | 1.9% | 0 | 0% | 98% | 1.01 | 1.5% |
| 503 | 81 | 1.9% | 100 | 5% | 40% | 1.00 | 0% |
| 50 | 1000 | 1.9% | 0 | 32% | 5% | 1.30 | 78% |
| 50 | 81 | 10% | 0 | 23% | 20% | 1.00 | 0.1% |

- At speed 81, 10 fish built larger shoals (up to 9) than with the 15% sprint (5). With 50 fish, grouping rose from 56% to 62%.
- At speed 1000, lone fish sprint almost all the time with no gain in grouping, because they pass each other even faster.
- 503+ fish are almost never alone, so their speed is effectively unchanged.
- Maximum speed fraction is 1.50. No wall clips, rock hits or stalls in any run, including 150% sprints at speed 1000.

### Speed variability (2026-10-13)

The lone-fish sprint above was replaced by the Speed Variability setting. At variability 0, every fish swims at exactly Speed. See [speed-variability.md](speed-variability.md) for the design and full results. No stalls, wall clips or rock hits occurred at any variability, speed (1, 81, 1000), count or size tested.


### Fish coverage slider (2026-10-13)

The Fish Amount slider now sets coverage of free screen area (monitors minus rocks) instead of an absolute count. Count = coverage x free area / (length x length/4), clamped to 2-5000.

| Override | Rocks | Fish |
|----------|-------|------|
| `/coverage:0 /size:10` | 0 | 2 |
| `/coverage:0 /size:10` | 10 | 2 |
| `/coverage:100 /size:1` | 0 | 5000 |
| `/coverage:100 /size:1` | 10 | 4814 (rocks took 3.7% of the area) |
| `/coverage:69 /size:1.9` (migrated saved 503) | 0 | 500 |
| `/coverage:50 /size:5` | 5 | 40 |

Performance with 10 rocks at size 1%:

| Fish | VSync | fps | Notes |
|------|-------|-----|-------|
| 1000 | off, unlimited | 2081 | |
| 5000 | off, unlimited | 716 | 1.37 ms frame interval p50 |
| 5000 | on | 71.9 / 60.0 | 1.00 frame per vblank on each monitor, 0 hitches |

The O(n^2) GPU flocking pass is fast enough at 5000 fish, so no tiling optimisation was needed. Behaviour at 4828 fish showed 0 stalls.

### Maximum raised to 10000 fish (2026-10-13)

Coverage at slider position 100 is now 7.29%, giving 10000 fish at size 1% on this desktop (9644 with 10 rocks). Performance with 10 rocks at size 1%:

| Fish | VSync off, unlimited fps | Frame interval p50 / p99 | VSync on |
|------|--------------------------|--------------------------|----------|
| 2500 | 1204 | 0.81 / 1.06 ms | |
| 5000 | 718 | 1.37 / 1.65 ms | 71.9 / 60.0, 0 hitches |
| 7500 | 506 | 1.94 / 2.29 ms | 71.9 / 60.0, 0 hitches, 1.00 frame per vblank |
| 10000 | 381 | 2.59 / 2.96 ms | 72.0 / 60.0, 0 hitches, 1.00 frame per vblank |

The frame time grows almost linearly, at about 0.25 ms per 1000 fish. At 10000 fish the GPU uses about 2.6 ms of the 13.9 ms budget at 72 Hz. Linear extrapolation puts the 72 Hz limit near 50000 fish, but the O(n^2) neighbour pass would eventually dominate.

Behaviour at 9644 fish (6 s): 0 stalls, 0 wall clips, 0 rock touches, mean speed 1.01x, overlap 0.15.

## Code cleanup regression check

Removed dead state, merged duplicated command-line parsers and monitor enumeration, and shared HLSL between the compute shaders. Main.cpp went from 2798 to 2709 lines. /W4 now reports no warnings.

| Run | Result |
|-----|--------|
| /debuglog:8 /rocks:10 (saved defaults, 1013 fish) | simulation=gpu, 0 stalls, 0 wall clips, 0 rock touches, overlap 0.11 |
| /debuglog:8 /restrict:0 /rocks:0 /showfps:1 | simulation=gpu, 0 stalls, 0 wall clips, 120 monitor crossings |
| /boids:10000 /vsync:0 /fpsmode:5 /noboidlog | 383 fps (381 before cleanup), 0 hitches |
| Settings dialog | All value labels populate |

## CPU fallback performance (/cpu)

Uncapped (/vsync:0 /fpsmode:5 /noboidlog), 6 s each. The GPU figures are from the 10000-fish sweep above.

| Fish | CPU fps | CPU sim time p50 | GPU fps |
|------|---------|------------------|---------|
| 500 | 387 | 2.2 ms | - |
| 1000 | 104 | 8.8 ms | - |
| 2500 | 17.3 | 56 ms | 1204 |
| 5000 | 4.4 | 231 ms | 718 |
| 10000 | 1.1 | 944 ms | 383 |

Simulation time grows exactly as n²: the neighbour loop compares every pair. Rendering stays cheap (3.4 ms at 10000).

Saved defaults (1010 fish, VSync on, 10 rocks, boid log on): 44/46 fps instead of 72/60. Sim p50 is 9 ms, but it runs about 89 times a second because two windows on different refresh rates each trigger a step. Behaviour has also drifted from the GPU path: 2 stalls, rock touches 0.040, mean speed 0.90x. The GPU path gives 0 stalls, 0.000 and 1.05x.

## CPU fallback removed

Following the measurements above, the CPU simulation and CPU fish drawing were removed (Main.cpp 2710 -> 2370 lines). Shoal now needs a feature level 11_0 GPU. If the device, compute shaders or instanced render shaders are unavailable, it closes its windows and shows "D3D11 GPU Not Found", then exits with code 1.

| Run | Result |
|-----|--------|
| /debuglog:8 /rocks:10 | 0 stalls, 0 wall clips, 0 rock touches, 71.9/59.9 fps, 0 hitches |
| /debuglog:8 /restrict:0 /rocks:0 /showfps:1 | 0 stalls, 0 wall clips, 71.9/60.1 fps |
| /boids:10000 /vsync:0 /fpsmode:5 /noboidlog /showfps:1 | 383 fps, 0 hitches |
| Temporary build with a broken compute shader | Error box shown, screensaver windows closed, exit code 1 |

## Control Panel preview fixed

- Cause: Windows starts the preview as `Shoal.scr /p <hwnd>` (with a space). Shoal only parsed `/p:<hwnd>`, so it fell through to an unhandled mode, and the preview stayed blank.
- Fix:
  - `/p` and `/c` accept both the space and the colon forms.
  - The preview now simulates the full primary screen (1920x1080) and renders it scaled into the child window. The back-buffer and viewport use the window's client size; the simulation uses the screen bounds.
  - In the preview, rock outlines are kept about 1 px wide, `SetWindowPos(HWND_TOPMOST)` is skipped for the child window, and the FPS counter is off.
- Results:
  - Test host (a 152x112 WinForms panel): fish animate and the 10 rocks are visible. The process exits when the host window closes.
  - Real Screen Saver Settings dialog (`Shoal.scr /p 9242470`): the preview plays. The Settings... button opens Shoal Settings (`/c:<hwnd>`), and closing it restarts the preview.
  - Full-screen regression run (`/debuglog:8 /rocks:10`): 0 stalls, 71.8 / 60.1 fps, 0 hitches.

## Control Panel preview: bigger fish, no rocks

- The `/p` preview now simulates an area 3x the preview's client size (about 456x336 for the 152x112 monitor image) instead of the full screen. Fish draw about 7 px long instead of 1–2 px, and at the saved settings there are about 40 fish.
- Rocks are always off in `/p`, whatever RockCount is set to. The dialog's Preview button (`/s`) is unchanged and shows rocks as configured.
- Verified in a 152x112 test host (fish shoal and no rocks; the process exits when the host closes) and in the real Screen Saver Settings dialog after reinstalling to System32.

## Wall lane softened

Groups no longer run along a fixed line about 50 px from the edge (the peak fell from 3.3x an even spread to a flat 1.5–1.9x). There were no stalls, clips or rock touches across sizes, speeds and 10000 fish. Details: [wall-lane.md](wall-lane.md).

## High fish count performance check (after wall-lane change)

Uncapped (`/vsync:0 /fpsmode:5 /rocks:10`), 6 s each.

| Fish | Per-fish log off | Per-fish log on |
|---|---|---|
| 2500 | 1201 fps, 1 hitch | 1052 fps, 79 hitches |
| 5000 | 715 fps, 0 hitches | 530 fps, 78 hitches (max 28 ms) |
| 10000 | 377 fps, 0 hitches | 180 fps, 77 hitches (max 54 ms) |

- With VSync on and the log off, 5000 and 10000 fish run at 72.0 / 60.0 fps with 0 hitches (max 14.4 / 17.0 ms).
- Without the log, frame rates are unchanged from before the wall change (381 / 718 / 1204 fps).
- With VSync on and the log on, 10000 fish drop to 45 / 39 fps.
- Where the log's time goes at 10000 fish, from temporary timing code: the blocking GPU readback (CopyResource + Map) takes 4.8 ms per sample, at 20 samples/s. Building and formatting rows takes 20 ms per sample (fish within 80 px of an obstacle), and 48 ms per full snapshot (every 0.5 s, all fish).
- Normal screensaver runs never enable this log.


## Fish tail added, shape setting removed

- Each fish is now an oval body (0.8L × 0.25L) plus a small oval tail (0.26L × 0.14L) behind it. The tail sweeps opposite to the body sway and opposite to turns, and its beat and sweep scale with speed and size. `GpuBoid` gained `yawRate` and `swimPhase`, advanced in the simulation shader. Fish are drawn with 12 vertices.
- The Boid Shape (Oval/Rectangle) setting and its registry value were removed; `Shape` is deleted on the next save. The settings dialog closed the gap (500×590) and its actual-size preview is now animated.
- Checks: /W4 clean; shaders compile at runtime; full-screen, settings dialog (size 2% and 10%) and `/p` preview checked visually.
- `/debuglog:8 /rocks:10` (defaults, 1012 fish): 0 stalls, 0 wall clips, 0 rock touches; 71.9 / 60.1 fps with VSync.
- `/boids:10000 /vsync:0 /fpsmode:5 /noboidlog`: 378.6 / 378.8 fps (377 before), so the tail costs nothing measurable.

## Thinner tail and coasting

- Tail half-width reduced from 0.07L to 0.04L (now 0.26L × 0.08L).
- New per-fish `thrust` (`GpuBoid` field), smoothed from relative acceleration in `finishSwim`. It is about 1 at steady speed, up to 1.6 when accelerating, and down to 0.03 when decelerating. It scales the beat rate, tail sweep and body sway. The rudder (turn) term is unaffected.
- `/debuglog:8 /rocks:10` (defaults): 0 stalls, 0 wall clips, 0 rock touches; 72.0 / 60.1 fps with VSync.
- `/boids:10000 /vsync:0 /fpsmode:5 /noboidlog`: 361 fps (379 before; the boid struct grew from 60 to 64 bytes).

## Tail Size slider

- New **Tail Size (% of body)** slider, 10–100, default 33 (`TailPercent`, `/tail:N`). The tail is the body oval scaled by the ratio, overlapping it by 20% of the tail length. Body length is `L / (1 + 0.8r)`, so the fish always spans L. At 33% the shape matches the previous fixed tail. Dialog is 500×630, and the preview and its size caption follow the slider.
- Checks: /W4 clean; shaders compile; screenshots at 10%, 33% and 100% at size 8%; dialog at 70%.
- `/debuglog:8 /rocks:10 /tail:100 /size:100`: 0 stalls, 0 wall clips, 0 rock touches.

## Fish colours

- Settings dialog: **Fish Colours** swatches (1–10; click to change via `ChooseColor`, right-click to remove, **Add...**) and **Tail Shade** slider (−100 to 100, default 40% darker). Registry `ColourCount`, `Colour0`–`9` and `TailShade` (+100 offset); `/shade:N` override. Default palette: silver, yellow, orange, blue, teal.
- Render constants grew to 208 bytes (`colours[10]`, `colourCount`, `tailShade`). The VS outputs the head-to-tail position and the fish's palette colour; the PS shades it. The dialog preview now rasterizes fish into a DIB with the same shape and gradient, one per colour.
- Checks: /W4 clean; screenshots at shade −40 and +60 with 20 large fish; dialog swatches, right-click removal, Add opens the Color dialog.
- `/debuglog:8 /rocks:10`: 0 stalls, 0 wall clips, 0 rock touches. `/boids:10000 /vsync:0 /fpsmode:5 /noboidlog`: 360 fps (unchanged).

## Colour bias and colour avoidance

- **Colour Bias** slider (0–10, setting `ColourBias`, `/colourbias:N`) and **Different colours actively avoid each other** checkbox (`ColourAvoid`, `/colouravoid:0|1`). Both are greyed out with one colour (checked by removing swatches down to one).
- `tools/analyze_grouping.py` gained `samecol`: the share of neighbor pairs within 90 px that have the same colour (random mixing ≈ 1/colours).
- 300 fish, 5 colours, 20 s runs (metrics after 10 s):

| Mode | samecol | diff-colour fish within 40 px | median nearest diff-colour | grouped | align |
|---|---|---|---|---|---|
| Bias 0 | 0.20 | 71% | 25 px | 0.80 | 0.90 |
| Bias 10 (ignore) | 0.72 | 18% | 97 px | 0.49 | 0.82 |
| Bias 10 + avoid | 0.48 | 7% | 65 px | 0.51 | 0.87 |

- Bias 0 matches random mixing (0.20 = 1/5), so default behaviour is unchanged. Ignore lets schools sort by colour and drift apart. Avoid holds other colours outside the look-ahead buffer, giving the fewest close encounters.
- Rejected variants:
  - Feeding the colour push into the wall rebound left 94% of fish permanently broken away.
  - Applying it as a velocity force (×0.02–0.1 speed) gave no more avoidance than ignore (14–16% within 40 px).
- Regression (`/rocks:10`, bias 0 / 10 / 10+avoid / 5+avoid, saved dense settings of 1138 fish; also `/size:60`, `/restrict:1`): 0 stalls in every run, broken-away share normal (21–26%).
- 10000 fish, unlimited fps: bias 0 357 fps (unchanged), bias 10 346 fps, bias 10 + avoid 304 fps.

## Colour Bias slider replaced by Sort by colour checkbox

- In-between bias values made little visible difference, so the slider became a **Sort fish by colour** checkbox (`ColourSort`, `/coloursort:0|1`), equivalent to the old bias 0 or 10. **Different colours actively avoid each other** is unchanged but now needs sorting on (greyed out otherwise, and ignored by the simulation). A saved `ColourBias` above 0 migrates to sorting on.
- Dialog: sort was ticked from the migrated setting; clicking it disabled and re-enabled the avoid box; both stay greyed out with one colour.
- 300 fish, `/randomness:0`, the user's 2-colour palette (random mixing ≈ 0.5):

| Mode | samecol | grouped | align |
|---|---|---|---|
| Sort off | 0.50 | 0.77 | 0.96 |
| Sort on | 0.88 | 0.54 | 0.87 |
| Sort + avoid | 0.70 | 0.52 | 0.95 |
| Avoid only (ignored) | 0.49 | 0.70 | 0.96 |

- 0 stalls in every run, including `/rocks:10` with sort + avoid and the saved Randomness 100. /W4 clean.

## Size Variability

- New **Size Variability** slider (`SizeVariation` 100–300%, `/sizevar:N`) with a **Most fish near the set size** checkbox (`SizeBell`, `/sizebell:0|1`; greyed out at 100%). Each fish gets a fixed length `L × V^u`, stored per fish on the GPU; spacing, wall/rock avoidance, clamps, tail beat and drawing all use it. Neighbor and separation radii use the pair's average length.
- Fish count at 4.0% size, Fish Amount 100, 1 monitor: 654 at 100%, 503 at 300% bell (÷1.30), 324 at 300% even (÷2.02), matching the `E[s²]` area factor. The dialog count updates with the slider and checkbox.
- Generated lengths at 300%, 43 px set size: bell 17–120 px (median 44), even 14–127 px (median 45); at 10% size with even spread 36–299 px.

| Run | Fish | Stalls | Wall clip | Rock touch | Rock nose |
|---|---|---|---|---|---|
| 300% bell | 503 | 0 | 0.000 | 0.000 | 0.000 |
| 300% even | 324 | 0 | 0.000 | 0.000 | 0.000 |
| 300% even, 6 rocks | 315 | 0 | 0.000 | 0.000 | 0.000 |
| 300% even, 10% size, 4 rocks | 51 | 0 | 0.000 | 0.000 | 0.000 |
| 100% (unchanged) | 654 | 0 | 0.000 | 0.000 | 0.000 |

- `analyze_size.py` now uses each fish's own length from the new `length` log column.
- 10000 fish, unlimited fps: 353 fps at 100%, 356 fps at 300% even (no cost). /W4 clean.


## 3D Depth

- New **3D depth** checkbox (`DepthEffect`, `/depth:0|1`, default on). Each fish slowly rises and sinks; deeper fish are smaller and darker and pass underneath, and higher fish cast offset shadows onto fish below (depth buffer + stencil, multiply blend).
- Depth log column ranged 0.009-0.996 (mean 0.57) over a 15 s run; 0 stalls (simulation unchanged).
- 10000 fish, unlimited fps: 324.9 fps with depth, 326.6 fps without (within noise). /W4 clean.
- Screenshots confirmed layering changes over time and visible shadows on lower fish where fish cross.
- Changed: depth is now fixed per fish, ranked by length (smaller fish always below larger; equal lengths random). Log check at 300% even spread, 205 fish: depth constant over time for every fish, 0 size-order violations, 0 stalls. /W4 clean.

## Desktop Background

- New **Show the desktop behind the fish** checkbox (`DesktopBackground`, `/desktop:0|1`, default off). Each monitor's screen image is captured with GDI before the screensaver windows open, uploaded as a texture and drawn as a full-screen triangle at depth 0.999, so fish draw over it and (with 3D depth) their shadows fall on it.
- Verified by screenshot on the primary monitor (desktop visible, fish shadows on it); /W4 clean. Not used in the Control Panel preview.

## Settings dialog layout and presets

- Dialog reorganised: "Shoal: Settings" title, sliders on the left, colours and checkboxes on the right, full-width preview at the bottom (980x680). Verified by screenshot.
- Presets dropdown with save and delete icons (Segoe MDL2 Assets). Driven by test messages: typing a name enables Save; Save wrote `HKCU\Software\Shoal\Presets\Test Preset` (Speed 53 matched the slider) and enabled Delete; after moving the Speed slider to 150, selecting the preset restored 53; Delete asked for confirmation, removed the key and list entry. Test preset cleaned up, user settings untouched. /W4 clean.

## Size-dependent speed and agility

- Cruise speed × `r^-0.2` and speed-up/slow-down rates × `r^-0.5`, with `r = length / set length`. Neighbour speed matching removes each neighbour's size factor first.
- Test: `/sizevar:300 /sizebell:0 /speed:150 /speedvar:3 /coverage:40 /coloursort:0`, 219 fish, 5.5–47.5 px. Previous build vs new:

| Fish | Mean speed (old → new) | Speed-up p90 | Slow-down p90 (away from walls) |
|------|------------------------|--------------|---------------------------------|
| Small (<12 px) | 150 → 174 | 56 → 157 | 75 → 111 |
| Mid (12–24 px) | 148 → 146 | 67 → 67 | 75 → 73 |
| Large (≥24 px) | 150 → 128 | 57 → 52 | 74 → 53 |

- 0 stopped fish in both runs and in a `/speed:300 /rocks:6 /coverage:70 /sizevar:300` run (148,860 samples). 10k fish, V-Sync off: 349 fps, unchanged. /W4 clean.

## Cursor hidden

- Full-screen mode hides the cursor (`ShowCursor(FALSE)` plus `SetCursor(nullptr)` on `WM_SETCURSOR`) and restores it on exit. `GetCursorInfo` check: before flags=1, while running flags=0 / no cursor, after exit flags=1. /W4 clean.

## Random preset

- New checkbox (`RandomPreset`, `/randompreset:0|1`). Four `/randompreset:1` runs with the two saved presets alternated (Medium, White and Varied Sizes / Small Red and Blue / ... ), each run's settings line matching the preset; `/randompreset:0` used the main settings with no `# preset` line. Test `LastPreset` value removed afterwards. Dialog checkbox verified by screenshot. /W4 clean.
