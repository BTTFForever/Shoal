// Fish outlines on the CPU, for previews drawn without the GPU (the Windows settings dialog). Same shapes, swimming wave
// and species table as shaders/fish.vert; the table is read from the embedded shader source, so it can't drift.
#pragma once

#include <vector>

struct ShoalFishShape { float width, peak, nose, rear, stalk, tail, fin, flare, fork, pect, tip; };

// The species table from fish.vert (SPECIES), in order. Empty if the shader source has no table.
const std::vector<ShoalFishShape>& ShoalSpeciesShapes();

struct ShoalPoint { float x, y; };
struct ShoalFishOutline
{
    std::vector<ShoalPoint> body;  // closed polygon: body and tail fin (forked or rounded)
    ShoalPoint fins[2][3];         // the two pectoral fins
};

// A fish `length` pixels long with its nose at (0, 0), facing +x (y down), swimming: `amp` 0 (straight glide) to about
// 1.3 (hard stroke), `phase` the tail-beat phase in radians, `curvature` the bend of a turn (1/pixels, 0 = straight).
// `tailScale` is the Tail size setting relative to its default (tailPercent / 33).
ShoalFishOutline ShoalFishOutlineFor(const ShoalFishShape& shape, float length, float tailScale, float amp, float phase,
                                     float curvature = 0.0f);
