// CPU fish outlines, see shoal_fishshape.h. The maths follows fish.vert and tools/fish_gallery.py.
#include "shoal_fishshape.h"
#include "shoal_engine.h" // ShoalShaderSource

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const float kPi = 3.14159265f;

std::vector<ShoalFishShape> ParseSpecies()
{
    // Rows look like:  /* 1 Classic  */ 0.156, 0.30, 2.0, ...   (11 numbers used, a 12th spare)
    std::vector<ShoalFishShape> out;
    const char* src = ShoalShaderSource("fish.vert");
    if (!src) return out;
    std::string text(src);
    size_t table = text.find("const float SPECIES[");
    size_t end = table == std::string::npos ? std::string::npos : text.find(");", table);
    if (end == std::string::npos) return out;
    size_t pos = table;
    while ((pos = text.find("/*", pos)) != std::string::npos && pos < end)
    {
        size_t close = text.find("*/", pos);
        size_t lineEnd = text.find('\n', close);
        if (close == std::string::npos || lineEnd == std::string::npos) break;
        std::string row = text.substr(close + 2, lineEnd - close - 2);
        float v[12] = {};
        int n = 0;
        const char* p = row.c_str();
        while (*p && n < 12)
        {
            char* next = nullptr;
            float f = std::strtof(p, &next);
            if (next == p) { ++p; continue; }
            v[n++] = f;
            p = next;
        }
        if (n >= 11) out.push_back({ v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10] });
        pos = lineEnd;
    }
    return out;
}

float BodyProfile(const ShoalFishShape& f, float u)
{
    if (u < f.peak)
    {
        float t = u / f.peak;
        float pointed = 1.0f - std::pow(1.0f - t, f.nose);
        float rounded = std::sqrt(std::max(0.0f, 1.0f - (1.0f - t) * (1.0f - t)));
        return pointed + (rounded - pointed) * f.tip;
    }
    float v = (u - f.peak) / (1.0f - f.peak);
    return f.stalk + (1.0f - f.stalk) * std::pow(std::max(std::cos(v * kPi * 0.5f), 0.0f), f.rear);
}

struct Spine { float px, py, fx, fy; };

// Centre line x lengths behind the nose, as in fish.vert (an arc for the turn, with the swimming wave across it).
Spine SpineAt(float x, float length, float amp, float phase, float curvature)
{
    float s = x * length;
    float cs = curvature * s;
    float back = std::fabs(cs) < 1e-4f ? s : std::sin(cs) / curvature;
    float across = std::fabs(cs) < 1e-4f ? 0.5f * cs * s : (1.0f - std::cos(cs)) / curvature;
    float px = -back, py = across;
    float fx = std::cos(cs), fy = -std::sin(cs);
    float nx = -fy, ny = fx;
    float k = 2.0f * kPi / 0.95f;
    float a = amp * (0.02f - 0.0825f * x + 0.1625f * x * x);
    float da = amp * (-0.0825f + 0.325f * x);
    float arg = k * x - phase;
    float h = a * std::sin(arg) * length;
    float dh = da * std::sin(arg) + a * k * std::cos(arg);
    px += nx * h; py += ny * h;
    fx -= nx * dh; fy -= ny * dh;
    float n = std::sqrt(fx * fx + fy * fy);
    return { px, py, fx / n, fy / n };
}

} // namespace

const std::vector<ShoalFishShape>& ShoalSpeciesShapes()
{
    static const std::vector<ShoalFishShape> shapes = ParseSpecies();
    return shapes;
}

ShoalFishOutline ShoalFishOutlineFor(const ShoalFishShape& f, float length, float tailScale, float amp, float phase, float curvature)
{
    const float tail = f.tail * tailScale;
    const float bodyLen = length / (1.0f + tail);
    const float bodyEnd = bodyLen / length;
    const float maxHw = f.width * bodyLen;
    const float stalkHw = maxHw * f.stalk;
    const float finHw = maxHw * f.fin;
    auto at = [&](float x, float side) {
        Spine sp = SpineAt(x, length, amp, phase, curvature);
        return ShoalPoint{ sp.px - sp.fy * side, sp.py + sp.fx * side };
    };
    auto finHwAt = [&](float u) { return stalkHw + (finHw - stalkHw) * std::pow(u, f.flare); };
    auto finEnd = [&](float v) { return f.fork >= 0 ? 1.0f - f.fork * (1.0f - std::fabs(v)) : 1.0f + f.fork * v * v; };

    ShoalFishOutline out;
    std::vector<ShoalPoint> left, right;
    const int bodySteps = 24, finSteps = 10, endSteps = 16;
    for (int i = 0; i <= bodySteps; ++i)
    {
        float u = 0.5f - 0.5f * std::cos((float)i / bodySteps * kPi);
        float hw = maxHw * BodyProfile(f, u);
        left.push_back(at(u * bodyEnd, -hw));
        right.push_back(at(u * bodyEnd, hw));
    }
    for (int i = 1; i <= finSteps; ++i)
    {
        float u = (float)i / finSteps * finEnd(-1.0f);
        float x = bodyEnd + (1.0f - bodyEnd) * u;
        left.push_back(at(x, -finHwAt(u)));
        right.push_back(at(x, finHwAt(u)));
    }
    out.body = left;
    for (int i = 0; i <= endSteps; ++i)
    {
        float v = -1.0f + 2.0f * i / endSteps;
        float u = finEnd(v);
        out.body.push_back(at(bodyEnd + (1.0f - bodyEnd) * u, v * finHwAt(u)));
    }
    out.body.insert(out.body.end(), right.rbegin(), right.rend());

    const float root = 0.2f;
    const float rootHw = maxHw * BodyProfile(f, root / bodyEnd);
    for (int s = 0; s < 2; ++s)
    {
        float sign = s == 0 ? -1.0f : 1.0f;
        out.fins[s][0] = at(root, sign * rootHw * 0.85f);
        out.fins[s][1] = at(root + 0.11f, sign * rootHw * 0.85f);
        out.fins[s][2] = at(root + 0.17f, sign * (rootHw + maxHw * f.pect));
    }
    return out;
}
