"""Profile how far fish swim from blocking screen edges, to spot "lanes" along walls.

Usage:
    python tools/analyze_walls.py [log.csv] [--warmup 2]

Uses only full snapshots (snapshot=1), because per-frame rows are logged only near obstacles.
Each 10 px band shows the share of fish, the share an even spread over a 1920x1080 monitor would give,
their ratio (a sharp peak well above the neighbouring bands is a lane), and the share of fish swimming
roughly parallel to the wall (|facing_wall| < 0.35).
"""

import argparse
import collections

from analyze_boid_log import load, newest_log


def even_share(d0, d1, width=1920, height=1080):
    inner = lambda d: max(width - 2 * d, 0) * max(height - 2 * d, 0)
    return (inner(d0) - inner(d1)) / (width * height)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", nargs="?")
    parser.add_argument("--warmup", type=float, default=2.0)
    args = parser.parse_args()
    _, rows = load(args.log or newest_log())
    rows = [r for r in rows if r["snapshot"] == 1 and r["time_s"] > args.warmup]
    if not rows:
        raise SystemExit("No snapshot rows after the warm-up")

    total = len(rows)
    bins = collections.Counter()
    parallel = collections.Counter()
    for r in rows:
        d = r["wall_distance"]
        if 0 <= d < 200:
            band = int(d // 10) * 10
            bins[band] += 1
            if abs(r["facing_wall"]) < 0.35:
                parallel[band] += 1

    print(f"snapshot rows {total}")
    print(" band     fish%  even%  ratio  parallel")
    for band in range(0, 200, 10):
        n = bins[band]
        even = even_share(band, band + 10)
        print(f"{band:4d}-{band + 9:<4d} {n / total * 100:5.1f}  {even * 100:5.1f}  {n / total / even:5.2f}  "
              f"{parallel[band] / max(n, 1):.2f}  " + "#" * int(n / total * 200))
    near = sum(bins[b] for b in range(0, 80, 10)) / total
    print(f"within 80px: {near * 100:.1f}% (even spread {even_share(0, 80) * 100:.1f}%)")


if __name__ == "__main__":
    main()
