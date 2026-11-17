# Wall lane ("invisible wall")

## Symptom

Groups ran alongside a monitor edge, parallel to it, but not close to it, as if along an invisible wall.

## Measurement

`tools/analyze_walls.py` builds a histogram of distance to the nearest blocking wall from the full snapshots (every fish, every 0.25 s). It compares each 10 px band with an even spread over a 1920x1080 monitor. Run: `/debuglog:20 /rocks:0` at the saved defaults (about 1010 fish, size 2%, speed 104).

| Version | Peak band | Peak vs. even | Parallel share at peak | Within 80 px of a wall |
|---|---|---|---|---|
| Before | 50–59 px | 3.29x | 0.83 | 23.8% |
| Heading-aware reach + per-fish comfort only | 10–19 px | 6.43x | 0.98 | 51.3% |
| + wall rebound (shipped) | 30–39 px | 1.90x (flat 1.5–1.9x from 20 to 110 px) | 0.59 | 28.4% |

## Cause

The look-ahead was about 50 px. Every fish inside it got an outward push and an outward steering bias, even when already swimming parallel. Once it left the zone, alignment with its neighbours turned it parallel again, and random wander drifted it back in. The zone edge therefore acted like a reflecting boundary that fish piled up along.

Only softening the push (row 2) moved the equilibrium closer to the wall and made the lane sharper. Any balance between a gentle push and the flock produces a lane at some distance.

## Fix

1. **Heading-aware reach:** `reach = lookahead * (0.5 + 0.5 * approach)`, where `approach = saturate(-dot(dir, wallNormal))`. Fish heading straight at a wall start turning at the full distance, while fish swimming parallel feel it only within half of it.
2. **Per-fish comfort:** each fish's look-ahead is scaled by a factor of 0.55–1.15. The factor is hashed per fish and blended smoothly between new values every 3–7 s, so no two fish share an edge.
3. **Wall rebound:** while a wall or rock push is active, a fish that is not already breaking away and either heads into the surface (`into > 0.2`) or randomly decides to leave (1.2 times per second) gets a breakaway heading. That heading points 20–60° out from the wall along its current direction of travel, for 0.6–1.4 s. Fish in a school share a heading, so they rebound together, front first, like a school turning.
4. The constant outward steering bias was reduced from 0.25 to 0.15.

The hard clamp, braking and the 30% speed floor are unchanged.

## Regression

All runs had 0 stalls, 0 wall clips, 0 rock touches and 0 rock noses.

| Run | Fish size | Mean speed |
|---|---|---|
| Defaults, 10 rocks | 22 px | 1.08 |
| `/size:1` | 11 px | 1.07 |
| `/size:10` | 108 px | 1.09 |
| `/speed:1000` | 22 px | 0.73 |
| `/speed:1` | 22 px | 3.95 (speed floor) |
| `/boids:10000` | 22 px | 1.06 |

With `/restrict:0` there were 411 monitor crossings in 12 s.
