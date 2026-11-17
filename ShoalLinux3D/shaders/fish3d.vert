#version 430
struct Boid {
  float px, py, pz;
  float fx, fy, fz;
  float wanderYaw, wanderPitch, wanderYawT, wanderPitchT;
  float independentUntil, nextTurnChange;
  float bx, by, bz;
  float speed, cruise, reserved, yawRate, phase, thrust, len, pad;
};
layout(std430, binding = 0) readonly buffer BoidBuf { Boid Boids[]; };
layout(std140, binding = 1) uniform Render {
  vec4 win;
  vec4 box;
  float D, camDist, cx, cy;
  float tailRatio, tailShade, colourCount, time;
  vec4 colours[10];
};

out vec2 vPix;                    // desktop pixel position of this vertex
flat out vec3 fCentre;            // ellipsoid centre (world)
flat out vec3 fAxis;              // unit axis of the spheroid
flat out vec2 fRadii;             // radius along the axis, radius across it
flat out vec3 fColour;
flat out vec3 fNose;
flat out vec3 fForward;
flat out float fLen;

const vec3 UP = vec3(0.0, -1.0, 0.0);          // screen up

float swimOmega(float fishSpeed, float len, uint i) { return 6.2831853 * clamp(1.5 * fishSpeed / max(len, 1.0), 0.8, 5.0) * (0.9 + 0.2 * fract(float(i) * 0.61803399)); }

vec3 rotateAbout(vec3 axis, vec3 v, float a) { float c = cos(a), s = sin(a); return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c); }

const vec2 quad[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
  uint instance = uint(gl_InstanceID);
  uint fish = instance >> 1;
  bool isTail = (instance & 1u) != 0u;
  Boid b = Boids[fish];
  vec3 pos = vec3(b.px, b.py, b.pz);
  vec3 fwd = normalize(vec3(b.fx, b.fy, b.fz));
  float L = b.len;

  // Body sway and yaw wobble in time with the tail beat (visual only), then the tail angle.
  float omega = swimOmega(b.speed, L, fish);
  float amp = min(L * 0.04, b.speed * 0.1 / omega) * min(b.thrust, 1.2);
  vec3 side = cross(UP, fwd);
  side = length(side) > 0.001 ? normalize(side) : vec3(1.0, 0.0, 0.0);
  pos += side * amp * sin(b.phase);
  vec3 f2 = rotateAbout(UP, fwd, atan(amp * omega, max(b.speed, 0.001)) * cos(b.phase));
  float sweep = clamp(0.25 + 0.15 * b.speed / (L * 4.0), 0.25, 0.6) * min(b.thrust, 1.3);
  float tailAngle = clamp(-sweep * cos(b.phase - 0.6) - clamp(b.yawRate * 0.12, -0.5, 0.5), -0.9, 0.9);
  vec3 tailDir = rotateAbout(UP, f2, tailAngle);

  // Fish shape as in the 2D version: a spheroid body plus a smaller spheroid tail overlapping it, together spanning L.
  float bodyLen = L / (1.0 + 0.8 * tailRatio);
  vec2 bodyHalf = vec2(0.5, 0.15625) * bodyLen;
  vec2 tailHalf = bodyHalf * tailRatio;
  vec3 centre, axis; vec2 radii;
  if (!isTail) { axis = f2; centre = pos + f2 * (L * 0.5 - bodyHalf.x); radii = bodyHalf; }
  else { axis = tailDir; centre = pos - f2 * (L * 0.5 - 1.85 * tailHalf.x) - tailDir * 0.85 * tailHalf.x; radii = tailHalf; }

  // Camera at (cx, cy, -camDist) looking along +z; the z = 0 plane maps to the screen unscaled.
  float R = max(radii.x, radii.y);
  vec2 cc = vec2(cx, cy);
  float s = camDist / (camDist + centre.z);
  vec2 sc = cc + (centre.xy - cc) * s;
  float sNear = camDist / max(camDist + centre.z - R, 1.0);
  float off = length(centre.xy - cc) / camDist;
  float hs = R * sNear * (1.1 + off);
  vec2 corner = quad[uint(gl_VertexID) % 6u];
  vec2 p = sc + corner * hs;
  vPix = p;
  gl_Position = vec4((p.x - win.x) / (win.z - win.x) * 2.0 - 1.0, 1.0 - (p.y - win.y) / (win.w - win.y) * 2.0, 0.0, 1.0);

  uint colourIndex = ((fish * 2654435761u) >> 16) % max(uint(colourCount), 1u);
  fCentre = centre; fAxis = axis; fRadii = radii;
  fColour = colours[colourIndex].rgb;
  fNose = pos + f2 * (L * 0.5);
  fForward = f2;
  fLen = L;
}
