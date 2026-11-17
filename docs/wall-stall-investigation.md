# Wall stall investigation

## Symptom

Some fish reached a wall before they could finish turning. They then sat almost still against the wall for one to three seconds before swimming away.

## Measurements

Each measurement came from a 60 s `/debuglog` run with these settings:

- speed 81
- 503 boids
- size 20
- `RestrictToMonitor=1`
- two 1920×1080 monitors

Logs:

- Before: `build/logs/boid-debug-20261013-171307.csv`
- After: `build/logs/boid-debug-20261013-171644.csv`

| Metric | Before | After |
|--------|--------|-------|
| Stalls (< 20% speed for ≥ 0.5 s) | 85 (77 boids) | 0 |
| Longest stall | 2.83 s | — |
| Minimum speed fraction | 0.01 | 1.00 |
| Samples within 2 px of a wall | 472 | 1 |
| Mean speed fraction within 10 px of a wall | 0.45 | 1.00 |

Every stall began within 10 px of a wall.

Example trace for boid 262:

1. It entered a corner at full speed while turning.
2. It hit the bottom wall at 80% speed.
3. The hard clamp snapped it to face away from the wall at 1% speed.
4. It crawled about 0.2 px per sample.
5. The wall normal flipped between the right and bottom walls, and the boid oscillated.
6. It moved only 3.6 px in 2.8 s.

## Root causes (GPU compute shader)

1. **Braking ignored direction.** The test `distance < stoppingDistance + 4` is always true within 4 px of a wall. Fish that already faced away were still braked to the 1% speed floor.
2. **The hard clamp reset speed to 1%.** It also snapped the heading to the wall normal.
3. **Turning started late.** Turning began only at 40 px. In corners, the single nearest-wall normal flipped between walls.
4. **Flocking dominated at crawl speed.** Heading is rebuilt from velocity, and flocking forces are absolute. At 1% speed, they kept pulling fish back into the wall.

## Fix

- All blocking walls within the look-ahead distance contribute a weighted outward normal (`(1 - d/lookahead)² · 2`). Corners steer diagonally out.
- Fish turn toward the wall tangent plus an outward bias. The turn rate rises from 2 to 12 rad/s as the push grows.
- Fish brake only while closing on the nearest wall. The speed floor is 30% of the configured speed.
- The hard clamp only removes the into-wall component of the heading and keeps the current speed, so fish slide along the wall.
- The rock shader gained the same direction-aware braking and 30% floor. Its penetration push-out also no longer resets speed.
- The unused `stopped` flag is always cleared.

The look-ahead was originally `max(48, speed * 0.75)`. The speed sweep in [test-results.md](test-results.md) showed this was 750 px at speed 1000, which squeezed 92% of boids into the central quarter of each monitor. It is now `clamp(speed * 0.3, 48, 20% of the monitor's shorter side)`.

The CPU fallback renormalizes speed every step, so it never showed this stall and is unchanged.
