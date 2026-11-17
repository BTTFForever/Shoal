#version 430
layout(std140, binding = 1) uniform Render { vec4 bounds; float baseLength; float monitorCount; float restrictToMonitor; float tailRatio; float colourCount; float tailShade; float depthEffect; float padding; vec4 colours[10]; };
uniform int uShadow;
in vec2 vShape;
in float vAlong;
flat in vec3 vColour;
flat in float vFork;
out vec4 outColour;
void main() {
    // Tail-fin end (vShape.x runs 0..1 along the fin, -1 on the body): a V notch for a forked tail, or the corners
    // cut back for a rounded one (negative fork). Same shape as fin_end() in tools/fish_gallery.py.
    float finEnd = vFork >= 0.0 ? 1.0 - vFork * (1.0 - abs(vShape.y)) : 1.0 + vFork * vShape.y * vShape.y;
    if (vShape.x > 0.0 && vShape.x > finEnd) discard;
    if (uShadow != 0) { outColour = vec4(0.2, 0.2, 0.2, 1.0); return; }
    float t = clamp(vAlong, 0.0, 1.0) * tailShade;
    vec3 c = t < 0.0 ? vColour * (1.0 + t) : mix(vColour, vec3(1.0), t);
    outColour = vec4(c, 1.0);
}
