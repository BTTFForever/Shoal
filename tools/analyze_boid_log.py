"""Summarise Shoal boid debug logs and find episodes where boids stall.

Usage:
    python tools/analyze_boid_log.py [log.csv] [--threshold 0.2] [--min-duration 0.5] [--trace N]

With no log path, the newest build/logs/boid-debug-*.csv file is used.
"""

import argparse
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_SAMPLE_GAP = 0.25


def newest_log():
    logs = glob.glob(os.path.join(PROJECT_ROOT, "build", "logs", "boid-debug-*.csv"))
    if not logs:
        sys.exit("No logs found in build/logs. Run: build\\ShoalNative.exe /debuglog:60")
    return max(logs, key=os.path.getmtime)


def load(path):
    header = []
    with open(path, newline="") as handle:
        lines = []
        for line in handle:
            if line.startswith("#"):
                header.append(line[1:].strip())
            else:
                lines.append(line)
    rows = []
    for row in csv.DictReader(lines):
        for key, value in row.items():
            if key in ("boid", "monitor"):
                row[key] = int(value)
            elif key != "wall_side":
                row[key] = float(value)
        # With speed variability a fish may choose to swim slowly; a stall is being far below its own cruise speed.
        cruise = row.get("cruise_fraction")
        row["stall_fraction"] = row["speed_fraction"] / max(cruise, 0.01) if cruise is not None else row["speed_fraction"]
        rows.append(row)
    return header, rows


def obstacle(row):
    wall = row["wall_distance"]
    rock = row["rock_distance"]
    if rock >= 0 and (wall < 0 or rock < wall):
        return f"rock {rock:.1f}px"
    if wall >= 0:
        return f"wall {row['wall_side']} {wall:.1f}px"
    return "off-monitor"


def find_stalls(by_boid, threshold, min_duration):
    episodes = []
    for boid, samples in by_boid.items():
        current = []
        for row in samples + [None]:
            slow = row is not None and row["stall_fraction"] < threshold
            contiguous = row is not None and current and row["time_s"] - current[-1]["time_s"] <= MAX_SAMPLE_GAP
            if slow and (not current or contiguous):
                current.append(row)
                continue
            if current and current[-1]["time_s"] - current[0]["time_s"] >= min_duration:
                episodes.append((boid, current))
            current = [row] if slow else []
    return episodes


def describe(boid, episode, samples):
    start = episode[0]
    end = episode[-1]
    before = [r for r in samples if start["time_s"] - 0.5 <= r["time_s"] < start["time_s"]]
    approach = before[0] if before else start
    return (
        f"boid {boid:4d}  t={start['time_s']:7.2f}-{end['time_s']:7.2f}s  "
        f"duration={end['time_s'] - start['time_s']:5.2f}s  "
        f"min speed={min(r['speed_fraction'] for r in episode) * 100:5.1f}%  "
        f"start=({start['x']:.0f},{start['y']:.0f}) {obstacle(start)}  "
        f"approach facing_wall={approach['facing_wall']:+.2f}  "
        f"distance moved={((end['x'] - start['x']) ** 2 + (end['y'] - start['y']) ** 2) ** 0.5:.1f}px"
    )


def trace(boid, episode, samples):
    start = episode[0]["time_s"] - 1.0
    end = episode[-1]["time_s"] + 0.5
    print(f"\nTrace for boid {boid} ({start:.2f}s to {end:.2f}s)")
    print("  time_s        x        y  speed%  heading  side  wall_px  facing")
    for row in samples:
        if start <= row["time_s"] <= end:
            print(
                f"  {row['time_s']:6.2f} {row['x']:8.1f} {row['y']:8.1f} {row['speed_fraction'] * 100:6.1f} "
                f"{row['heading_deg']:8.1f} {row['wall_side']:>5} {row['wall_distance']:8.2f} {row['facing_wall']:+7.2f}"
            )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?")
    parser.add_argument("--threshold", type=float, default=0.2, help="speed fraction treated as stalled")
    parser.add_argument("--min-duration", type=float, default=0.5, help="seconds below threshold to report")
    parser.add_argument("--trace", type=int, default=2, help="number of longest episodes to trace in detail")
    args = parser.parse_args()

    path = args.log or newest_log()
    header, rows = load(path)
    if not rows:
        sys.exit(f"{path} contains no samples")

    by_boid = defaultdict(list)
    for row in rows:
        by_boid[row["boid"]].append(row)
    for samples in by_boid.values():
        samples.sort(key=lambda r: r["time_s"])

    print(f"Log: {path}")
    for line in header[1:]:
        if not line.startswith(("rows:", "F,", "frames:")):
            print(f"  {line}")
    print(f"Samples: {len(rows)} rows, {len(by_boid)} boids, {rows[-1]['time_s'] - rows[0]['time_s']:.1f}s simulated")

    episodes = find_stalls(by_boid, args.threshold, args.min_duration)
    episodes.sort(key=lambda item: item[1][-1]["time_s"] - item[1][0]["time_s"], reverse=True)
    print(f"\nStalls below {args.threshold * 100:.0f}% speed for at least {args.min_duration:.2f}s: {len(episodes)}")
    if not episodes:
        return

    durations = [e[-1]["time_s"] - e[0]["time_s"] for _, e in episodes]
    near_wall = sum(1 for _, e in episodes if 0 <= e[0]["wall_distance"] < 10)
    print(f"  duration median={statistics.median(durations):.2f}s max={max(durations):.2f}s")
    print(f"  started within 10px of a wall: {near_wall}/{len(episodes)}")
    print(f"  stalled boids: {len({boid for boid, _ in episodes})}")
    for boid, episode in episodes[:15]:
        print("  " + describe(boid, episode, by_boid[boid]))
    for boid, episode in episodes[: args.trace]:
        trace(boid, episode, by_boid[boid])


if __name__ == "__main__":
    main()
