#version 430
// Full-screen triangle for the background picture. uUv picks the part of the picture the view shows
// (xy = offset, zw = size, in texture coordinates with v = 0 at the picture's top row), so it covers the view
// without stretching.
uniform vec4 uUv;
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); // (0,0) (2,0) (0,2)
    vec2 s = p; // 0..1 across the screen (y up); the triangle reaches 2, past the screen's far edges
    vUv = uUv.xy + vec2(s.x, 1.0 - s.y) * uUv.zw;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
