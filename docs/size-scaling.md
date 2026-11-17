# Fish size scaling

## Size setting

Fish size is a percentage of the primary screen height, 1%–10% in 0.1% steps (`SizePercentTenths`, 10–100). The pixel length is `round(screenHeight × percent / 100)`, at least 4 px. On 1080p, 1% = 11 px, 1.9% = 21 px and 10% = 108 px. Fish are drawn `length` long and `length / 4` wide.

The settings dialog shows an animated, actual-size preview of the fish (body and tail) that updates as the slider changes.

## Size Variability

`SizeVariation` (100–300%) gives every fish its own fixed length, `set length × V^u` with `V = SizeVariation / 100` and `u` in −1…1 (at least 4 px). `SizeBell` picks the spread: bell curve (`u` = mean of three uniform values, most fish near the set size) or even (`u` uniform). The length is stored per fish in the GPU boid buffer (`fishLength`), and all rules below use it. The dialog preview shows fish from the smallest to the largest size and the caption gives the pixel range.

Fish Amount keeps the covered area: the count is divided by `E[s²]`, the mean squared length ratio. Even spread: `sinh(2 ln V) / (2 ln V)`; bell: `(sinh(2 ln V / 3) / (2 ln V / 3))³`. At 300% these are 2.02 and 1.31.

Older versions stored `Size` in pixels. On load it is converted to a percentage, and it is deleted on the next save.

## Why scaling was needed

The simulation treats each fish as a point, and spacing and avoidance were tuned for 20 px fish. With fixed spacing, large fish overlapped and their noses went into walls and rocks (see [test results](test-results.md#fish-size-2026-10-13)).

## What scales

With `k = length / 20` and `bodyOffset = (length − 20) / 2`, using each fish's own length:

| Behavior | Rule |
|---|---|
| Neighbor radius | `90 × k`, with `k` from the average length of the two fish |
| Separation radius | `46 × k`, with `k` from the average length of the two fish |
| Cohesion strength | `0.0012 / k` (the pull grows with distance, which already grows with `k`) |
| Separation strength | `0.12 × max(1, k)` |
| Wall and rock distance | reduced by `bodyOffset`, so avoidance measures from the body instead of the center (larger fish turn earlier, smaller fish can go closer) |
| Look-ahead minimum | walls `48 × max(1, √k)`, rocks `40 × max(1, √k)`; the speed-based upper limits are unchanged |
| Hard wall clamp | keeps the body extent `length × (0.5·|cos| + 0.125·|sin|)` inside blocking edges |
| Rock safety net | pushes out to the same body extent along the rock normal |
| Rock placement | buffer `40 + max(0, bodyOffset)` of the largest possible fish (set length × V), so passages stay wide enough for large fish |

At 20 px (`k = 1`) every rule except the body clamps reduces to the original behavior.
