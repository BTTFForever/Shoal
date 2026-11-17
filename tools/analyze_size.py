"""Check how fish size (Size setting = body length L) interacts with movement and avoidance.

Usage:
    python tools/analyze_size.py <log> [<log> ...] [--warmup 2]

Fish are drawn as an oval body plus a small oval tail, together L long and L/4 wide, centered on the boid position,
but the simulation treats each boid as a point. With Size Variability each fish has its own L (the log's length
column); 'size' is the set length. Using full snapshots (snapshot=1) after the warm-up,
it reports per log:
    nn/L        median nearest-neighbor center distance divided by L (below ~0.25 bodies overlap side by side)
    overlap     share of boids whose nearest neighbor is closer than L/4 (bodies visibly overlap)
    wall clip   share of boids whose nose or tail is past an outer (blocking) screen edge
    rock touch  share of boids whose center is within L/8 of a rock outline (body side touches the rock)
    rock nose   share of boids whose nose (center + L/2 along heading) is inside a rock, when rock outlines
                are in the log header ('# rock' lines); otherwise share with center within L/2 of a rock
    speed       mean speed fraction, and rows below 60% of the fish's own cruise speed
    stalls      stretches below 20% of the fish's own cruise speed for at least 0.5 s
                (cruise speed varies with Speed Variability; older logs use the Speed setting)
"""

import argparse
import csv
import math
from collections import defaultdict


def point_in_polygon(px, py, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        ax, ay = poly[i]
        bx, by = poly[j]
        if (ay > py) != (by > py) and px < (bx - ax) * (py - ay) / (by - ay) + ax:
            inside = not inside
        j = i
    return inside


def analyze(path, warmup):
    settings = {}
    monitors = []
    rocks = []
    with open(path) as f:
        header = [line for line in f if line.startswith("#") and not line.startswith("#F,")]
    for line in header:
        if line.startswith("# settings"):
            settings = dict(kv.split("=") for kv in line.split()[2:])
        elif line.startswith("# monitor"):
            parts = dict(kv.split("=") for kv in line.split()[3:])
            monitors.append(tuple(int(parts[k]) for k in ("left", "top", "right", "bottom")))
        elif line.startswith("# rock "):
            pts = line.split()[3:]
            rocks.append([tuple(float(v) for v in p.split(",")) for p in pts])
    length = float(settings.get("size", 20))

    def on_screen(x, y):
        return any(l <= x < r and t <= y < b for l, t, r, b in monitors)

    snapshots = defaultdict(list)
    slow_rows = 0
    speeds = []
    below = defaultdict(float)
    last_time = {}
    stalls = 0
    with open(path) as f:
        for row in csv.DictReader(line for line in f if not line.startswith("#")):
            t = float(row["time_s"])
            boid = int(row["boid"])
            actual = float(row["speed_fraction"])
            cruise = row.get("cruise_fraction")
            fraction = actual / max(float(cruise), 0.01) if cruise else actual
            if t >= warmup:
                speeds.append(actual)
                if fraction < 0.6:
                    slow_rows += 1
            if fraction < 0.2:
                below[boid] += t - last_time.get(boid, t)
            else:
                if below[boid] >= 0.5:
                    stalls += 1
                below[boid] = 0.0
            last_time[boid] = t
            if row["snapshot"] == "1" and t >= warmup:
                fish_length = float(row["length"]) if row.get("length") else length
                snapshots[t].append((float(row["x"]), float(row["y"]), math.radians(float(row["heading_deg"])),
                                     float(row["rock_distance"]), fish_length))
    stalls += sum(1 for v in below.values() if v >= 0.5)

    nn_ratio = []
    overlap = clip = touch = nose = total = 0
    for boids in snapshots.values():
        cell = max(max(b[4] for b in boids), 46.0)
        grid = defaultdict(list)
        for i, (x, y, _, _, _) in enumerate(boids):
            grid[(int(x // cell), int(y // cell))].append(i)
        for i, (x, y, h, rock_d, fl) in enumerate(boids):
            total += 1
            cx, cy = int(x // cell), int(y // cell)
            best = math.inf
            for gx in (cx - 1, cx, cx + 1):
                for gy in (cy - 1, cy, cy + 1):
                    for j in grid.get((gx, gy), ()):
                        if j != i:
                            best = min(best, math.hypot(boids[j][0] - x, boids[j][1] - y))
            if best < math.inf:
                nn_ratio.append(best / fl)
                if best < fl / 4:
                    overlap += 1
            dx, dy = math.cos(h) * fl / 2, math.sin(h) * fl / 2
            if not on_screen(x + dx, y + dy) or not on_screen(x - dx, y - dy):
                clip += 1
            if rock_d >= 0:
                if rock_d < fl / 8:
                    touch += 1
                if rocks:
                    if any(point_in_polygon(x + dx, y + dy, r) for r in rocks):
                        nose += 1
                elif rock_d < fl / 2:
                    nose += 1
    nn_ratio.sort()
    median = nn_ratio[len(nn_ratio) // 2] if nn_ratio else 0
    total = max(total, 1)
    return {
        "size": int(length),
        "speed": settings.get("speed", "?"),
        "nn/L": median,
        "overlap": overlap / total,
        "wall clip": clip / total,
        "rock touch": touch / total,
        "rock nose": nose / total,
        "rock outlines": bool(rocks),
        "mean speed": sum(speeds) / max(len(speeds), 1),
        "slow rows": slow_rows,
        "stalls": stalls,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--warmup", type=float, default=2.0)
    args = parser.parse_args()
    print(" size speed   nn/L  overlap  wall clip  rock touch  rock nose  mean speed  slow rows  stalls")
    estimated = False
    for path in args.logs:
        r = analyze(path, args.warmup)
        nose_label = "" if r["rock outlines"] else "*"
        estimated = estimated or not r["rock outlines"]
        print(f"{r['size']:5d} {r['speed']:>5} {r['nn/L']:6.2f} {r['overlap']:8.3f} {r['wall clip']:10.3f} "
              f"{r['rock touch']:11.3f} {r['rock nose']:9.3f}{nose_label:1} {r['mean speed']:11.2f} "
              f"{r['slow rows']:10d} {r['stalls']:7d}")
    if estimated:
        print("* no '# rock' outlines in the header: rock nose = center within L/2 of a rock (upper bound)")


if __name__ == "__main__":
    main()
