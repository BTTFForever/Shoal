# Randomness setting

The Randomness slider (0–100) controls how often individual fish break away from the shoal.

- **0:** pure flocking.
- **Low values:** the occasional fish wanders off.
- **50:** about half the fish are out on their own at any moment.
- **100:** close to chaos, with almost no lasting groups.

## Behavior

Let `chaos = randomness / 100`.

- **Trigger:** A fish that is not already breaking away starts a breakaway at a rate of `chaos²` per second. That is never at 0, about once every 16 s at 25, once every 4 s at 50, and once per second at 100. The square keeps the low end of the slider gentle.
- **Duration:** A breakaway lasts `1.5 + 3.5·chaos + random(0–1.5)` seconds.
- **Direction:** The fish picks a heading 35–140° to the left or right of its current course. It turns toward that heading at up to 1.5 rad/s.
- **While breaking away:**
  - The fish ignores cohesion and alignment.
  - It still uses separation, the normal wander and wall/rock avoidance.
  - If a wall forces a turn, the breakaway heading is replaced with the new course so the fish doesn't fight the wall.
- **Rejoining:** When the breakaway ends, the fish rejoins whichever fish are within the 90 px neighbor radius.

The GPU shader uses a PCG integer hash for these random draws.

## Why it changed

The old trigger used a `frac(sin(seed · 12.9898) · 43758.5453)` hash with a sequential seed. On the GPU, this produced far too many values near zero. As a result, even randomness 1 kept 64% of fish permanently broken away, while randomness 100 still left half of them grouped. The `sin` hash is still used, unchanged, for the normal wander turns, so behavior at 0 is identical.

## Measurements

Each run used `build\ShoalNative.exe /debuglog:40 /randomness:N` with these settings:

- 503 boids
- speed 81
- size 20
- `RestrictToMonitor=1`
- two 1920×1080 monitors

Results come from `python tools\analyze_grouping.py`, averaged after a 10 s warm-up. Column meanings are documented in that script.

Before:

| Randomness | Isolated | Neighbors | Grouped | Largest | Align | Independent |
|-----------:|---------:|----------:|--------:|--------:|------:|------------:|
| 0   | 0.001 | 29.8 | 0.974 | 0.286 | 0.974 | 0.000 |
| 1   | 0.028 | 8.2  | 0.730 | 0.146 | 0.780 | 0.642 |
| 10  | 0.029 | 8.0  | 0.721 | 0.143 | 0.795 | 0.643 |
| 50  | 0.037 | 4.9  | 0.563 | 0.076 | 0.681 | 0.805 |
| 100 | 0.040 | 4.5  | 0.525 | 0.066 | 0.616 | 0.845 |

After:

| Randomness | Isolated | Neighbors | Grouped | Largest | Align | Independent |
|-----------:|---------:|----------:|--------:|--------:|------:|------------:|
| 0   | 0.002 | 26.6 | 0.973 | 0.263 | 0.971 | 0.000 |
| 10  | 0.011 | 20.4 | 0.934 | 0.182 | 0.961 | 0.024 |
| 25  | 0.029 | 10.9 | 0.796 | 0.174 | 0.889 | 0.162 |
| 50  | 0.039 | 5.5  | 0.601 | 0.073 | 0.719 | 0.502 |
| 75  | 0.042 | 4.0  | 0.457 | 0.043 | 0.591 | 0.737 |
| 100 | 0.046 | 3.6  | 0.392 | 0.035 | 0.540 | 0.855 |

For comparison, the same number of fish with uniformly random positions and headings gives these values:

| Isolated | Neighbors | Grouped | Largest | Align |
|---------:|----------:|--------:|--------:|------:|
| 0.055 | 3.0 | 0.283 | 0.025 | 0.466 |

Randomness 100 is close to that baseline, which is the intended "almost chaos".

## Tuning

- **The middle feels too chaotic:** raise the exponent in the trigger, for example `chaos³`.
- **Breakaways look too short or too long:** adjust the duration terms.
- **100 should be even more chaotic:** extend the duration at the top end, or scale cohesion down by `(1 - chaos)`.

Re-run the sweep after any change and compare the tables.
