# Frame timing

## Stutter: cause

The original loop was driven by `WM_TIMER` with `SetTimer(1 ms)`.

- **Frame rate:** Windows timers have about 15.6 ms resolution, so the loop actually ran at about 60 fps whatever the FrameRate setting.
- **Uneven steps:** The simulation delta came from `GetTickCount`, which also has about 15.6 ms granularity.
  - Steps were 15 or 16 ms.
  - About 7% of steps were doubled to 31 or 32 ms.
  - This made about 5 visible jerks per second.
- **Sway:** The swim sway clock also used `GetTickCount`.

## Fix

`RunFrameLoop` replaces the timer:

- **Pacing:** A `PeekMessage` loop runs to a `QueryPerformanceCounter` deadline. It waits on a high-resolution waitable timer (falling back to `timeBeginPeriod(1)`), wakes 0.5 ms early, then spins.
- **Simulation:** Each frame performs one simulation step with the real QPC elapsed time, clamped to 0.5–100 ms, then renders all monitor windows.
- **Sway:** The swim sway uses QPC time.
- **Flocking:** Cohesion, alignment and separation were per-step nudges tuned for 60 steps per second. They are now scaled by `delta * 60`, so flocks keep the same tightness at 240 fps. Without this, flocking was about 4× stronger at 240 fps (at speed 1000, mean neighbors were 110 instead of about 15–19).

## Results

These runs used 503 boids on two 60 Hz monitors, with `/noboidlog`.

| Cap | Measured fps | Interval p50 / p99 / max | Sim-step error max |
|----:|-------------:|-------------------------:|-------------------:|
| Before (`WM_TIMER`) | about 60 | 15–32 ms steps | 15.9 ms |
| 60 | 60 | 16.66 ms | 0.26 ms |
| 240 | 240.1 | 4.17 / 4.34 / 4.52 ms | 0.26 ms |
| 240, 10 rocks | 240.1 | 4.17 / 4.34 / 4.52 ms | 0.26 ms |
| Uncapped | about 2,886 (2,670 with 10 rocks) | 0.37 / 0.60 / 12 ms | 0.61 ms |

- **Main cost:** Present is the main per-frame cost.
- **Simulation cost:** The simulation costs well under 0.1 ms on the CPU side.
- **Occasional spikes:** A few 8–13 ms Present spikes appear only at uncapped frame rates.

## V-Sync and per-monitor pacing

The single global frame deadline was replaced with per-window pacing.

- **VSync on (the default):**
  - Each swap chain has `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` with a maximum frame latency of 1.
  - Its waitable object signals at that monitor's own vblank. The loop waits on all of them (plus window messages) and presents the due window with `Present(1, 0)`.
  - Monitors with different refresh rates never wait on each other.
- **VSync off:**
  - Each window has its own deadline at `refresh × multiplier` (½×, 1×, 2×, 3×, 4×, or unlimited).
  - It uses `Present(0, DXGI_PRESENT_ALLOW_TEARING)`, so tearing is possible.
  - The refresh rate comes from `QueryDisplayConfig` as an exact rational. `EnumDisplaySettings` rounds 71.925 Hz down to 71, which made 1× pacing repeat a frame 9 times in 10 s.
- **Shared simulation step:**
  - When any windows are due, one simulation step runs with the real elapsed time, then those windows are drawn.
  - If the previous step was less than 0.1 ms ago, for example two monitors whose vblanks almost coincide, the windows are drawn without a new step, so no artificial minimum delta speeds up time.

Verification uses DXGI frame statistics (`GetFrameStatistics`), logged per frame and summarized by `tools/analyze_frames.py`.

### Mixed refresh rates (monitor 0 at 71.925 Hz, monitor 1 at 60 Hz, 10 rocks)

| Mode | Monitor | FPS | Measured vblank | Frames per vblank | Repeats | Interval p99 / max |
|------|--------:|----:|----------------:|------------------:|--------:|-------------------:|
| VSync | 0 | 72.0 | 71.926 Hz | 1.00 | 0 | 14.15 / 14.24 ms |
| VSync | 1 | 60.0 | 60.000 Hz | 1.00 | 0 | 16.80 / 16.91 ms |
| Off, 1× | 0 | 72.0 | 71.925 Hz | 1.00 | 0 | 14.14 / 14.55 ms |
| Off, 1× | 1 | 60.0 | 60.000 Hz | 1.00 | 0 | 16.86 / 17.02 ms |
| Off, 2× | 0 | 143.9 | 71.926 Hz | 2.00 | 0 | 7.22 / 7.60 ms |
| Off, 2× | 1 | 120.1 | 60.000 Hz | 2.00 | 0 | 8.59 / 8.90 ms |
| Off, unlimited | 0 / 1 | about 2,680 | — | 37 / 45 | 0 | Occasional 16 ms Present spike |

With VSync on, each monitor showed exactly one new frame per vblank at its own rate, and each had a different vblank phase. On two 60 Hz monitors, the ½×, 1×, 2× and 4× modes measured 30, 60, 120 and 240 fps (0.50, 1.00, 2.00 and 4.00 frames per vblank).
