#version 430
layout(std140, binding = 1) uniform Render {
  vec4 win;
  vec4 box;
  float D, camDist, cx, cy;
  float tailRatio, tailShade, colourCount, time;
  vec4 colours[10];
};
in vec2 vPix;
flat in vec3 fCentre;
flat in vec3 fAxis;
flat in vec2 fRadii;
flat in vec3 fColour;
flat in vec3 fNose;
flat in vec3 fForward;
flat in float fLen;
out vec4 outColour;

const vec3 FOG = vec3(0.012, 0.055, 0.095);

void main() {
  vec3 O = vec3(cx, cy, -camDist);
  vec3 d = vec3(vPix.x - cx, vPix.y - cy, camDist);          // ray from the camera through this pixel; t = 1 at the z = 0 plane
  vec3 o = O - fCentre;
  float ra = fRadii.x, rb = fRadii.y;
  float oa = dot(o, fAxis), da = dot(d, fAxis);
  float od = dot(o, d), oo = dot(o, o), dd = dot(d, d);
  float A = da * da / (ra * ra) + (dd - da * da) / (rb * rb);
  float B = oa * da / (ra * ra) + (od - oa * da) / (rb * rb);
  float C = oa * oa / (ra * ra) + (oo - oa * oa) / (rb * rb) - 1.0;
  float disc = B * B - A * C;
  if (disc < 0.0) discard;
  float t = (-B - sqrt(disc)) / A;
  if (t < 0.0) discard;
  vec3 p = o + t * d;                                         // hit point relative to the centre
  vec3 hit = fCentre + p;
  float pa = dot(p, fAxis);
  vec3 n = normalize(pa / (ra * ra) * fAxis + (p - fAxis * pa) / (rb * rb));

  // Head-to-tail gradient, as in the 2D version: 0 at the nose, 1 at the tail tip.
  float along = clamp(dot(fNose - hit, fForward) / fLen, 0.0, 1.0);
  float sh = along * tailShade;
  vec3 base = sh < 0.0 ? fColour * (1.0 + sh) : mix(fColour, vec3(1.0), sh);

  // Light from above, left and slightly behind the viewer.
  vec3 toLight = normalize(vec3(-0.35, -0.8, -0.45));
  vec3 view = -normalize(d);
  float diff = max(dot(n, toLight), 0.0);
  float spec = pow(max(dot(n, normalize(toLight + view)), 0.0), 40.0) * 0.35;
  float rim = pow(1.0 - max(dot(n, view), 0.0), 3.0) * 0.12;
  vec3 col = base * (0.30 + 0.85 * diff) + vec3(spec) + base * rim;

  float z = clamp(hit.z / D, 0.0, 1.0);
  float fog = pow(z, 1.15) * 0.72;
  col = mix(col, FOG, fog);
  outColour = vec4(col, 1.0);
  gl_FragDepth = clamp((hit.z + camDist) / (D + camDist), 0.0, 1.0);
}
