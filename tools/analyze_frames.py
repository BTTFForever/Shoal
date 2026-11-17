"""Summarize frame timing recorded by Shoal /debuglog runs.

Usage:
    python tools/analyze_frames.py [log ...] [--warmup 2]

With no log paths, analyzes the newest log in build/logs. Frame rows are '#F,...' lines written at
exit, one per presented frame per monitor window.

Per window it reports:
    fps          presented frames per second
    interval     time between this window's frames (p50 / p95 / p99 / max, ms)
    hitches      frames whose interval is more than 2x the median
    no-sim       frames presented without a simulation step (the image repeats; reads as stutter).
                 A step is shared by every window drawn in the same batch, so a frame counts as no-sim only
                 if no step ran (for any window) since that window's previous frame.
    sim error    |simulated delta - real time since the previous step| (p50 / p99 / max, ms);
                 large values mean motion speeds up and slows down from frame to frame
    sim/render/present  CPU cost per frame (p50 / p99 / max, ms)

When DXGI frame statistics are present (current builds), a second table shows each window's
display timing as reported by the swap chain:
    vblank Hz       refresh rate measured from that monitor's own vblank counter and timestamps
    frames/vblank   frames displayed per vblank (1.00 = exactly one new frame every refresh)
    repeats         vblanks that showed the previous frame again. With vsync or 1x+ this should be 0
                    (a repeat is a visible stutter); at 1/2x every other vblank repeats by design
    phase           vblank timestamp modulo the refresh period (ms); different values per window
                    show each monitor is synced to its own vblank
"""

import argparse
import bisect
import glob
import os
import sys
from collections import defaultdict


def newest_log():
    root = os.path.join(os.path.dirname(__file__), "..", "build", "logs")
    logs = sorted(glob.glob(os.path.join(root, "*.csv")), key=os.path.getmtime)
    if not logs:
        sys.exit("No logs found in build/logs")
    return logs[-1]


def pct(values, q):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def triple(values):
    return f"{pct(values, 0.5):6.2f} {pct(values, 0.99):6.2f} {max(values) if values else 0:7.2f}"


def analyze(path, warmup):
    settings = ""
    windows = []
    frames = []
    stats = defaultdict(list)
    with open(path) as f:
        for line in f:
            if line.startswith("# settings"):
                settings = line[2:].strip()
            elif line.startswith("# window "):
                windows.append(line[2:].strip())
            elif line.startswith("#F,"):
                fields = line[3:].split(",")
                w, end, interval, sim_dt, sim_cpu, render_cpu, present = fields[:7]
                frames.append((int(w), float(end), float(interval), float(sim_dt), float(sim_cpu),
                               float(render_cpu), float(present)))
                if len(fields) >= 11 and int(fields[7]) > 0:
                    stats[int(w)].append((float(end), int(fields[7]), int(fields[8]), int(fields[9]),
                                          float(fields[10])))
    print(f"\n{os.path.basename(path)}  {settings}")
    for line in windows:
        print(f"  {line}")
    if not frames:
        print("  no frame samples (run with /debuglog:N on the current build)")
        return
    start = warmup * 1000.0
    frames = [fr for fr in frames if fr[1] >= start]
    duration = (max(fr[1] for fr in frames) - start) / 1000.0

    sim_steps = [fr for fr in frames if fr[3] > 0]
    sim_error = [abs(b[3] - (b[1] - a[1])) for a, b in zip(sim_steps, sim_steps[1:])]
    print(f"  simulation steps/s {len(sim_steps) / duration:7.1f}   sim error p50/p99/max {triple(sim_error)} ms")

    by_window = defaultdict(list)
    for fr in frames:
        by_window[fr[0]].append(fr)
    print("  win    fps   interval p50    p95    p99     max  hitches  no-sim |  sim p50/p99/max      "
          "render p50/p99/max   present p50/p99/max")
    for w, rows in sorted(by_window.items()):
        intervals = [r[2] for r in rows if r[2] > 0]
        median = pct(intervals, 0.5)
        hitches = sum(1 for v in intervals if v > 2 * median)
        step_times = [fr[1] for fr in sim_steps]
        no_sim = 0
        for a, b in zip(rows, rows[1:]):
            lo = bisect.bisect_right(step_times, a[1])
            if lo >= len(step_times) or step_times[lo] > b[1]:
                no_sim += 1
        print(f"  {w:>3} {len(rows) / duration:6.1f}   {median:12.2f} {pct(intervals, 0.95):6.2f} "
              f"{pct(intervals, 0.99):6.2f} {max(intervals):7.2f} {hitches:8d} {no_sim:7d} | "
              f"{triple([r[4] for r in rows])}  {triple([r[5] for r in rows])}  {triple([r[6] for r in rows])}")

    if stats:
        print("  win  vblank Hz  frames/vblank  repeats  phase ms")
        for w, rows in sorted(stats.items()):
            rows = [r for r in rows if r[0] >= start]
            # Collapse repeats: statistics only change when a new frame reaches the screen.
            shown = []
            for r in rows:
                if not shown or r[1] != shown[-1][1]:
                    shown.append(r)
            if len(shown) < 2:
                print(f"  {w:>3}  no frame statistics")
                continue
            first, last = shown[0], shown[-1]
            span_ms = last[4] - first[4]
            hz = (last[3] - first[3]) / (span_ms / 1000.0) if span_ms > 0 else 0.0
            refreshes = last[2] - first[2]
            per_vblank = (last[1] - first[1]) / refreshes if refreshes else 0.0
            missed = sum(max(0, (b[2] - a[2]) - (b[1] - a[1])) for a, b in zip(shown, shown[1:]))
            phase = (last[4] % (1000.0 / hz)) if hz else 0.0
            print(f"  {w:>3}  {hz:9.3f}  {per_vblank:13.2f}  {missed:7d}  {phase:8.2f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="*")
    parser.add_argument("--warmup", type=float, default=2.0, help="seconds to skip at start-up")
    args = parser.parse_args()
    for path in args.logs or [newest_log()]:
        analyze(path, args.warmup)


if __name__ == "__main__":
    main()
