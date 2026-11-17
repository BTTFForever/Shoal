# Speed variability

Real fish don't swim at a constant speed. The **Speed Variability** slider (0–10, registry `SpeedVariability`, override `/speedvar:N`) lets each fish speed up and slow down on its own. **Speed** becomes the weight: the speed most fish cruise at.

- **0:** every fish always swims at exactly Speed. There is no variation and no lone-fish sprint. Wall braking still applies.
- **10:** fish can range over the full 1–1000 px/s, but most stay near Speed.

## How it works

The logic is in the main compute shader.

1. **Moods.** Every 1.5–5 s (a different period and phase per fish), a fish picks a new mood:
   - **Jitter (about 70%):** `Speed × (1 ± 0.12 × v)`, where `v = variability / 10`.
   - **Excursion (30%, up to 80% when alone):** `mood = ±(0.1 + 0.9 w²)`, with `w` uniform, so most excursions are modest. The target speed is spread geometrically:
     - faster: `Speed × (1000 / Speed)^(mood·v)`
     - slower: `Speed × Speed^(mood·v)`, which reaches 1 px/s at `mood = −1`, `v = 1`
2. **Lonely fish lean fast.** `seekWeight` is `1 − neighbors/4` (scaled by `1 − randomness/100`).
   - It raises the excursion chance by `0.5 × seekWeight`.
   - It raises the chance an excursion is fast from 50% to `50% + 40% × seekWeight`.
   - It shrinks the excursion size (`w² × (1 − 0.7 seekWeight)`), so a lone fish sprints at up to about 2.5× instead of 12×. Faster lone fish overshot the fish they were seeking and never grouped.
3. **Schools share speed.** A fish's cruise speed blends `min(neighbors/4, 1) × 75%` toward the geometric mean of its neighbors' *cruise* speeds. Schools surge and slow together, while individuals still drift within them.
   - Using neighbors' *current* speeds made a fish braking at a wall drag its whole school down.
4. **Slow near obstacles.** The cruise speed is multiplied by `1 − 0.4 v × urgency` near walls and rocks, where `urgency` is the wall/rock push strength (0–1).
5. **Easing.**
   - Fish accelerate at `max(Speed, cruise) × 1.5` px/s².
   - They decelerate at `max(Speed × 0.5, (current − cruise) × 1.5)` px/s², an exponential approach with a time constant of about 0.7 s, so they come down from a burst quickly at any speed ratio.
   - Seek steering scales with the fish's actual speed, so fast fish still turn toward their target.
6. **Wall safety.**
   - Wall and rock look-ahead and braking use `max(Speed, current speed)`, so a fish bursting to 1000 px/s sees walls as early as one set to Speed 1000.
   - The braking floor is 30% of the fish's cruise speed. When it was 30% of Speed, fish at Speed 1 stalled at walls.

The debug log's `cruise_fraction` column records each fish's intended speed. `analyze_boid_log.py` and `analyze_size.py` count stalls and slow rows relative to it, so a fish choosing to swim slowly is not reported as stuck.

## Results (2026-10-13)

30 s runs, 10 rocks, size 1.9%, restrict off, vsync. Speed is shown as a fraction of the Speed setting (snapshot rows after 3 s).

| Speed | Fish | Var | p5 | p50 | p95 | Max | Within 0.8–1.25× | Grouped | Align | Stalls / clips / rock hits |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 81 | 503 | 0 | 1.00 | 1.00 | 1.00 | 1.00 | 100% | 96% | 0.95 | 0 / 0 / 0 |
| 81 | 503 | 5 | 0.82 | 1.00 | 1.32 | 2.24 | 89% | 96% | 0.96 | 0 / 0 / 0 |
| 81 | 503 | 10 | 0.78 | 1.05 | 2.03 | 4.87 | 71% | 92% | 0.96 | 0 / 0 / 0 |
| 81 | 10 | 10 | 0.54 | 1.30 | 2.37 | 3.01 | 32% | n/a | 0.98 | 0 / 0 / 0 |
| 1000 | 503 | 10 | 0.48 | 0.79 | 0.97 | 1.00 | 49% | 74% | 0.74 | 0 / 0 / 0 |
| 1 | 503 | 10 | 1.05 | 2.68 | 25.9 | 275 | 14% | 69% | 0.92 | 0 / 0 / 0 |
| 81, size 10% | 1000 | 0 | 1.00 | 1.00 | 1.00 | 1.00 | 99% | 91% | 0.75 | 0 / 0 / 0 |
| 81, size 10% | 1000 | 10 | 0.36 | 0.86 | 1.54 | 3.79 | 58% | 89% | 0.73 | 0 / 0 / 0 |
| 81, randomness 100 | 503 | 10 | 0.71 | 1.08 | 2.51 | 12.2 | 59% | 41% | 0.54 | 0 / 0 / 0 |

- At Speed 1000, fish can only go slower (1000 px/s is the maximum). At Speed 1, they can only go faster. So at the slider extremes, the speed spread is one-sided.
- Large crowded fish spend more time near walls, rocks and each other, so they cruise a little slower (median 0.86).
- Randomness 100 is unchanged (41% grouped, 86% of fish broken away).

10 fish at Speed 81, 60 s (largest group / number of groups):

| Var | 16 s | 32 s | 48 s | 59 s | Mean speed |
|---:|---|---|---|---|---:|
| 0 | 5/3 | 6/2 | 6/2 | 6/2 | 1.00 |
| 5 | 3/7 | 5/5 | 5/3 | 5/3 | 1.12 |
| 10 | 5/4 | 4/5 | 5/5 | 5/4 | 1.38 |

At high variability, lone fish still find each other (shoals of 4–5), but shoals split and re-form more often, which is the intended "more dynamic" look.

### Tuning history

- **First version.** Moods were cubed (`z³`) and the lone-fish bias shifted `z` toward +1. Schools blended toward their neighbors' arithmetic mean *current* speed, and the jitter was in log space. At variability 10 and Speed 81:
  - Fish swam too fast: the median was 1.52×, and only 24% of fish were within 0.8–1.25×.
  - Lone fish reached 12× and never grouped (0% grouped with 10 fish).
  - The braking floor was 30% of Speed, so at Speed 1 fish stalled at walls (40 stalls).
  - At Speed 1000 with log-space jitter, the median was 0.74×.
- **Final version.** The jitter is now linear, ±12% × v. Excursions are capped for lone fish, schools share the geometric mean of their neighbors' cruise speeds, deceleration is exponential, and the braking floor follows the cruise speed.
