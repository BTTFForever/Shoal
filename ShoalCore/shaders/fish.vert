#version 430
struct Boid { vec2 position; vec2 velocity; float turnRate; float turnTarget; float independentUntil; float nextTurnChange; float heading; float currentSpeed; float stopped; float breakawayHeading; float cruiseSpeed; float yawRate; float swimPhase; float thrust; float fishLength; float depth; };
struct Monitor { float left; float top; float right; float bottom; };
layout(std430, binding = 0) readonly buffer BoidBuf { Boid Boids[]; };
layout(std430, binding = 1) readonly buffer MonBuf { Monitor Monitors[]; };
// The body's centre line, SPINE_POINTS points per fish from nose to tail tip, kept by sim.comp (updateSpine): the head
// leads and each point follows the one in front, so the body trails along the path the head swam.
const uint SPINE_POINTS = 10u;
layout(std430, binding = 3) readonly buffer SpineBuf { vec2 Spines[]; };
uint spineBase; // this fish's first spine point (set in main)
// Spine point k (0 = nose). Kept as a function, not a copied array, because local arrays are slow on mobile GPUs;
// the Android build replaces the body with a read of its per-instance attributes (tools/es_shaders.py).
vec2 joint(int k) { return Spines[spineBase + uint(k)]; }
layout(std140, binding = 1) uniform Render { vec4 bounds; float baseLength; float monitorCount; float restrictToMonitor; float tailRatio; float colourCount; float tailShade; float depthEffect; float padding; vec4 colours[10]; };
// Which species swim: speciesInfo.x = how many are enabled, .y = 0 random mix / 1 one species per colour;
// speciesList holds the enabled species numbers (0-based), four per uvec4.
layout(std140, binding = 2) uniform SpeciesSet { uvec4 speciesInfo; uvec4 speciesList[3]; };
uniform int uShadow;
out vec2 vShape;   // x: position along the tail fin (0 at its root, 1 at its tip; -1 on the body), y: side (-1..1)
out float vAlong;  // 0 at the nose, 1 at the tail tip (tail shade)
flat out vec3 vColour;
flat out float vFork;  // tail-fin notch depth (negative = rounded end), for fish.frag

// The fish, seen from above, is one strip from nose to tail tip that bends with a travelling wave. It is drawn indexed,
// so each outline point is computed once and shared by the triangles around it:
//   vertices 0 .. STRIP_VERTICES-1   two per station (left, right): BODY_SEGMENTS+1 body stations from the nose to the
//                                    tail stalk, then FIN_SEGMENTS more along the tail fin (forked in fish.frag)
//   then 6                           two pectoral fins (a triangle each)
// The index list is built by the host (ShoalFishIndices in shoal_engine.cpp, ShoalRenderer on Android) from the same
// segment counts: segment n uses vertices 2n..2n+3 as two triangles, then the six fin vertices.
const uint BODY_SEGMENTS = 12u;
const uint FIN_SEGMENTS = 4u;
const uint STRIP_VERTICES = (BODY_SEGMENTS + FIN_SEGMENTS + 1u) * 2u;

const float PI = 3.14159265;
const float PIVOT = 0.3;

// ---------------------------------------------------------------------------------------------------------------- species
// One row per species, 12 numbers each. ShoalCore/tools/fish_gallery.py reads this table to draw the species gallery,
// so keep the "/* n Name */" comments and one row per line.
//   width  widest half-width / body length            peak   where the body is widest (0 nose .. 1 tail stalk)
//   nose   front curve exponent (1 = cone, bigger = blunter)   rear   back taper exponent (bigger = fuller hips)
//   stalk  tail-stalk half-width / widest half-width
//   tail   tail-fin length / body length (scaled by the Tail size setting, 33 % = as listed)
//   fin    fin end half-width / widest half-width     flare  how fast the fin widens (smaller = sooner)
//   fork   notch depth as a share of the fin (negative = rounded end)
//   pect   pectoral fin reach / widest half-width     tip    nose tip rounding (0 = sharp point, 1 = fully round)
const int SPECIES_COUNT = 10;
const float SPECIES[SPECIES_COUNT * 12] = float[](
    //            width  peak  nose  rear  stalk  tail   fin  flare  fork   pect   tip   -
    /* 1 Classic  */ 0.156, 0.30, 2.0, 1.4, 0.16, 0.26, 0.98, 0.70,  0.30, 0.55, 0.55, 0.0,
    /* 2 Sardine  */ 0.110, 0.34, 1.5, 1.2, 0.20, 0.22, 1.15, 0.60,  0.45, 0.40, 0.55, 0.0,
    /* 3 Goldfish */ 0.250, 0.38, 3.0, 1.8, 0.30, 0.48, 1.25, 0.60,  0.22, 0.55, 0.55, 0.0,
    /* 4 Koi      */ 0.190, 0.33, 3.5, 1.3, 0.24, 0.30, 1.00, 0.80,  0.12, 0.85, 0.55, 0.0,
    /* 5 Tuna     */ 0.175, 0.42, 1.6, 2.2, 0.07, 0.30, 1.65, 0.40,  0.60, 0.35, 0.55, 0.0,
    /* 6 Trout    */ 0.140, 0.33, 2.3, 1.2, 0.26, 0.22, 0.95, 0.80,  0.06, 0.45, 0.55, 0.0,
    /* 7 Pike     */ 0.085, 0.55, 1.2, 1.0, 0.32, 0.16, 1.00, 0.70,  0.15, 0.35, 0.55, 0.0,
    /* 8 Tetra    */ 0.200, 0.36, 2.2, 1.5, 0.22, 0.36, 1.10, 0.60,  0.42, 0.40, 0.55, 0.0,
    /* 9 Fantail  */ 0.170, 0.35, 2.6, 1.6, 0.30, 0.62, 1.95, 0.50, -0.30, 0.50, 0.55, 0.0,
    /* 10 Mackerel*/ 0.130, 0.40, 1.7, 1.6, 0.09, 0.27, 1.30, 0.45,  0.52, 0.35, 0.55, 0.0
);
struct Shape { float width; float peak; float nose; float rear; float stalk; float tail; float fin; float flare; float fork; float pect; float tip; };
Shape shapeOf(uint species) {
    int o = int(species) * 12;
    return Shape(SPECIES[o], SPECIES[o + 1], SPECIES[o + 2], SPECIES[o + 3], SPECIES[o + 4], SPECIES[o + 5], SPECIES[o + 6],
                 SPECIES[o + 7], SPECIES[o + 8], SPECIES[o + 9], SPECIES[o + 10]);
}

// Half-width of the body at u (0 = nose, 1 = tail stalk), relative to the widest point.
// Front: a tapered curve 1 - (1 - t)^nose blended with an ellipse quarter by `tip`, so the nose ends in a soft point.
// Back: a cosine taper from the widest point down to the tail stalk.
float bodyProfile(Shape f, float u) {
    if (u < f.peak) {
        float t = u / f.peak;
        float pointed = 1.0 - pow(1.0 - t, f.nose);
        float rounded = sqrt(max(0.0, 1.0 - (1.0 - t) * (1.0 - t)));
        return mix(pointed, rounded, f.tip);
    }
    float v = (u - f.peak) / (1.0 - f.peak);
    return f.stalk + (1.0 - f.stalk) * pow(max(cos(v * PI * 0.5), 0.0), f.rear);
}

// The swimming wave, in the fish's own frame. x runs from the nose (0) to the tail tip (1) in fish lengths.
// Lateral displacement h(x) = A(x) sin(k x - phase): the crest travels from head to tail as the phase grows (sim.comp
// advances the phase with distance travelled). A(x) is the carangiform envelope (Videler): about 2 % of the length at
// the head (recoil) up to 10 % at the tail.
struct Wave { float amp; float phase; float k; float len; };
const float WAVELENGTH = 0.95;
// Returns (h in pixels, dh/ds).
vec2 waveAt(Wave w, float x) {
    float a = w.amp * (0.02 - 0.0825 * x + 0.1625 * x * x);
    float da = w.amp * (-0.0825 + 0.325 * x);
    float arg = w.k * x - w.phase;
    return vec2(a * sin(arg) * w.len, da * sin(arg) + a * w.k * cos(arg));
}

// A point of the body's centre line, x fish lengths behind the nose, and the local forward direction there: a
// Catmull-Rom curve through the spine points (they are evenly spaced, so the tangents come straight from the
// neighbours), with the swimming wave across it. `scale` maps the simulated length to the drawn one (fish lower down
// are drawn smaller), around the nose.
struct Spine { vec2 point; vec2 forward; };
Spine spineAt(Wave w, vec2 nose, float scale, float x) {
    float f = clamp(x, 0.0, 1.0) * float(SPINE_POINTS - 1u);
    int j = min(int(f), int(SPINE_POINTS) - 2);
    float t = f - float(j);
    vec2 p0 = joint(j), p1 = joint(j + 1);
    vec2 pa = j > 0 ? joint(j - 1) : 2.0 * p0 - p1;                         // extend straight past the ends
    vec2 pb = j + 2 < int(SPINE_POINTS) ? joint(j + 2) : 2.0 * p1 - p0;
    vec2 m0 = 0.5 * (p1 - pa), m1 = 0.5 * (pb - p0);                          // tangents towards the tail
    float t2 = t * t, t3 = t2 * t;
    vec2 point = (2.0 * t3 - 3.0 * t2 + 1.0) * p0 + (t3 - 2.0 * t2 + t) * m0 + (-2.0 * t3 + 3.0 * t2) * p1 + (t3 - t2) * m1;
    vec2 back = (6.0 * t2 - 6.0 * t) * p0 + (3.0 * t2 - 4.0 * t + 1.0) * m0 + (-6.0 * t2 + 6.0 * t) * p1 + (3.0 * t2 - 2.0 * t) * m1;
    vec2 forward = -back * inversesqrt(max(dot(back, back), 1e-10));
    point = nose + (point - joint(0)) * scale;
    vec2 normal = vec2(-forward.y, forward.x);
    vec2 h = waveAt(w, x);
    return Spine(point + normal * h.x, normalize(forward - normal * h.y));
}

void main() {
    uint vertexId = uint(gl_VertexID);
    uint instanceId = uint(gl_InstanceID);
    bool shadow = uShadow != 0;
    Boid b = Boids[instanceId];
    float height = depthEffect > 0.5 ? b.depth : 1.0;
    float fishLength = b.fishLength * (0.8 + 0.2 * height);
    vec2 centre = b.position;
    if (restrictToMonitor > 0.5) { Monitor m = Monitors[instanceId % uint(monitorCount)]; centre = clamp(centre, vec2(m.left, m.top), vec2(m.right - 1.0, m.bottom - 1.0)); }
    vec2 dir = vec2(cos(b.heading), sin(b.heading));
    vec2 side = vec2(-dir.y, dir.x);
    // The simulated position is the fish's pivot, PIVOT lengths behind the nose (fish turn about a point near the head);
    // sim.comp's SPINE_PIVOT must match.
    vec2 nose = centre + dir * fishLength * PIVOT;

    // Beat amplitude: full while thrusting, fading towards a straight glide when coasting (thrust is set in sim.comp).
    float amp = clamp(b.thrust, 0.08, 1.3);
    // The wavelength is set against the simulated length (sim.comp's stride), so the slip is the same at every depth.
    float k = 2.0 * PI / WAVELENGTH * fishLength / max(b.fishLength, 1.0);
    Wave w = Wave(amp, b.swimPhase, k, fishLength);
    spineBase = instanceId * SPINE_POINTS;
    float scale = fishLength / max(b.fishLength, 1.0);

    // Species: a random mix of the enabled ones, or one per colour.
    uint colourIndex = ((instanceId * 2654435761u) >> 16) % max(uint(colourCount), 1u);
    uint enabled = max(speciesInfo.x, 1u);
    uint pick = (speciesInfo.y == 1u ? colourIndex : ((instanceId * 2246822519u) >> 13)) % enabled;
    uint species = speciesInfo.x == 0u ? 0u : min(speciesList[pick / 4u][pick % 4u], uint(SPECIES_COUNT - 1));
    Shape f = shapeOf(species);

    float tailFraction = f.tail * tailRatio / 0.33;          // the Tail size setting scales every species' tail
    float bodyLength = fishLength / (1.0 + tailFraction);
    float bodyEnd = bodyLength / fishLength;                 // where the tail fin starts, in fish lengths
    float maxHalfWidth = f.width * bodyLength;
    float stalkHalfWidth = maxHalfWidth * f.stalk;
    float finHalfWidth = maxHalfWidth * f.fin;
    vFork = f.fork;

    vec2 position;
    float x;
    if (vertexId < STRIP_VERTICES) {
        uint station = vertexId / 2u;
        float sideSign = (vertexId & 1u) == 0u ? -1.0 : 1.0;
        float halfWidth;
        if (station < BODY_SEGMENTS) {
            // Stations bunched towards the nose and the stalk, where the outline curves most.
            float t = float(station) / float(BODY_SEGMENTS);
            float u = 0.5 - 0.5 * cos(t * PI);
            x = u * bodyEnd;
            halfWidth = maxHalfWidth * bodyProfile(f, u);
            vShape = vec2(-1.0, sideSign);
        } else {
            // The tail fin, from the stalk (shared with the body's last station) to the tip.
            float u = float(station - BODY_SEGMENTS) / float(FIN_SEGMENTS);
            x = mix(bodyEnd, 1.0, u);
            halfWidth = mix(stalkHalfWidth, finHalfWidth, pow(u, f.flare));
            vShape = vec2(u, sideSign);
        }
        Spine sp = spineAt(w, nose, scale, x);
        position = sp.point + vec2(-sp.forward.y, sp.forward.x) * halfWidth * sideSign;
    } else {
        // Pectoral fins: a swept-back triangle each side, rowing gently with the beat.
        uint k = vertexId - STRIP_VERTICES;
        float s = k < 3u ? -1.0 : 1.0;
        uint c = k % 3u;
        float root = 0.2;
        float rootWidth = maxHalfWidth * bodyProfile(f, root / bodyEnd);
        float row = 0.12 * sin(b.swimPhase + s * 1.2) * clamp(b.thrust, 0.2, 1.0);
        float px = c == 0u ? root : (c == 1u ? root + 0.11 : root + 0.17 + 0.03 * row);
        float out_ = c == 2u ? rootWidth + maxHalfWidth * (f.pect + row) : rootWidth * 0.85;
        x = px;
        Spine sp = spineAt(w, nose, scale, x);
        position = sp.point + vec2(-sp.forward.y, sp.forward.x) * out_ * s;
        vShape = vec2(-1.0, 0.0);
    }

    if (shadow) position += vec2(0.7071, 0.7071) * fishLength * (0.04 + 0.08 * height);
    float z = 0.5 - 0.4 * height + (shadow ? 0.00001 : 0.0);
    gl_Position = vec4((position.x - bounds.x) / (bounds.z - bounds.x) * 2.0 - 1.0, 1.0 - (position.y - bounds.y) / (bounds.w - bounds.y) * 2.0, z, 1.0);
    vAlong = x;
    vColour = colours[colourIndex].rgb * (0.6 + 0.4 * height);
}
