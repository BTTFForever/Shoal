#!/usr/bin/env python3
"""Convert the shared desktop GLSL 4.30 shaders (ShoalCore/shaders) to GLSL ES 3.10 for Android.

GLSL ES has no implicit int->uint or int/uint->float conversions, and the Mali driver on the projector has no
GL_EXT_shader_implicit_conversions, so the literals and comparisons that relied on them are made explicit here.
The desktop files stay the single source of truth; every rewrite below is semantics-preserving.

Usage: es_shaders.py <src dir> <out dir>
"""
import pathlib
import re
import sys

HEADER = "#version 310 es\nprecision highp float;\nprecision highp int;\n"

RULES = [
    # uint i = 0  ->  uint i = 0u   (also inside for-initialisers)
    (r"\buint (\w+) = 0(?=[,;])", r"uint \1 = 0u"),
    # float x = 0;  ->  0.0
    (r"\bfloat (\w+) = 0;", r"float \1 = 0.0;"),
    # float comparisons with an int literal 0 (chaos > 0, turn < 0, neighbors > 0, monitorCount > 0)
    (r"(\w) ([<>]) 0(?![.\dxu])", r"\1 \2 0.0"),
    # uint(...) - 1  ->  - 1u
    (r"(uint\([^()]*\)) - 1\b(?![.u])", r"\1 - 1u"),
    # k + 1 / comfortStep + 1 on uints
    (r"\b([ijkq]|comfortStep) \+ 1\b(?![.u])", r"\1 + 1u"),
    # points[i * 2], points[i * 2 + 1]
    (r"\[(\w) \* 2\]", r"[\1 * 2u]"),
    (r"\[(\w) \* 2 \+ 1\]", r"[\1 * 2u + 1u]"),
]

# uint loop variable compared with a float uniform: "j < boidCount" -> "j < uint(boidCount)"
COMPARE = re.compile(r"\b(\w+) (<|>=) (boidCount|rockCount|monitorCount)\b")


# Mali-G31 allows no SSBOs in vertex shaders, so fish.vert reads the boid buffer as per-instance vertex attributes
# (the Java side binds the same GPU buffer as an array buffer, 72-byte stride) and the single monitor comes from
# the Render block's bounds.
BOID_ATTRIBS = """layout(location = 0) in vec4 aBoid0; // position, velocity
layout(location = 1) in vec4 aBoid1; // turnRate, turnTarget, independentUntil, nextTurnChange
layout(location = 2) in vec4 aBoid2; // heading, currentSpeed, stopped, breakawayHeading
layout(location = 3) in vec4 aBoid3; // cruiseSpeed, yawRate, swimPhase, thrust
layout(location = 4) in vec2 aBoid4; // fishLength, depth
"""
BOID_FROM_ATTRIBS = ("Boid(aBoid0.xy, aBoid0.zw, aBoid1.x, aBoid1.y, aBoid1.z, aBoid1.w, aBoid2.x, aBoid2.y, aBoid2.z, "
                     "aBoid2.w, aBoid3.x, aBoid3.y, aBoid3.z, aBoid3.w, aBoid4.x, aBoid4.y)")


def replace_once(text: str, old: str, new: str, name: str) -> str:
    if old not in text:
        sys.exit(f"es_shaders.py: '{old}' not found in {name}; update the converter")
    return text.replace(old, new)


# The projector's Mali-G31 is vertex-bound with 1000 fish, and its fish are only ~14 px long, so Android uses a lighter
# mesh (6 + 2 segments: 60 fps at 1000 fish with the spine chain, 2026-11-08). ShoalRenderer.FISH_VERTICES must match: (BODY + FIN) * 6 + 6.
ANDROID_SEGMENTS = {"BODY_SEGMENTS": 6, "FIN_SEGMENTS": 2}


# The spine chain (10 points per fish, written by sim.comp) also reaches fish.vert as per-instance attributes:
# ShoalRenderer binds the spine buffer to locations 5-9, two points (one vec4) each, 80-byte stride.
SPINE_ATTRIBS = "".join(f"layout(location = {5 + k}) in vec4 aSpine{k}; // spine points {2 * k}, {2 * k + 1}\n" for k in range(5))
SPINE_JOINT = ("vec2 joint(int k) { vec4 v = k < 2 ? aSpine0 : k < 4 ? aSpine1 : k < 6 ? aSpine2 : k < 8 ? aSpine3 : aSpine4; "
               "return (k & 1) == 0 ? v.xy : v.zw; }")


def convert_vertex_ssbos(text: str, name: str) -> str:
    text = replace_once(text, "layout(std430, binding = 3) readonly buffer SpineBuf { vec2 Spines[]; };\n", SPINE_ATTRIBS, name)
    text = replace_once(text, "vec2 joint(int k) { return Spines[spineBase + uint(k)]; }", SPINE_JOINT, name)
    text = replace_once(text, "layout(std430, binding = 0) readonly buffer BoidBuf { Boid Boids[]; };\n", BOID_ATTRIBS, name)
    text = replace_once(text, "layout(std430, binding = 1) readonly buffer MonBuf { Monitor Monitors[]; };\n", "", name)
    text = replace_once(text, "Boids[instanceId]", BOID_FROM_ATTRIBS, name)
    return replace_once(text, "Monitors[instanceId % uint(monitorCount)]", "Monitor(bounds.x, bounds.y, bounds.z, bounds.w)", name)


def convert(text: str, compute: bool, name: str = "") -> str:
    text = re.sub(r"^#version 430\s*\n", HEADER, text)
    if "buffer BoidBuf" in text and not compute:
        text = convert_vertex_ssbos(text, name)
        for const, n in ANDROID_SEGMENTS.items():
            text = re.sub(rf"const uint {const} = \d+u;", f"const uint {const} = {n}u;", text)
    # The Mali compiler rejects passing an element of a readonly buffer to a function ("discards 'readonly'").
    text = text.replace("readonly buffer RockBuf", "buffer RockBuf")
    # The literal fixes are for the compute shaders' uint counters and float uniforms; the render shaders are written
    # with explicit types (and their int counters must stay int).
    if compute:
        for pattern, repl in RULES:
            text = re.sub(pattern, repl, text)
    if compute:
        text = COMPARE.sub(lambda m: f"{m[1]} {m[2]} uint({m[3]})", text)
    return text


SKIP = {"rock.comp", "bg.vert", "bg.frag"}  # no rocks or background picture on Android (see ShoalRenderer)


def main() -> None:
    src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for f in sorted(src.iterdir()):
        if f.suffix not in (".comp", ".vert", ".frag") or f.name in SKIP:
            continue
        (out / f.name).write_text(convert(f.read_text(), f.suffix == ".comp", f.name))


if __name__ == "__main__":
    main()
