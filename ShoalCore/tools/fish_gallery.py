#!/usr/bin/env python3
"""Draws fish silhouettes from the shape parameters used by ShoalCore/shaders/fish.vert, for choosing species.

Usage: fish_gallery.py out.png
Each species is drawn straight (top) and mid-stroke while turning (bottom), with the same outline, swimming wave and
turning arc as the shader, so what you pick here is what swims on screen.
"""
import math
import pathlib
import re
import sys

from PIL import Image, ImageDraw, ImageFont

# The species table lives in ../shaders/fish.vert (const float SPECIES[]); it is read from there so the gallery always
# shows what swims on screen. Parameters are explained next to the table.
KEYS = ("width", "peak", "nose", "rear", "stalk", "tail", "fin", "flare", "fork", "pect", "tip")


def load_species():
    src = (pathlib.Path(__file__).resolve().parent.parent / "shaders" / "fish.vert").read_text()
    out = []
    for m in re.finditer(r"/\*\s*\d+\s+(\w+)\s*\*/([^\n]*)", src):
        values = [float(v) for v in re.findall(r"-?\d+\.?\d*", m.group(2))]
        out.append((m.group(1), dict(zip(KEYS, values))))
    if not out:
        sys.exit("fish_gallery.py: no species table found in fish.vert")
    return out


SPECIES = load_species()
COLOURS = [(220, 230, 240), (255, 205, 40), (255, 120, 30), (40, 110, 230), (40, 200, 190)]


def body_profile(u, p):
    """Half-width at u (0 nose .. 1 stalk) relative to the widest point."""
    if u < p["peak"]:
        t = u / p["peak"]
        pointed = 1 - (1 - t) ** p["nose"]
        rounded = math.sqrt(max(0.0, 1 - (1 - t) ** 2))  # an ellipse quarter: round tip
        return pointed + (rounded - pointed) * p["tip"]
    v = (u - p["peak"]) / (1 - p["peak"])
    return p["stalk"] + (1 - p["stalk"]) * math.cos(v * math.pi / 2) ** p["rear"]


def spine(x, length, amp, phase, curvature):
    """Centre-line point and forward direction x lengths behind the nose (nose at 0,0 facing +x), as fish.vert."""
    s = x * length
    cs = curvature * s
    back = s if abs(cs) < 1e-4 else math.sin(cs) / curvature
    across = 0.5 * cs * s if abs(cs) < 1e-4 else (1 - math.cos(cs)) / curvature
    px, py = -back, across
    fx, fy = math.cos(cs), -math.sin(cs)
    nx, ny = -fy, fx
    k = 2 * math.pi / 0.95
    a = amp * (0.02 - 0.0825 * x + 0.1625 * x * x)
    da = amp * (-0.0825 + 0.325 * x)
    arg = k * x - phase
    h = a * math.sin(arg) * length
    dh = da * math.sin(arg) + a * k * math.cos(arg)
    px, py = px + nx * h, py + ny * h
    fx, fy = fx - nx * dh, fy - ny * dh
    n = math.hypot(fx, fy)
    return px, py, fx / n, fy / n


def outline(p, length, amp, phase, curvature):
    """Polygon of the fish (body + forked or rounded tail fin) and the two pectoral fins."""
    body_len = length / (1 + p["tail"])
    body_end = body_len / length
    max_hw = p["width"] * body_len
    stalk_hw = max_hw * p["stalk"]
    fin_hw = max_hw * p["fin"]

    def at(x, side_offset):
        px, py, fx, fy = spine(x, length, amp, phase, curvature)
        return px + -fy * side_offset, py + fx * side_offset

    left, right = [], []
    for i in range(41):
        u = 0.5 - 0.5 * math.cos(i / 40 * math.pi)
        hw = max_hw * body_profile(u, p)
        left.append(at(u * body_end, -hw))
        right.append(at(u * body_end, hw))

    def fin_hw_at(u):
        return stalk_hw + (fin_hw - stalk_hw) * u ** p["flare"]

    def fin_end(v):  # how far along the fin its end reaches at side position v (-1..1)
        k = p["fork"]
        return 1 - k * (1 - abs(v)) if k >= 0 else 1 + k * v * v

    for i in range(1, 21):
        u = i / 20 * fin_end(-1)
        x = body_end + (1 - body_end) * u
        left.append(at(x, -fin_hw_at(u)))
        right.append(at(x, fin_hw_at(u)))
    end = []
    for i in range(41):
        v = -1 + i / 20
        u = fin_end(v)
        end.append(at(body_end + (1 - body_end) * u, v * fin_hw_at(u)))
    poly = left + end + right[::-1]

    fins = []
    root = 0.2
    root_hw = max_hw * body_profile(root / body_end, p)
    for s in (-1, 1):
        fins.append([at(root, s * root_hw * 0.85), at(root + 0.11, s * root_hw * 0.85),
                     at(root + 0.17, s * (root_hw + max_hw * p["pect"]))])
    return poly, fins


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "fish_gallery.png"
    scale = 3
    cw, ch, cols = 400, 360, 5
    rows = (len(SPECIES) + cols - 1) // cols
    img = Image.new("RGB", (cw * cols * scale, ch * rows * scale), (11, 34, 54))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 22 * scale)
        small = ImageFont.truetype("DejaVuSans.ttf", 13 * scale)
    except OSError:
        font = small = ImageFont.load_default()
    length = 250
    for n, (name, p) in enumerate(SPECIES):
        cx0, cy0 = (n % cols) * cw, (n // cols) * ch
        colour = COLOURS[n % len(COLOURS)]
        d.text(((cx0 + 14) * scale, (cy0 + 10) * scale), f"{n + 1}. {name}", fill=(235, 240, 245), font=font)
        d.text(((cx0 + 14) * scale, (cy0 + 40) * scale),
               f"width {p['width']:.2f}  tail {p['tail']:.2f}  fork {p['fork']:+.2f}", fill=(150, 170, 190), font=small)
        for row, (amp, phase, curv) in enumerate(((0.0, 0.0, 0.0), (1.0, 1.9, 0.45 / length))):
            poly, fins = outline(p, length, amp, phase, curv)
            ox = cx0 + (cw + length) / 2 - 5
            oy = cy0 + 120 + row * 150
            tf = lambda pt: ((ox + pt[0]) * scale, (oy + pt[1]) * scale)
            shade = tuple(int(c * 0.55) for c in colour)
            for f in fins:
                d.polygon([tf(q) for q in f], fill=shade)
            d.polygon([tf(q) for q in poly], fill=colour)
    img = img.resize((cw * cols, ch * rows), Image.LANCZOS)
    img.save(out)
    print(out)


if __name__ == "__main__":
    main()
