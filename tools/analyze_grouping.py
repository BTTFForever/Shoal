"""Measure how strongly boids group in Shoal debug logs.

Usage:
    python tools/analyze_grouping.py [log ...] [--warmup 10] [--link 60]

With no log paths, analyzes the newest log in build/logs. Only snapshot rows (snapshot=1, every boid
every 0.5 s) are used. Each line of output is one log, averaged over snapshots after the warm-up.

Metrics:
    isolated   fraction of boids with no other boid within 90 px (the flocking neighbor radius)
    neighbors  mean number of boids within 90 px
    grouped    fraction of boids in a cluster of 5+ (boids linked when closer than --link px)
    largest    largest cluster as a fraction of all boids
    clusters   number of clusters with 5+ boids
    align      mean local alignment (1 = neighbors swim the same way, ~0 = random headings)
    indep      fraction of boids currently broken away (independent=1)
    samecol    fraction of neighbor pairs (within 90 px) sharing a colour; random mixing gives about
               1/colours, 1.0 means fish only school with their own colour
"""

import argparse
import csv
import glob
import math
import os
import sys
from collections import defaultdict

NEIGHBOR_RADIUS = 90.0


def newest_log():
    root = os.path.join(os.path.dirname(__file__), "..", "build", "logs")
    logs = sorted(glob.glob(os.path.join(root, "*.csv")), key=os.path.getmtime)
    if not logs:
        sys.exit("No logs found in build/logs")
    return logs[-1]


def read_snapshots(path):
    settings = ""
    snapshots = defaultdict(list)
    with open(path, newline="") as f:
        lines = []
        for line in f:
            if line.startswith("# settings"):
                settings = line[2:].strip()
            elif not line.startswith("#"):
                lines.append(line)
    reader = csv.DictReader(lines)
    if "snapshot" not in (reader.fieldnames or []):
        sys.exit(f"{path} has no snapshot column; re-run with the current build")
    for row in reader:
        if row["snapshot"] == "1":
            heading = math.radians(float(row["heading_deg"]))
            snapshots[float(row["time_s"])].append(
                (float(row["x"]), float(row["y"]), math.cos(heading), math.sin(heading), row["independent"] == "1", int(row["boid"])))
    return settings, snapshots


def pairs_within(boids, radius):
    grid = defaultdict(list)
    for i, b in enumerate(boids):
        grid[(int(b[0] // radius), int(b[1] // radius))].append(i)
    r2 = radius * radius
    for (cx, cy), members in grid.items():
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for j in grid.get((cx + dx, cy + dy), ()):
                    for i in members:
                        if i < j:
                            ex = boids[i][0] - boids[j][0]
                            ey = boids[i][1] - boids[j][1]
                            if ex * ex + ey * ey < r2:
                                yield i, j


def fish_colour(index, colours):
    # Must match fishColour() in the simulation shader.
    return (((index * 2654435761) & 0xFFFFFFFF) >> 16) % max(colours, 1)


def measure(boids, link, colours):
    n = len(boids)
    neighbors = [[] for _ in range(n)]
    same = 0
    for i, j in pairs_within(boids, NEIGHBOR_RADIUS):
        neighbors[i].append(j)
        neighbors[j].append(i)
        same += fish_colour(boids[i][5], colours) == fish_colour(boids[j][5], colours)
    pair_count = sum(len(near) for near in neighbors) // 2

    parent = list(range(n))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for i, j in pairs_within(boids, link):
        parent[find(i)] = find(j)
    sizes = defaultdict(int)
    for i in range(n):
        sizes[find(i)] += 1
    groups = [s for s in sizes.values() if s >= 5]

    alignments = []
    for i, near in enumerate(neighbors):
        if near:
            sx = boids[i][2] + sum(boids[j][2] for j in near)
            sy = boids[i][3] + sum(boids[j][3] for j in near)
            alignments.append(math.hypot(sx, sy) / (len(near) + 1))

    return {
        "isolated": sum(1 for near in neighbors if not near) / n,
        "neighbors": sum(len(near) for near in neighbors) / n,
        "grouped": sum(groups) / n,
        "largest": max(sizes.values()) / n,
        "clusters": len(groups),
        "align": sum(alignments) / len(alignments) if alignments else 0.0,
        "indep": sum(1 for b in boids if b[4]) / n,
        "samecol": same / pair_count if pair_count else 0.0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="*")
    parser.add_argument("--warmup", type=float, default=10.0, help="seconds to skip while the initial scatter forms groups")
    parser.add_argument("--link", type=float, default=60.0, help="cluster link distance in px")
    args = parser.parse_args()

    keys = ["isolated", "neighbors", "grouped", "largest", "clusters", "align", "indep", "samecol"]
    print(f"{'randomness':>10} " + " ".join(f"{k:>9}" for k in keys) + "  log")
    for path in args.logs or [newest_log()]:
        settings, snapshots = read_snapshots(path)
        colours = int(next((p.split("=")[1] for p in settings.split() if p.startswith("colours=")), "1"))
        results = [measure(b, args.link, colours) for t, b in sorted(snapshots.items()) if t >= args.warmup and b]
        if not results:
            print(f"{'?':>10} no snapshots after warm-up  {os.path.basename(path)}")
            continue
        randomness = next((p.split("=")[1] for p in settings.split() if p.startswith("randomness=")), "?")
        means = {k: sum(r[k] for r in results) / len(results) for k in keys}
        print(f"{randomness:>10} " + " ".join(f"{means[k]:>9.3f}" for k in keys) + f"  {os.path.basename(path)}")


if __name__ == "__main__":
    main()
