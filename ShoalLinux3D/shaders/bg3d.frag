#version 430
layout(std140, binding = 1) uniform Render {
  vec4 win;
  vec4 box;
  float D, camDist, cx, cy;
  float tailRatio, tailShade, colourCount, time;
  vec4 colours[10];
};
uniform vec2 uViewport;
out vec4 outColour;

const vec3 FOG = vec3(0.012, 0.055, 0.095);

float caustic(vec2 uv, float t) {
  float a = sin(uv.x * 0.012 + t * 0.35 + sin(uv.y * 0.010 + t * 0.25) * 1.6);
  float b = sin(uv.y * 0.016 - t * 0.30 + sin(uv.x * 0.008 + t * 0.20) * 1.4);
  float l = 1.0 - abs(a * b);
  return pow(l, 7.0);
}

void main() {
  // Desktop pixel of this fragment, then the ray from the camera through it to the far side of the box.
  vec2 pix = vec2(win.x + gl_FragCoord.x / uViewport.x * (win.z - win.x), win.y + (1.0 - gl_FragCoord.y / uViewport.y) * (win.w - win.y));
  vec3 O = vec3(cx, cy, -camDist);
  vec3 d = vec3(pix.x - cx, pix.y - cy, camDist);
  float tx = d.x > 0.0 ? (box.z - O.x) / d.x : (d.x < 0.0 ? (box.x - O.x) / d.x : 1e9);
  float ty = d.y > 0.0 ? (box.w - O.y) / d.y : (d.y < 0.0 ? (box.y - O.y) / d.y : 1e9);
  float tz = (D - O.z) / d.z;
  float t = min(min(tx, ty), tz);
  vec3 H = O + t * d;
  float zf = clamp(H.z / D, 0.0, 1.0);

  vec3 wall; float lightAmt; vec2 uv;
  if (t == tz)            { wall = vec3(0.05, 0.16, 0.24); lightAmt = 0.9; uv = H.xy; }       // back wall
  else if (t == ty && d.y > 0.0) { wall = vec3(0.07, 0.19, 0.25); lightAmt = 1.0; uv = H.xz; } // floor
  else if (t == ty)       { wall = vec3(0.03, 0.09, 0.14); lightAmt = 0.45; uv = H.xz; }      // ceiling
  else                    { wall = vec3(0.04, 0.12, 0.19); lightAmt = 0.6; uv = H.zy; }       // side walls
  float c = caustic(uv, time) * 0.5 + caustic(uv * 1.7 + 40.0, time * 1.3) * 0.3;
  vec3 col = wall * (0.35 + 0.65 * lightAmt) + vec3(0.25, 0.55, 0.65) * c * 0.22 * lightAmt;
  // Distance fog: the far end of the tank dissolves into the dark water, which also frames the room.
  float fog = pow(zf, 0.9) * 0.88;
  col = mix(col, FOG, fog);
  col += (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;   // dither away banding
  outColour = vec4(col, 1.0);
}
