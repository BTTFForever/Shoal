// Shoal engine. Merged from ShoalLinux/shoal.cpp (several monitors, rocks) and ShoalWallpaper's engine (per-instance
// state, desktop obstacles, see-through water), plus a background picture. See shoal_engine.h.
#include "shoal_engine.h"
#include "shoal_gl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace {

struct Pt { int x, y; };
struct Rock { float x, y, radius; int pointCount; Pt points[6]; };
struct Boid { float x, y, vx, vy, breakawayHeading, swimTurnRate, swimTurnTarget, length, depth; };
struct GpuBoid
{
    float x, y, vx, vy, turnRate, turnTarget, independentUntil, nextTurnChange, heading, currentSpeed, stopped, breakawayHeading,
        cruiseSpeed, yawRate, swimPhase, thrust, fishLength, depth;
};
struct GpuMonitor { float left, top, right, bottom; };
struct GpuSimulationConstants
{
    float deltaSeconds, speed, boidCount, randomness, monitorCount, timeSeconds, rockCount, restrictToMonitor, baseLength,
        speedVariability, colourCount, colourBias, colourAvoid, padding[3];
};
struct GpuRenderConstants
{
    float left, top, right, bottom, length, monitorCount, restrictToMonitor, tailRatio, colourCount, tailShade, depthEffect, padding;
    float colours[10][4];
};
struct GpuSpecies { uint32_t info[4]; uint32_t list[12]; }; // std140 SpeciesSet in fish.vert
struct Vertex { float x, y, r, g, b, a; };
static_assert(sizeof(ShoalObstacle) == 16 * sizeof(float), "ShoalObstacle must match the GLSL Rock struct");

const float kReferenceBoidLength = 20.0f;
const float kRockBufferDistance = 40.0f;
const size_t kMaxOverlayVertices = 2048;
// Fish mesh, drawn indexed (see fish.vert): segment counts must match fish.vert's BODY_SEGMENTS and FIN_SEGMENTS.
const int kBodySegments = 12, kFinSegments = 4;
// Spine chain per fish (sim.comp updateSpine, fish.vert): points from nose to tail tip, and the head pivot.
const int kSpinePoints = 10;
const float kSpinePivot = 0.3f;
std::vector<uint16_t> FishIndices()
{
    std::vector<uint16_t> idx;
    const int segments = kBodySegments + kFinSegments;
    for (int n = 0; n < segments; ++n)
    {
        const uint16_t a0 = (uint16_t)(2 * n), a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
        idx.insert(idx.end(), { a0, b0, a1, b0, b1, a1 });
    }
    const uint16_t fins = (uint16_t)(2 * (segments + 1));
    for (uint16_t k = 0; k < 6; ++k) idx.push_back(fins + k);
    return idx;
}
const double kMinFishCoverage = 0.00140625;
const double kMaxFishCoverage = 302500.0 / 4147200.0;

} // namespace

const char* const kShoalSpeciesNames[kShoalSpeciesCount] = { "Classic", "Sardine", "Goldfish", "Koi", "Tuna", "Trout", "Pike",
                                                             "Tetra", "Fantail", "Mackerel" };

namespace {

std::function<void(const std::string&)> g_log = [](const std::string& s) { std::fprintf(stderr, "shoal: %s\n", s.c_str()); };

float Len(float x, float y) { return std::sqrt(x * x + y * y); }
int BoidLengthForSize(int tenths, float height) { return std::max(4, (int)std::lround(height * tenths / 1000.0)); }
double FishCoverageFraction(int p) { return kMinFishCoverage * std::pow(kMaxFishCoverage / kMinFishCoverage, p / 100.0); }
double SizeAreaFactor(int v, bool bell)
{
    if (v <= 100) return 1.0;
    double k = 2.0 * std::log(v / 100.0) / (bell ? 3.0 : 1.0);
    double f = std::sinh(k) / k;
    return bell ? f * f * f : f;
}
int BoidCountFor(int position, int length, double freeArea, double areaFactor)
{
    double count = FishCoverageFraction(position) * freeArea / ((double)length * length * 0.25 * areaFactor);
    return count < 2 ? 2 : (count > 10000 ? 10000 : (int)std::lround(count));
}
double RockArea(const Rock& r)
{
    double a = 0;
    for (int i = 0; i < r.pointCount; ++i)
    {
        const Pt& p = r.points[i];
        const Pt& q = r.points[(i + 1) % r.pointCount];
        a += (double)p.x * q.y - (double)q.x * p.y;
    }
    return std::fabs(a) * 0.5;
}
bool IsPointInsideRock(const Rock& r, float x, float y)
{
    bool inside = false;
    for (int p = 0, q = r.pointCount - 1; p < r.pointCount; q = p++)
    {
        float x1 = r.x + r.points[p].x, y1 = r.y + r.points[p].y, x2 = r.x + r.points[q].x, y2 = r.y + r.points[q].y;
        if ((y1 > y) != (y2 > y) && x < (x2 - x1) * (y - y1) / (y2 - y1) + x1) inside = !inside;
    }
    return inside;
}
float DistanceToRockWall(const Rock& r, float x, float y)
{
    float best = 1e9f;
    for (int p = 0; p < r.pointCount; ++p)
    {
        const Pt& a = r.points[p];
        const Pt& b = r.points[(p + 1) % r.pointCount];
        float x1 = r.x + a.x, y1 = r.y + a.y, dx = (float)(b.x - a.x), dy = (float)(b.y - a.y);
        float l2 = dx * dx + dy * dy;
        float t = l2 > 0.001f ? std::max(0.0f, std::min(1.0f, ((x - x1) * dx + (y - y1) * dy) / l2)) : 0.0f;
        best = std::min(best, Len(x - (x1 + dx * t), y - (y1 + dy * t)));
    }
    return best;
}

} // namespace

// ------------------------------------------------------------------------------------------------ engine state
struct ShoalEngineState
{
    // A background picture slot (see ShoalEngine::setPicture).
    struct Picture
    {
        std::vector<uint8_t> pixels; // waiting for upload
        int width = 0, height = 0;
        bool hasArea = false, dirty = false;
        ShoalRect area;
        GLuint texture = 0;
    };

    ~ShoalEngineState()
    {
        if (!m_glReady) return;
        GLuint bufs[] = { m_boidBuffer, m_monitorBuffer, m_rockBuffer, m_simUbo, m_renderUbo, m_overlayVbo, m_speciesUbo, m_fishIndexBuffer, m_spineBuffer };
        glDeleteBuffers(9, bufs);
        GLuint vaos[] = { m_overlayVao, m_emptyVao };
        glDeleteVertexArrays(2, vaos);
        for (Picture& pic : m_pictures) if (pic.texture) glDeleteTextures(1, &pic.texture);
        for (GLuint p : { m_simProgram, m_rockProgram, m_fishProgram, m_overlayProgram, m_bgProgram }) if (p) glDeleteProgram(p);
    }

    void Fail(const std::string& msg) { if (m_error.empty()) m_error = msg; g_log(msg); }

    // ---------------------------------------------------------------- inputs
    void SetSettings(const ShoalSettings& n)
    {
        const ShoalSettings& o = m_s;
        if (!m_haveSettings || n.fishCoverage != o.fishCoverage || n.boidCount != o.boidCount || n.sizeTenths != o.sizeTenths
            || n.sizeVariation != o.sizeVariation || n.sizeBell != o.sizeBell || n.rockCount != o.rockCount)
            m_rebuild = true;
        m_s = n;
        m_haveSettings = true;
    }

    void SetMonitors(const std::vector<ShoalRect>& monitors, float referenceHeight)
    {
        if (monitors.size() != m_monitors.size() || referenceHeight != m_referenceHeight) m_rebuild = true;
        if (monitors != m_monitors) m_monitorsDirty = true;
        m_monitors = monitors;
        m_referenceHeight = referenceHeight;
    }

    // ---------------------------------------------------------------- world setup
    float RandomFloat(float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); }
    float MaxBoidLength() const { return (float)m_boidLength * m_s.sizeVariation / 100.0f; }
    float RockBuffer() const { return kRockBufferDistance + std::max(0.0f, (MaxBoidLength() - kReferenceBoidLength) * 0.5f); }

    bool IsOutsideRockBuffers(float x, float y) const
    {
        for (const Rock& r : m_rocks)
            if (IsPointInsideRock(r, x, y) || DistanceToRockWall(r, x, y) < RockBuffer()) return false;
        // Hard desktop obstacles (icons, widgets) are axis-aligned rectangles; soft ones (windows) don't block spawning.
        for (const ShoalObstacle& o : m_static)
        {
            if (o.radius < 0.0f) continue;
            float hx = std::fabs(o.points[0]), hy = std::fabs(o.points[1]);
            if (std::fabs(x - o.x) < hx && std::fabs(y - o.y) < hy) return false;
        }
        return true;
    }

    void CreateRocks()
    {
        m_rocks.clear();
        const float rockBuffer = RockBuffer();
        const float clearance = rockBuffer + 50.0f;
        for (int idx = 0; idx < m_s.rockCount && !m_monitors.empty(); ++idx)
        {
            bool placed = false;
            for (int attempt = 0; attempt < 200 && !placed; ++attempt)
            {
                const ShoalRect& m = m_monitors[std::uniform_int_distribution<size_t>(0, m_monitors.size() - 1)(m_rng)];
                float maxR = (float)((int)std::min(m.width(), m.height()) / 10);
                if (maxR < 90.0f) continue;
                Rock rock = {};
                rock.radius = RandomFloat(90.0f, maxR);
                rock.pointCount = (int)RandomFloat(4.0f, 6.999f);
                float largest = 0;
                bool gapOk = true;
                for (int p = 0; p < rock.pointCount; ++p)
                {
                    float a = 2.0f * 3.14159265f * p / rock.pointCount;
                    float pr = rock.radius * RandomFloat(0.7f, 1.0f);
                    rock.points[p].x = (int)(std::cos(a) * pr);
                    rock.points[p].y = (int)(std::sin(a) * pr);
                    largest = std::max(largest, pr);
                }
                for (int p = 0; p < rock.pointCount; ++p)
                {
                    const Pt& a = rock.points[p];
                    const Pt& b = rock.points[(p + 1) % rock.pointCount];
                    if (Len((float)(b.x - a.x), (float)(b.y - a.y)) < 100.0f) { gapOk = false; break; }
                }
                if (!gapOk) continue;
                float margin = largest + clearance;
                float minX = m.left + margin, maxX = m.right - margin, minY = m.top + margin, maxY = m.bottom - margin;
                if (minX >= maxX || minY >= maxY) continue;
                rock.x = RandomFloat(minX, maxX);
                rock.y = RandomFloat(minY, maxY);
                bool clear = true;
                for (const Rock& o : m_rocks)
                {
                    float oLargest = 0;
                    for (int p = 0; p < o.pointCount; ++p) oLargest = std::max(oLargest, Len((float)o.points[p].x, (float)o.points[p].y));
                    if (Len(rock.x - o.x, rock.y - o.y) < largest + oLargest + 2.0f * rockBuffer + 50.0f) { clear = false; break; }
                }
                if (clear) { m_rocks.push_back(rock); placed = true; }
            }
        }
    }

    void CreateBoids()
    {
        m_boids.clear();
        if (m_monitors.empty()) return;
        int count = m_s.boidCount;
        if (count <= 0)
        {
            double freeArea = 0;
            for (const ShoalRect& r : m_monitors) freeArea += (double)r.width() * r.height();
            for (const Rock& r : m_rocks) freeArea -= RockArea(r);
            count = BoidCountFor(m_s.fishCoverage, m_boidLength, freeArea, SizeAreaFactor(m_s.sizeVariation, m_s.sizeBell));
        }
        for (int i = 0; i < count; ++i)
        {
            const ShoalRect& m = m_monitors[(size_t)i % m_monitors.size()];
            Boid b;
            float u = RandomFloat(-1.0f, 1.0f);
            if (m_s.sizeBell) u = (u + RandomFloat(-1.0f, 1.0f) + RandomFloat(-1.0f, 1.0f)) / 3.0f;
            b.length = std::max(4.0f, (float)m_boidLength * std::pow(m_s.sizeVariation / 100.0f, u));
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                float inset = std::min(b.length, std::min(m.width(), m.height()) * 0.25f);
                b.x = RandomFloat(m.left + inset, m.right - inset);
                b.y = RandomFloat(m.top + inset, m.bottom - inset);
                if (IsOutsideRockBuffers(b.x, b.y)) break;
            }
            float angle = RandomFloat(0.0f, 2.0f * 3.14159265f);
            b.vx = std::cos(angle) * m_s.speed;
            b.vy = std::sin(angle) * m_s.speed;
            b.breakawayHeading = angle;
            b.swimTurnRate = RandomFloat(-0.35f, 0.35f);
            b.swimTurnTarget = RandomFloat(-0.35f, 0.35f);
            b.depth = 0;
            m_boids.push_back(b);
        }
        // Depth: small fish swim lower, ties broken randomly.
        std::vector<std::pair<float, float>> order;
        for (const Boid& b : m_boids) order.emplace_back(b.length, RandomFloat(0.0f, 1.0f));
        std::vector<size_t> ranked(m_boids.size());
        for (size_t i = 0; i < ranked.size(); ++i) ranked[i] = i;
        std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) { return order[a] < order[b]; });
        for (size_t r = 0; r < ranked.size(); ++r)
            m_boids[ranked[r]].depth = ranked.size() > 1 ? (float)r / (float)(ranked.size() - 1) : 0.5f;
    }

    void RebuildWorld()
    {
        m_boidLength = BoidLengthForSize(m_s.sizeTenths, m_referenceHeight);
        CreateRocks();
        CreateBoids();
        std::vector<GpuBoid> init;
        for (size_t i = 0; i < m_boids.size(); ++i)
        {
            const Boid& b = m_boids[i];
            float phase = std::fmod((float)i * 2.39996323f, 6.2831853f);
            float sp = (float)m_s.speed;
            init.push_back({ b.x, b.y, b.vx, b.vy, b.swimTurnRate, b.swimTurnTarget, 0.0f, 0.0f, std::atan2(b.vy, b.vx), sp, 0.0f,
                b.breakawayHeading, sp, 0.0f, phase, 1.0f, b.length, b.depth });
        }
        if (init.empty()) init.push_back(GpuBoid{});
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_boidBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GpuBoid) * init.size(), init.data(), GL_DYNAMIC_DRAW);
        // Spines start straight behind each fish's heading.
        std::vector<float> spines;
        for (const GpuBoid& g : init)
        {
            const float cx = std::cos(g.heading), cy = std::sin(g.heading);
            for (int j = 0; j < kSpinePoints; ++j)
            {
                const float back = g.fishLength * ((float)j / (kSpinePoints - 1) - kSpinePivot);
                spines.push_back(g.x - cx * back);
                spines.push_back(g.y - cy * back);
            }
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_spineBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, spines.size() * sizeof(float), spines.data(), GL_DYNAMIC_DRAW);
        m_simTime = 0.0f;
        m_rocksDirty = true;
        char line[160];
        std::snprintf(line, sizeof line, "%zu fish (length %d), %zu rocks, %zu monitor(s)", m_boids.size(), m_boidLength,
            m_rocks.size(), m_monitors.size());
        g_log(line);
    }

    // ---------------------------------------------------------------- GL
    GLuint CompileStage(GLenum type, const char* file)
    {
        const char* src = ShoalShaderSource(file);
        if (!src) { Fail(std::string("missing shader ") + file); return 0; }
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[4096] = {};
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            Fail(std::string("shader ") + file + " failed: " + log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    GLuint LinkProgram(std::initializer_list<GLuint> stages, const char* name)
    {
        bool ok = true;
        for (GLuint s : stages) if (!s) ok = false;
        if (!ok) { for (GLuint s : stages) if (s) glDeleteShader(s); return 0; }
        GLuint p = glCreateProgram();
        for (GLuint s : stages) glAttachShader(p, s);
        glLinkProgram(p);
        for (GLuint s : stages) glDeleteShader(s);
        GLint linked = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            char log[4096] = {};
            glGetProgramInfoLog(p, sizeof log, nullptr, log);
            Fail(std::string("program ") + name + " failed: " + log);
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    bool InitGpu()
    {
        m_simProgram = LinkProgram({ CompileStage(GL_COMPUTE_SHADER, "sim.comp") }, "sim");
        m_rockProgram = LinkProgram({ CompileStage(GL_COMPUTE_SHADER, "rock.comp") }, "rock");
        m_fishProgram = LinkProgram({ CompileStage(GL_VERTEX_SHADER, "fish.vert"), CompileStage(GL_FRAGMENT_SHADER, "fish.frag") }, "fish");
        m_overlayProgram = LinkProgram({ CompileStage(GL_VERTEX_SHADER, "overlay.vert"), CompileStage(GL_FRAGMENT_SHADER, "overlay.frag") }, "overlay");
        m_bgProgram = LinkProgram({ CompileStage(GL_VERTEX_SHADER, "bg.vert"), CompileStage(GL_FRAGMENT_SHADER, "bg.frag") }, "bg");
        if (!m_simProgram || !m_rockProgram || !m_fishProgram || !m_overlayProgram || !m_bgProgram) return false;
        m_fishShadowLoc = glGetUniformLocation(m_fishProgram, "uShadow");
        m_bgUvLoc = glGetUniformLocation(m_bgProgram, "uUv");
        m_bgDimLoc = glGetUniformLocation(m_bgProgram, "uDim");

        glGenBuffers(1, &m_boidBuffer);
        glGenBuffers(1, &m_monitorBuffer);
        glGenBuffers(1, &m_rockBuffer);
        glGenBuffers(1, &m_spineBuffer);
        glGenBuffers(1, &m_simUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, m_simUbo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(GpuSimulationConstants), nullptr, GL_DYNAMIC_DRAW);
        glGenBuffers(1, &m_renderUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, m_renderUbo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(GpuRenderConstants), nullptr, GL_DYNAMIC_DRAW);

        glGenBuffers(1, &m_speciesUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, m_speciesUbo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(GpuSpecies), nullptr, GL_DYNAMIC_DRAW);

        glGenVertexArrays(1, &m_emptyVao);
        // The fish index list lives in the (otherwise empty) VAO used for the fish draws.
        const std::vector<uint16_t> indices = FishIndices();
        m_fishIndexCount = (GLsizei)indices.size();
        glBindVertexArray(m_emptyVao);
        glGenBuffers(1, &m_fishIndexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_fishIndexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        glGenVertexArrays(1, &m_overlayVao);
        glGenBuffers(1, &m_overlayVbo);
        glBindVertexArray(m_overlayVao);
        glBindBuffer(GL_ARRAY_BUFFER, m_overlayVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * kMaxOverlayVertices, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)8);
        glBindVertexArray(0);
        return true;
    }

    // Apply recorded changes; only called with the context current (step/draw).
    void Sync()
    {
        if (!m_glReady || m_monitors.empty() || !m_haveSettings) return;
        if (m_rebuild) { m_rebuild = false; RebuildWorld(); }
        if (m_monitorsDirty)
        {
            m_monitorsDirty = false;
            std::vector<GpuMonitor> mons;
            for (const ShoalRect& m : m_monitors) mons.push_back({ m.left, m.top, m.right, m.bottom });
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_monitorBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GpuMonitor) * mons.size(), mons.data(), GL_DYNAMIC_DRAW);
        }
        if (m_rocksDirty)
        {
            // Rocks first (drawn), then desktop obstacles (not drawn).
            m_rocksDirty = false;
            std::vector<ShoalObstacle> all;
            for (const Rock& r : m_rocks)
            {
                ShoalObstacle g = {};
                g.x = r.x; g.y = r.y; g.radius = r.radius; g.pointCount = (float)r.pointCount;
                for (int p = 0; p < r.pointCount; ++p) { g.points[p * 2] = (float)r.points[p].x; g.points[p * 2 + 1] = (float)r.points[p].y; }
                all.push_back(g);
            }
            all.insert(all.end(), m_static.begin(), m_static.end());
            all.insert(all.end(), m_dynamic.begin(), m_dynamic.end());
            m_rockTotal = all.size();
            if (all.empty()) all.push_back(ShoalObstacle{});
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_rockBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(ShoalObstacle) * all.size(), all.data(), GL_DYNAMIC_DRAW);
        }
        for (Picture& pic : m_pictures)
        {
            if (!pic.dirty) continue;
            pic.dirty = false;
            if (pic.pixels.empty())
            {
                if (pic.texture) glDeleteTextures(1, &pic.texture);
                pic.texture = 0;
                continue;
            }
            if (!pic.texture) glGenTextures(1, &pic.texture);
            glBindTexture(GL_TEXTURE_2D, pic.texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pic.width, pic.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pic.pixels.data());
            glGenerateMipmap(GL_TEXTURE_2D); // a large picture shrunk to the screen stays smooth
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            std::vector<uint8_t>().swap(pic.pixels);
        }
    }

    void Step(float dt)
    {
        Sync();
        if (!m_glReady || m_boids.empty()) return;
        m_simTime += dt;
        GpuSimulationConstants c = { dt, (float)m_s.speed, (float)m_boids.size(), (float)m_s.randomness, (float)m_monitors.size(),
            m_simTime, (float)m_rockTotal, m_s.restrictToMonitor ? 1.0f : 0.0f, (float)m_boidLength, (float)m_s.speedVariability,
            (float)m_s.colourCount, m_s.colourCount > 1 && m_s.colourSort ? 1.0f : 0.0f, m_s.colourSort && m_s.colourAvoid ? 1.0f : 0.0f,
            { 0, 0, 0 } };
        glBindBuffer(GL_UNIFORM_BUFFER, m_simUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof c, &c);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_simUbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_boidBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_monitorBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_rockBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_spineBuffer);
        GLuint groups = ((GLuint)m_boids.size() + 63) / 64;
        glUseProgram(m_simProgram);
        glDispatchCompute(groups, 1, 1);
        if (m_rockTotal > 0)
        {
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            glUseProgram(m_rockProgram);
            glDispatchCompute(groups, 1, 1);
        }
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
        glUseProgram(0);

        ++m_frames;
        auto now = std::chrono::steady_clock::now();
        if (!m_fpsStarted) { m_fpsStarted = true; m_fpsStart = now; }
        if (now - m_fpsStart >= std::chrono::seconds(1)) { m_fps = m_frames; m_frames = 0; m_fpsStart = now; }
    }

    float ClockDelta()
    {
        auto now = std::chrono::steady_clock::now();
        float dt = m_haveLast ? std::chrono::duration<float>(now - m_last).count() : 1.0f / 60.0f;
        m_last = now;
        m_haveLast = true;
        if (dt > 0.25f) dt = 1.0f / 60.0f; // resuming after a pause: carry on smoothly instead of jumping
        return std::max(0.0001f, std::min(dt, 0.1f));
    }

    // The monitor area a view mostly shows (for placing the frame-rate counter).
    ShoalRect ViewArea(const ShoalRect& v) const
    {
        for (const ShoalRect& m : m_monitors)
        {
            ShoalRect i = { std::max(m.left, v.left), std::max(m.top, v.top), std::min(m.right, v.right), std::min(m.bottom, v.bottom) };
            if (!i.empty()) return i;
        }
        return v;
    }

    void BuildOverlay(const ShoalView& view)
    {
        const ShoalRect& b = view.bounds;
        const float width = b.width(), height = b.height();
        m_verts.clear();
        auto add = [&](float x, float y, float shade)
        { m_verts.push_back({ (x - b.left) / width * 2.0f - 1.0f, 1.0f - (y - b.top) / height * 2.0f, shade, shade, shade, 1.0f }); };
        for (const Rock& r : m_rocks)
        {
            for (int p = 0; p < r.pointCount; ++p)
            {
                const Pt& a = r.points[p];
                const Pt& c = r.points[(p + 1) % r.pointCount];
                float x1 = r.x + a.x, y1 = r.y + a.y, x2 = r.x + c.x, y2 = r.y + c.y, dx = x2 - x1, dy = y2 - y1;
                float el = Len(dx, dy);
                if (el <= 0.001f) continue;
                float ox = -dy / el * 2.0f, oy = dx / el * 2.0f;
                add(x1 - ox, y1 - oy, 0.45f); add(x2 - ox, y2 - oy, 0.45f); add(x1 + ox, y1 + oy, 0.45f);
                add(x2 - ox, y2 - oy, 0.45f); add(x2 + ox, y2 + oy, 0.45f); add(x1 + ox, y1 + oy, 0.45f);
            }
        }
        if (m_s.showFps)
        {
            int fps = view.fps >= 0 ? view.fps : m_fps.load();
            int digits = fps >= 1000 ? 4 : fps >= 100 ? 3 : fps >= 10 ? 2 : 1;
            const ShoalRect area = ViewArea(b);
            const float t = 2.0f, dw = 10.0f, dh = 18.0f;
            float startX = area.right - 18.0f * digits - 12.0f, y = view.fpsAtBottom ? area.bottom - dh - 12.0f : area.top + 10.0f;
            static const bool seg[10][7] = {
                { 1, 1, 1, 1, 1, 1, 0 }, { 0, 1, 1, 0, 0, 0, 0 }, { 1, 1, 0, 1, 1, 0, 1 }, { 1, 1, 1, 1, 0, 0, 1 }, { 0, 1, 1, 0, 0, 1, 1 },
                { 1, 0, 1, 1, 0, 1, 1 }, { 1, 0, 1, 1, 1, 1, 1 }, { 1, 1, 1, 0, 0, 0, 0 }, { 1, 1, 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 0, 1, 1 } };
            auto quad = [&](float x, float qy, float qw, float qh)
            { add(x, qy, 1); add(x + qw, qy, 1); add(x, qy + qh, 1); add(x + qw, qy, 1); add(x + qw, qy + qh, 1); add(x, qy + qh, 1); };
            for (int di = 0; di < digits; ++di)
            {
                int div = 1;
                for (int i = 1; i < digits - di; ++i) div *= 10;
                int d = (fps / div) % 10;
                float x = startX + di * 18.0f;
                if (seg[d][0]) quad(x + 1, y, dw - 2, t);
                if (seg[d][1]) quad(x + dw - t, y + 1, t, dh * 0.5f - 1);
                if (seg[d][2]) quad(x + dw - t, y + dh * 0.5f, t, dh * 0.5f - 1);
                if (seg[d][3]) quad(x + 1, y + dh - t, dw - 2, t);
                if (seg[d][4]) quad(x, y + dh * 0.5f, t, dh * 0.5f - 1);
                if (seg[d][5]) quad(x, y + 1, t, dh * 0.5f - 1);
                if (seg[d][6]) quad(x + 1, y + dh * 0.5f - t * 0.5f, dw - 2, t);
            }
        }
        if (m_verts.size() > kMaxOverlayVertices) m_verts.resize(kMaxOverlayVertices / 3 * 3);
    }

    void DrawPicture(const ShoalView& view, const Picture& pic)
    {
        // Cover the view without stretching: show the middle of the picture at the view's aspect ratio.
        const float viewAspect = view.bounds.width() / std::max(1.0f, view.bounds.height());
        const float picAspect = (float)pic.width / std::max(1, pic.height);
        float sx = 1, sy = 1, ox, oy;
        if (pic.hasArea && !pic.area.empty())
        {
            // The picture lies on a world rectangle: show the part under this view.
            const ShoalRect& a = pic.area;
            sx = view.bounds.width() / a.width();
            sy = view.bounds.height() / a.height();
            ox = (view.bounds.left - a.left) / a.width();
            oy = (view.bounds.top - a.top) / a.height();
        }
        else
        {
            if (viewAspect > picAspect) sy = picAspect / viewAspect; else sx = viewAspect / picAspect;
            ox = (1 - sx) * 0.5f;
            oy = (1 - sy) * 0.5f;
        }
        glUseProgram(m_bgProgram);
        glUniform4f(m_bgUvLoc, ox, oy, sx, sy);
        glUniform1f(m_bgDimLoc, std::max(0, std::min(100, m_s.pictureDim)) / 100.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, pic.texture);
        glBindVertexArray(m_emptyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void Draw(const ShoalView& view)
    {
        Sync();
        glViewport(0, 0, view.pixelWidth, view.pixelHeight);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        const uint32_t bg = m_s.background;
        if (m_s.transparent) glClearColor(0, 0, 0, 0); // premultiplied: the host's picture underneath shows through
        else glClearColor(((bg >> 16) & 255) / 255.0f, ((bg >> 8) & 255) / 255.0f, (bg & 255) / 255.0f, 1.0f);
        glClearDepthf(1.0f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (!m_glReady || view.bounds.empty() || view.pixelWidth <= 0 || view.pixelHeight <= 0) return;
        if (m_s.picture && !m_s.transparent && view.picture >= 0 && view.picture < (int)m_pictures.size() && m_pictures[view.picture].texture)
            DrawPicture(view, m_pictures[view.picture]);
        if (m_boids.empty()) return;

        const bool depth = m_s.depthEffect;
        GpuRenderConstants rc = {};
        rc.left = view.bounds.left; rc.top = view.bounds.top; rc.right = view.bounds.right; rc.bottom = view.bounds.bottom;
        rc.length = (float)m_boidLength;
        rc.monitorCount = (float)m_monitors.size();
        rc.restrictToMonitor = m_s.restrictToMonitor ? 1.0f : 0.0f;
        rc.tailRatio = m_s.tailPercent / 100.0f;
        rc.colourCount = (float)m_s.colourCount;
        rc.tailShade = m_s.tailShade / 100.0f;
        rc.depthEffect = depth ? 1.0f : 0.0f;
        for (int i = 0; i < m_s.colourCount && i < 10; ++i)
        {
            uint32_t c = m_s.colours[i];
            rc.colours[i][0] = ((c >> 16) & 255) / 255.0f;
            rc.colours[i][1] = ((c >> 8) & 255) / 255.0f;
            rc.colours[i][2] = (c & 255) / 255.0f;
        }
        glBindBuffer(GL_UNIFORM_BUFFER, m_renderUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof rc, &rc);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_renderUbo);
        GpuSpecies sp = {};
        for (int i = 0; i < kShoalSpeciesCount; ++i)
            if (m_s.speciesMask & (1u << i)) sp.list[sp.info[0]++] = (uint32_t)i;
        if (sp.info[0] == 0) sp.list[sp.info[0]++] = 0; // nothing chosen: the classic fish
        sp.info[1] = m_s.speciesMode == 1 ? 1u : 0u;
        glBindBuffer(GL_UNIFORM_BUFFER, m_speciesUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof sp, &sp);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_speciesUbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_boidBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_monitorBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_spineBuffer);

        glBindVertexArray(m_emptyVao);
        glUseProgram(m_fishProgram);
        GLsizei count = (GLsizei)m_boids.size();
        if (depth) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); }
        glUniform1i(m_fishShadowLoc, 0);
        glDrawElementsInstanced(GL_TRIANGLES, m_fishIndexCount, GL_UNSIGNED_SHORT, nullptr, count);
        if (depth)
        {
            glDepthMask(GL_FALSE);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
            glStencilMask(0xFF);
            glEnable(GL_BLEND);
            if (m_s.transparent)
            {
                // Over a see-through background the shadow also has to add coverage: alpha += 0.8 (premultiplied black at
                // 80%), so the picture underneath ends up at the same 20% brightness that the opaque path multiplies by.
                glBlendColor(0, 0, 0, 0.8f);
                glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_CONSTANT_ALPHA, GL_ONE);
            }
            else
                glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
            glUniform1i(m_fishShadowLoc, 1);
            glDrawElementsInstanced(GL_TRIANGLES, m_fishIndexCount, GL_UNSIGNED_SHORT, nullptr, count);
            glDisable(GL_BLEND);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
        }

        BuildOverlay(view);
        if (!m_verts.empty())
        {
            glBindVertexArray(m_overlayVao);
            glBindBuffer(GL_ARRAY_BUFFER, m_overlayVbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * m_verts.size(), m_verts.data());
            glUseProgram(m_overlayProgram);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m_verts.size());
        }
        glBindVertexArray(0);
        glUseProgram(0);
    }

    std::string m_error;
    ShoalSettings m_s;
    bool m_haveSettings = false;
    std::vector<ShoalRect> m_monitors;
    float m_referenceHeight = 0;
    bool m_rebuild = true, m_monitorsDirty = true, m_rocksDirty = true, m_glReady = false;
    std::mt19937 m_rng{ std::random_device{}() };
    int m_boidLength = 26;
    std::vector<Boid> m_boids;
    std::vector<Rock> m_rocks;
    std::vector<ShoalObstacle> m_static, m_dynamic;
    int m_staticGen = -1;
    size_t m_rockTotal = 0;
    std::vector<Vertex> m_verts;
    float m_simTime = 0.0f;
    std::chrono::steady_clock::time_point m_last, m_fpsStart;
    bool m_haveLast = false, m_fpsStarted = false;
    std::atomic<int> m_fps{ 0 }; // read by other threads (the wallpaper's offload)
    int m_frames = 0;

    std::vector<Picture> m_pictures;

    GLuint m_simProgram = 0, m_rockProgram = 0, m_fishProgram = 0, m_overlayProgram = 0, m_bgProgram = 0;
    GLuint m_boidBuffer = 0, m_monitorBuffer = 0, m_rockBuffer = 0, m_simUbo = 0, m_renderUbo = 0, m_speciesUbo = 0, m_overlayVbo = 0, m_overlayVao = 0,
           m_emptyVao = 0;
    GLuint m_fishIndexBuffer = 0, m_spineBuffer = 0;
    GLsizei m_fishIndexCount = 0;
    GLint m_fishShadowLoc = -1, m_bgUvLoc = -1, m_bgDimLoc = -1;
};

// ------------------------------------------------------------------------------------------------ public API
ShoalEngine::ShoalEngine() : d(new ShoalEngineState) {}
ShoalEngine::~ShoalEngine() = default;

bool ShoalEngine::init(std::string* error)
{
    if (!ShoalLoadGL())
        d->Fail("couldn't load the OpenGL 4.3 functions");
    else
    {
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major * 10 + minor < 43)
            d->Fail("Shoal needs OpenGL 4.3 (got " + std::to_string(major) + "." + std::to_string(minor) + ")");
        else
        {
            d->m_glReady = d->InitGpu();
            g_log(std::string("OpenGL ") + (const char*)glGetString(GL_VERSION) + " on " + (const char*)glGetString(GL_RENDERER));
        }
    }
    if (!d->m_glReady && error) *error = d->m_error;
    return d->m_glReady;
}

void ShoalEngine::setSettings(const ShoalSettings& s) { d->SetSettings(s); }
void ShoalEngine::setMonitors(const std::vector<ShoalRect>& m, float referenceHeight) { d->SetMonitors(m, referenceHeight); }
void ShoalEngine::rebuild() { d->m_rebuild = true; }

void ShoalEngine::setObstacles(const std::vector<ShoalObstacle>& obstacles, int generation)
{
    if (generation == d->m_staticGen) return;
    d->m_staticGen = generation;
    d->m_static = obstacles;
    d->m_rocksDirty = true;
}

void ShoalEngine::setDynamicObstacles(const std::vector<ShoalObstacle>& obstacles)
{
    if (obstacles.empty() && d->m_dynamic.empty()) return;
    d->m_dynamic = obstacles;
    d->m_rocksDirty = true;
}

void ShoalEngine::setPicture(const uint8_t* rgba, int width, int height, const ShoalRect* area, int slot)
{
    if (slot < 0) return;
    if ((int)d->m_pictures.size() <= slot) d->m_pictures.resize(slot + 1);
    ShoalEngineState::Picture& pic = d->m_pictures[slot];
    pic.hasArea = area != nullptr;
    if (area) pic.area = *area;
    if (rgba && width > 0 && height > 0)
    {
        pic.pixels.assign(rgba, rgba + (size_t)width * height * 4);
        pic.width = width;
        pic.height = height;
    }
    else
        pic.pixels.clear();
    pic.dirty = true;
}

float ShoalEngine::clockDelta() { return d->ClockDelta(); }
void ShoalEngine::step(float dt) { d->Step(dt); }
void ShoalEngine::draw(const ShoalView& view) { d->Draw(view); }
bool ShoalEngine::fishState(int index, float out[4])
{
    if (!d->m_glReady || index < 0 || index >= (int)d->m_boids.size()) return false;
    GpuBoid b;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, d->m_boidBuffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(GpuBoid) * index, sizeof b, &b);
    out[0] = b.x; out[1] = b.y; out[2] = b.heading; out[3] = b.currentSpeed;
    return true;
}

void ShoalEngine::readPixels(int width, int height, uint8_t* rgba)
{
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

int ShoalEngine::fishCount() const { return (int)d->m_boids.size(); }
int ShoalEngine::rockCount() const { return (int)d->m_rocks.size(); }
int ShoalEngine::fps() const { return d->m_fps; }
float ShoalEngine::boidLength() const { return (float)d->m_boidLength; }
const std::string& ShoalEngine::error() const { return d->m_error; }
void ShoalEngine::setLogger(std::function<void(const std::string&)> log) { g_log = std::move(log); }
