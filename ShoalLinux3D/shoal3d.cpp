// Shoal 3D (experimental): the Linux port with a third dimension. Fish are spheroids in a box whose depth equals its width,
// seen through a perspective camera with depth fog. SDL2 + OpenGL 4.3 compute shaders.
#include <SDL.h>
#include <GL/glew.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "shaders_gen.h"

struct Rect { int left, top, right, bottom; };
struct Pt { int x, y; };

struct Settings
{
    int speed = 42;
    int boidCount = 20;
    int randomness = 0;
    int boidLength = 26;
    bool showFps = false;
    int frameRate = 0;
    int rockCount = 5;
    bool restrictToMonitor = false;
    bool vsync = true;
    int frameRateMode = 1;
    int sizeTenths = 24;
    int speedVariability = 5;
    int fishCoverage = 50;
    int tailPercent = 33;
    int colourCount = 5;
    unsigned colours[10] = { 0xdce6f0, 0xffcd28, 0xff781e, 0x286ee6, 0x28c8be }; // 0xRRGGBB
    int tailShade = -40;
    bool colourSort = false;
    bool colourAvoid = false;
    int sizeVariation = 100;
    bool sizeBell = true;
    bool depthEffect = true;
    int cameraPct = 100;   // camera distance in front of the screen plane, as a percent of the tank depth
    int depthPct = 100;    // tank depth as a percent of the tank width
};

static Settings g_settings;
static bool g_boidCountOverridden = false;
static bool g_windowed = false;
static int g_runSeconds = 0;
static const float kReferenceBoidLength = 20.0f;

struct GpuBoid
{
    float px, py, pz;
    float fx, fy, fz;
    float wanderYaw, wanderPitch, wanderYawT, wanderPitchT;
    float independentUntil, nextTurnChange;
    float bx, by, bz;
    float speed, cruise, reserved, yawRate, phase, thrust, len, pad;
};
static_assert(sizeof(GpuBoid) == 92, "GpuBoid must match the 23-float struct in the shaders");
struct GpuSimulationConstants
{
    float deltaSeconds, speed, boidCount, randomness, timeSeconds, baseLength, speedVariability, colourCount;
    float colourBias, colourAvoid, boxL, boxT, boxR, boxB, boxD, radiusScale;
};
struct GpuRenderConstants
{
    float win[4];
    float box[4];
    float D, camDist, cx, cy;
    float tailRatio, tailShade, colourCount, time;
    float colours[10][4];
};
struct Vertex { float x, y, r, g, b, a; };

struct Boid { float x, y, z, fx, fy, fz, length; };
struct Box3 { float left, top, right, bottom, depth, camDist, cx, cy; };
static Box3 g_box;

static std::vector<Rect> g_monitors;
static std::vector<Boid> g_boids;
static std::mt19937 g_rng{ std::random_device{}() };

static float RandomFloat(float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng); }

static int BoidLengthForSize(int tenths, int screenHeight) { return std::max(4, (int)std::lround(screenHeight * tenths / 1000.0)); }

static const double kMinFishCoverage = 0.00140625;
static const double kMaxFishCoverage = 302500.0 / 4147200.0;
static double FishCoverageFraction(int p) { return kMinFishCoverage * std::pow(kMaxFishCoverage / kMinFishCoverage, p / 100.0); }
static double SizeAreaFactor(int v, bool bell)
{
    if (v <= 100) return 1.0;
    double k = 2.0 * std::log(v / 100.0) / (bell ? 3.0 : 1.0);
    double f = std::sinh(k) / k;
    return bell ? f * f * f : f;
}
static int BoidCountFor(int position, int length, double freeArea, double areaFactor)
{
    double count = FishCoverageFraction(position) * freeArea / ((double)length * length * 0.25 * areaFactor);
    return count < 2 ? 2 : (count > 10000 ? 10000 : (int)std::lround(count));
}
// ---------------------------------------------------------------- settings
static std::string ConfigPath()
{
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? xdg : (std::string(home ? home : ".") + "/.config");
    return base + "/shoal/settings.conf";
}

static std::string Lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

static bool ApplySetting(const std::string& rawName, const std::string& value)
{
    std::string n = Lower(rawName);
    Settings& s = g_settings;
    auto I = [&](int lo, int hi, int* t) { *t = std::max(lo, std::min(hi, (int)std::strtol(value.c_str(), nullptr, 10))); return true; };
    auto B = [&](bool* t) { *t = std::strtol(value.c_str(), nullptr, 10) != 0; return true; };
    if (n == "speed") return I(1, 1000, &s.speed);
    if (n == "randomness") return I(0, 100, &s.randomness);
    if (n == "speedvar" || n == "speedvariability") return I(0, 10, &s.speedVariability);
    if (n == "coverage" || n == "fishcoverage") return I(0, 100, &s.fishCoverage);
    if (n == "boids") { g_boidCountOverridden = true; return I(2, 10000, &s.boidCount); }
    if (n == "rocks" || n == "rockcount") return I(0, 10, &s.rockCount);
    if (n == "tail" || n == "tailpercent") return I(10, 100, &s.tailPercent);
    if (n == "shade" || n == "tailshade") return I(-100, 100, &s.tailShade);
    if (n == "coloursort") return B(&s.colourSort);
    if (n == "colouravoid") return B(&s.colourAvoid);
    if (n == "size" || n == "sizepercent") { s.sizeTenths = std::max(10, std::min(100, (int)std::lround(std::atof(value.c_str()) * 10.0))); return true; }
    if (n == "sizevar" || n == "sizevariation") return I(100, 300, &s.sizeVariation);
    if (n == "sizebell") return B(&s.sizeBell);
    if (n == "depth" || n == "deptheffect") return B(&s.depthEffect);
    if (n == "restrict" || n == "restricttomonitor") return B(&s.restrictToMonitor);
    if (n == "showfps") return B(&s.showFps);
    if (n == "vsync") return B(&s.vsync);
    if (n == "fpsmode" || n == "frameratemode") return I(0, 5, &s.frameRateMode);
    if (n == "framerate") { s.vsync = false; return I(1, 100000, &s.frameRate); }
    if (n == "camera") return I(30, 300, &s.cameraPct);
    if (n == "depthscale") return I(25, 400, &s.depthPct);
    if (n == "colourcount") return I(1, 10, &s.colourCount);
    if (n.size() == 7 && n.compare(0, 6, "colour") == 0 && std::isdigit((unsigned char)n[6]))
    {
        std::string v = value;
        if (!v.empty() && v[0] == '#') v = v.substr(1);
        s.colours[n[6] - '0'] = (unsigned)std::strtoul(v.c_str(), nullptr, 16) & 0xFFFFFF;
        return true;
    }
    return false;
}

static void WriteDefaultConfig(const std::string& path)
{
    std::string dir = path.substr(0, path.rfind('/'));
    std::string cmd = "mkdir -p '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) return;
    std::ofstream f(path);
    if (!f) return;
    const Settings& s = g_settings;
    f << "# Shoal settings. Same names and ranges as the Windows version's registry values (see README.md).\n"
      << "Speed=" << s.speed << "\nSpeedVariability=" << s.speedVariability << "\nFishCoverage=" << s.fishCoverage
      << "\nRandomness=" << s.randomness << "\nSize=" << s.sizeTenths / 10.0 << "   # percent of screen height\n"
      << "SizeVariation=" << s.sizeVariation << "\nSizeBell=" << s.sizeBell << "\nTailPercent=" << s.tailPercent
      << "\nTailShade=" << s.tailShade << "\nDepthEffect=" << s.depthEffect << "\nRockCount=" << s.rockCount
      << "\nRestrictToMonitor=" << s.restrictToMonitor << "\nShowFps=" << s.showFps << "\nVSync=" << s.vsync
      << "\nFrameRateMode=" << s.frameRateMode << "   # when VSync=0: 0=1/2x 1=1x 2=2x 3=3x 4=4x 5=unlimited\n"
      << "ColourSort=" << s.colourSort << "\nColourAvoid=" << s.colourAvoid << "\nColourCount=" << s.colourCount << "\n";
    char buf[32];
    for (int i = 0; i < s.colourCount; ++i) { std::snprintf(buf, sizeof buf, "Colour%d=#%06x\n", i, s.colours[i]); f << buf; }
}

static void LoadSettings()
{
    std::string path = ConfigPath();
    std::ifstream f(path);
    if (!f) { WriteDefaultConfig(path); return; }
    std::string line;
    while (std::getline(f, line))
    {
        size_t hash = line.find('#');
        if (hash == 0) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq), value = line.substr(eq + 1);
        size_t c = value.find("   #");
        if (c != std::string::npos) value = value.substr(0, c);
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
        while (!value.empty() && std::isspace((unsigned char)value.front())) value.erase(value.begin());
        if (name == "Size" || name == "size") { g_boidCountOverridden = g_boidCountOverridden; }
        ApplySetting(name, value);
    }
}

// ---------------------------------------------------------------- world setup
// ---------------------------------------------------------------- world
// The tank is the bounding box of all monitors; its depth is the box width (times DepthScale). The camera sits in front of the
// z = 0 plane so that plane maps to the screen one to one, and everything deeper is drawn smaller and darker.
static void CreateWorld()
{
    int l = g_monitors[0].left, t = g_monitors[0].top, r = g_monitors[0].right, b = g_monitors[0].bottom;
    for (const Rect& m : g_monitors) { l = std::min(l, m.left); t = std::min(t, m.top); r = std::max(r, m.right); b = std::max(b, m.bottom); }
    g_box.left = (float)l; g_box.top = (float)t; g_box.right = (float)r; g_box.bottom = (float)b;
    g_box.depth = (float)(r - l) * g_settings.depthPct / 100.0f;
    g_box.camDist = g_box.depth * g_settings.cameraPct / 100.0f;
    g_box.cx = 0.5f * (float)(l + r); g_box.cy = 0.5f * (float)(t + b);
}

static void CreateBoids()
{
    g_boids.clear();
    if (!g_boidCountOverridden)
    {
        double area = (double)(g_box.right - g_box.left) * (g_box.bottom - g_box.top);
        g_settings.boidCount = BoidCountFor(g_settings.fishCoverage, g_settings.boidLength, area,
            SizeAreaFactor(g_settings.sizeVariation, g_settings.sizeBell));
    }
    for (int i = 0; i < g_settings.boidCount; ++i)
    {
        Boid b;
        float u = RandomFloat(-1.0f, 1.0f);
        if (g_settings.sizeBell) u = (u + RandomFloat(-1.0f, 1.0f) + RandomFloat(-1.0f, 1.0f)) / 3.0f;
        b.length = std::max(4.0f, (float)g_settings.boidLength * std::pow(g_settings.sizeVariation / 100.0f, u));
        float inset = b.length;
        b.x = RandomFloat(g_box.left + inset, g_box.right - inset);
        b.y = RandomFloat(g_box.top + inset, g_box.bottom - inset);
        b.z = RandomFloat(inset, g_box.depth - inset);
        float yaw = RandomFloat(0.0f, 2.0f * 3.14159265f), pitch = RandomFloat(-0.3f, 0.3f);
        b.fx = std::cos(yaw) * std::cos(pitch); b.fy = std::sin(pitch); b.fz = std::sin(yaw) * std::cos(pitch);
        g_boids.push_back(b);
    }
}

// ---------------------------------------------------------------- GL
struct ShaderBlob { const unsigned char* data; unsigned int len; };

static GLuint CompileStage(GLenum type, const unsigned char* data, unsigned len, const char* name)
{
    GLuint s = glCreateShader(type);
    const char* src = (const char*)data;
    GLint l = (GLint)len;
    glShaderSource(s, 1, &src, &l);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "Shader %s failed:\n%s\n", name, log);
        return 0;
    }
    return s;
}

static GLuint LinkProgram(std::initializer_list<GLuint> stages, const char* name)
{
    for (GLuint s : stages) if (!s) return 0;
    GLuint p = glCreateProgram();
    for (GLuint s : stages) glAttachShader(p, s);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "Program %s failed:\n%s\n", name, log);
        return 0;
    }
    return p;
}

static GLuint g_simProgram, g_fishProgram, g_bgProgram, g_overlayProgram;
static GLuint g_boidBuffer, g_simUbo, g_renderUbo, g_overlayVbo, g_overlayVao, g_emptyVao;
static GLint g_bgViewportLoc;
static float g_gpuSimulationTime = 0.0f;
static const size_t kMaxOverlayVertices = 2048;
static const float kRadiusScale = 2.2f;   // neighbour reach is larger in 3D, where the same fish are spread through much more volume

#define SHADER(arr) arr, arr##_len
static bool InitGpu()
{
    g_simProgram = LinkProgram({ CompileStage(GL_COMPUTE_SHADER, SHADER(shaders_sim3d_comp), "sim3d.comp") }, "sim3d");
    g_fishProgram = LinkProgram({ CompileStage(GL_VERTEX_SHADER, SHADER(shaders_fish3d_vert), "fish3d.vert"),
                                  CompileStage(GL_FRAGMENT_SHADER, SHADER(shaders_fish3d_frag), "fish3d.frag") }, "fish3d");
    g_bgProgram = LinkProgram({ CompileStage(GL_VERTEX_SHADER, SHADER(shaders_bg3d_vert), "bg3d.vert"),
                                CompileStage(GL_FRAGMENT_SHADER, SHADER(shaders_bg3d_frag), "bg3d.frag") }, "bg3d");
    g_overlayProgram = LinkProgram({ CompileStage(GL_VERTEX_SHADER, SHADER(shaders_overlay_vert), "overlay.vert"),
                                     CompileStage(GL_FRAGMENT_SHADER, SHADER(shaders_overlay_frag), "overlay.frag") }, "overlay");
    if (!g_simProgram || !g_fishProgram || !g_bgProgram || !g_overlayProgram) return false;
    g_bgViewportLoc = glGetUniformLocation(g_bgProgram, "uViewport");

    std::vector<GpuBoid> init;
    for (size_t i = 0; i < g_boids.size(); ++i)
    {
        const Boid& b = g_boids[i];
        float sp = (float)g_settings.speed;
        GpuBoid g = {};
        g.px = b.x; g.py = b.y; g.pz = b.z;
        g.fx = b.fx; g.fy = b.fy; g.fz = b.fz;
        g.wanderYaw = RandomFloat(-0.35f, 0.35f); g.wanderPitch = RandomFloat(-0.15f, 0.15f);
        g.wanderYawT = RandomFloat(-0.35f, 0.35f); g.wanderPitchT = RandomFloat(-0.15f, 0.15f);
        g.bx = b.fx; g.by = b.fy; g.bz = b.fz;
        g.speed = sp; g.cruise = sp; g.thrust = 1.0f; g.len = b.length;
        g.phase = std::fmod((float)i * 2.39996323f, 6.2831853f);
        init.push_back(g);
    }
    glGenBuffers(1, &g_boidBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_boidBuffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GpuBoid) * init.size(), init.data(), GL_DYNAMIC_DRAW);

    glGenBuffers(1, &g_simUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_simUbo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(GpuSimulationConstants), nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &g_renderUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_renderUbo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(GpuRenderConstants), nullptr, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &g_emptyVao);
    glGenVertexArrays(1, &g_overlayVao);
    glGenBuffers(1, &g_overlayVbo);
    glBindVertexArray(g_overlayVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_overlayVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * kMaxOverlayVertices, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)8);
    glBindVertexArray(0);
    return true;
}

static void StepSimulation(float dt)
{
    g_gpuSimulationTime += dt;
    GpuSimulationConstants c = { dt, (float)g_settings.speed, (float)g_boids.size(), (float)g_settings.randomness, g_gpuSimulationTime,
        (float)g_settings.boidLength, (float)g_settings.speedVariability, (float)g_settings.colourCount,
        g_settings.colourCount > 1 && g_settings.colourSort ? 1.0f : 0.0f, g_settings.colourSort && g_settings.colourAvoid ? 1.0f : 0.0f,
        g_box.left, g_box.top, g_box.right, g_box.bottom, g_box.depth, kRadiusScale };
    glBindBuffer(GL_UNIFORM_BUFFER, g_simUbo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof c, &c);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_simUbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_boidBuffer);
    glUseProgram(g_simProgram);
    glDispatchCompute(((GLuint)g_boids.size() + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

struct Window
{
    SDL_Window* window = nullptr;
    Rect bounds{};
    int pixelW = 1, pixelH = 1;
    double refreshHz = 60.0;
    int fps = 0, frames = 0;
    Uint32 fpsTick = 0;
};

static void BuildOverlay(const Window& w, std::vector<Vertex>& v)
{
    float width = (float)(w.bounds.right - w.bounds.left), height = (float)(w.bounds.bottom - w.bounds.top);
    v.clear();
    auto add = [&](float x, float y, float shade)
    {
        v.push_back({ (x - w.bounds.left) / width * 2.0f - 1.0f, 1.0f - (y - w.bounds.top) / height * 2.0f, shade, shade, shade, 1.0f });
    };
    if (g_settings.showFps)
    {
        int fps = w.fps;
        int digits = fps >= 1000 ? 4 : fps >= 100 ? 3 : fps >= 10 ? 2 : 1;
        float startX = width - 18.0f * digits - 12.0f;
        const float t = 2.0f, dw = 10.0f, dh = 18.0f;
        static const bool seg[10][7] = {
            { 1, 1, 1, 1, 1, 1, 0 }, { 0, 1, 1, 0, 0, 0, 0 }, { 1, 1, 0, 1, 1, 0, 1 }, { 1, 1, 1, 1, 0, 0, 1 }, { 0, 1, 1, 0, 0, 1, 1 },
            { 1, 0, 1, 1, 0, 1, 1 }, { 1, 0, 1, 1, 1, 1, 1 }, { 1, 1, 1, 0, 0, 0, 0 }, { 1, 1, 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 0, 1, 1 } };
        auto quad = [&](float x, float y, float qw, float qh)
        {
            auto f = [&](float px, float py) { add(px + w.bounds.left, py + w.bounds.top, 1.0f); };
            f(x, y); f(x + qw, y); f(x, y + qh); f(x + qw, y); f(x + qw, y + qh); f(x, y + qh);
        };
        for (int di = 0; di < digits; ++di)
        {
            int div = 1;
            for (int i = 1; i < digits - di; ++i) div *= 10;
            int d = (fps / div) % 10;
            float x = startX + di * 18.0f, y = 10.0f;
            if (seg[d][0]) quad(x + 1, y, dw - 2, t);
            if (seg[d][1]) quad(x + dw - t, y + 1, t, dh * 0.5f - 1);
            if (seg[d][2]) quad(x + dw - t, y + dh * 0.5f, t, dh * 0.5f - 1);
            if (seg[d][3]) quad(x + 1, y + dh - t, dw - 2, t);
            if (seg[d][4]) quad(x, y + dh * 0.5f, t, dh * 0.5f - 1);
            if (seg[d][5]) quad(x, y + 1, t, dh * 0.5f - 1);
            if (seg[d][6]) quad(x + 1, y + dh * 0.5f - t * 0.5f, dw - 2, t);
        }
    }
    if (v.size() > kMaxOverlayVertices) v.resize(kMaxOverlayVertices / 3 * 3);
}

static void RenderWindow(Window& w)
{
    Uint32 now = SDL_GetTicks();
    if (w.fpsTick == 0) w.fpsTick = now;
    if (now - w.fpsTick >= 1000) { w.fps = w.frames; w.frames = 0; w.fpsTick = now; }

    SDL_GL_GetDrawableSize(w.window, &w.pixelW, &w.pixelH);
    glViewport(0, 0, w.pixelW, w.pixelH);
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    GpuRenderConstants rc = {};
    rc.win[0] = (float)w.bounds.left; rc.win[1] = (float)w.bounds.top; rc.win[2] = (float)w.bounds.right; rc.win[3] = (float)w.bounds.bottom;
    rc.box[0] = g_box.left; rc.box[1] = g_box.top; rc.box[2] = g_box.right; rc.box[3] = g_box.bottom;
    rc.D = g_box.depth; rc.camDist = g_box.camDist; rc.cx = g_box.cx; rc.cy = g_box.cy;
    rc.tailRatio = g_settings.tailPercent / 100.0f;
    rc.tailShade = g_settings.tailShade / 100.0f;
    rc.colourCount = (float)g_settings.colourCount;
    rc.time = g_gpuSimulationTime;
    for (int i = 0; i < g_settings.colourCount; ++i)
    {
        unsigned c = g_settings.colours[i];
        rc.colours[i][0] = ((c >> 16) & 255) / 255.0f;
        rc.colours[i][1] = ((c >> 8) & 255) / 255.0f;
        rc.colours[i][2] = (c & 255) / 255.0f;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, g_renderUbo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof rc, &rc);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_renderUbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_boidBuffer);

    glBindVertexArray(g_emptyVao);
    glUseProgram(g_bgProgram);
    glUniform2f(g_bgViewportLoc, (float)w.pixelW, (float)w.pixelH);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(g_fishProgram);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)g_boids.size() * 2);     // two spheroids per fish: body and tail
    glDisable(GL_DEPTH_TEST);

    static std::vector<Vertex> verts;
    BuildOverlay(w, verts);
    if (!verts.empty())
    {
        glBindVertexArray(g_overlayVao);
        glBindBuffer(GL_ARRAY_BUFFER, g_overlayVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * verts.size(), verts.data());
        glUseProgram(g_overlayProgram);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());
    }
    SDL_GL_SwapWindow(w.window);
    ++w.frames;
}

// ---------------------------------------------------------------- main
static void ParseArgs(int argc, char** argv, char* mode)
{
    *mode = 's';
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        size_t k = 0;
        while (k < a.size() && (a[k] == '/' || a[k] == '-')) ++k;
        std::string body = a.substr(k), name = body, value;
        size_t sep = body.find_first_of(":=");
        if (sep != std::string::npos) { name = body.substr(0, sep); value = body.substr(sep + 1); }
        std::string n = Lower(name);
        if (n == "c" || n == "settings") { *mode = 'c'; continue; }
        if (n == "p") { *mode = 'p'; continue; }
        if (n == "s") { *mode = 's'; continue; }
        if (n == "windowed" || n == "w") { g_windowed = true; continue; }
        if (n == "seconds" || n == "debuglog") { g_runSeconds = std::atoi(value.c_str()); continue; }
        if (n == "noboidlog") continue;
        if ((n == "boids" || n == "size" || n == "speed") && value.empty() && i + 1 < argc) value = argv[++i];
        if (!ApplySetting(n, value)) std::fprintf(stderr, "shoal: ignoring unknown option '%s'\n", a.c_str());
    }
}

int main(int argc, char** argv)
{
    char mode;
    LoadSettings();
    ParseArgs(argc, argv, &mode);
    if (mode == 'c')
    {
        std::string self = argv[0];
        size_t slash = self.rfind('/');
        std::string dir = slash == std::string::npos ? "." : self.substr(0, slash);
        std::string cmd = "'" + dir + "/shoal-settings.py' || python3 '" + dir + "/shoal-settings.py'";
        return std::system(cmd.c_str()) == 0 ? 0 : 1;
    }
    if (mode == 'p') return 0; // no embedded preview on Linux

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    int displays = SDL_GetNumVideoDisplays();
    std::vector<double> refresh;
    if (g_windowed)
    {
        int w = 1280, h = 720;
        g_monitors.push_back({ 0, 0, w, h });
        refresh.push_back(60.0);
    }
    else
    {
        for (int d = 0; d < displays; ++d)
        {
            SDL_Rect r;
            if (SDL_GetDisplayBounds(d, &r) != 0) continue;
            SDL_DisplayMode mode_;
            double hz = 60.0;
            if (SDL_GetCurrentDisplayMode(d, &mode_) == 0 && mode_.refresh_rate > 0) hz = mode_.refresh_rate;
            g_monitors.push_back({ r.x, r.y, r.x + r.w, r.y + r.h });
            refresh.push_back(hz);
        }
    }
    if (g_monitors.empty()) { std::fprintf(stderr, "No displays found.\n"); return 1; }

    int refHeight = g_monitors[0].bottom - g_monitors[0].top;
    g_settings.boidLength = BoidLengthForSize(g_settings.sizeTenths, refHeight);
    CreateWorld();
    CreateBoids();

    std::vector<Window> windows(g_monitors.size());
    SDL_GLContext ctx = nullptr;
    for (size_t i = 0; i < g_monitors.size(); ++i)
    {
        Window& w = windows[i];
        w.bounds = g_monitors[i];
        w.refreshHz = refresh[i];
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
        if (!g_windowed) flags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN_DESKTOP;
        w.window = SDL_CreateWindow("Shoal", g_windowed ? SDL_WINDOWPOS_CENTERED : w.bounds.left, g_windowed ? SDL_WINDOWPOS_CENTERED : w.bounds.top,
            w.bounds.right - w.bounds.left, w.bounds.bottom - w.bounds.top, flags);
        if (!w.window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
        if (!g_windowed) { SDL_SetWindowPosition(w.window, w.bounds.left, w.bounds.top); SDL_SetWindowFullscreen(w.window, SDL_WINDOW_FULLSCREEN_DESKTOP); }
        if (!ctx)
        {
            ctx = SDL_GL_CreateContext(w.window);
            if (!ctx) { std::fprintf(stderr, "D3D11-class GPU not found: OpenGL 4.3 context failed: %s\n", SDL_GetError()); return 1; }
            glewExperimental = GL_TRUE;
            if (glewInit() != GLEW_OK) { std::fprintf(stderr, "glewInit failed\n"); return 1; }
            glGetError();
            if (!InitGpu()) { std::fprintf(stderr, "GPU initialisation failed (see shader log above).\n"); return 1; }
        }
    }
    if (!g_windowed) SDL_ShowCursor(SDL_DISABLE);

    bool vsync = g_settings.vsync && g_settings.frameRate == 0;
    SDL_GL_MakeCurrent(windows[0].window, ctx);
    SDL_GL_SetSwapInterval(vsync ? 1 : 0);

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    auto last = start;
    double inputReadyAt = 1.0;
    int mouseX = 0, mouseY = 0;
    bool haveMouse = false;
    bool running = true;
    long totalFrames = 0;
    std::vector<double> deadline(windows.size(), 0.0);

    while (running)
    {
        SDL_Event e;
        double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT) running = false;
            if (g_windowed) { if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false; continue; }
            if (elapsed < inputReadyAt) continue;
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_FINGERDOWN) running = false;
            if (e.type == SDL_MOUSEMOTION)
            {
                if (!haveMouse) { mouseX = e.motion.x; mouseY = e.motion.y; haveMouse = true; }
                else if (std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 2 || std::abs(e.motion.x - mouseX) > 4 || std::abs(e.motion.y - mouseY) > 4) running = false;
            }
        }
        if (g_runSeconds > 0 && elapsed >= g_runSeconds) break;

        auto now = Clock::now();
        // Window due times: with vsync each swap paces itself; otherwise cap per target rate.
        std::vector<size_t> due;
        for (size_t i = 0; i < windows.size(); ++i)
        {
            double target = 0.0;
            if (!vsync)
            {
                if (g_settings.frameRate > 0) target = g_settings.frameRate;
                else
                {
                    static const double mult[6] = { 0.5, 1.0, 2.0, 3.0, 4.0, 0.0 };
                    target = windows[i].refreshHz * mult[g_settings.frameRateMode];
                }
            }
            if (target > 0.0)
            {
                if (elapsed < deadline[i]) continue;
                deadline[i] = std::max(deadline[i] + 1.0 / target, elapsed - 1.0 / target);
            }
            due.push_back(i);
        }
        if (due.empty()) { std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }

        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::max(0.0001f, std::min(dt, 0.1f));
        SDL_GL_MakeCurrent(windows[due[0]].window, ctx);
        StepSimulation(dt);
        for (size_t i : due)
        {
            SDL_GL_MakeCurrent(windows[i].window, ctx);
            SDL_GL_SetSwapInterval(vsync ? 1 : 0);
            RenderWindow(windows[i]);
        }
        ++totalFrames;
    }

    double total = std::chrono::duration<double>(Clock::now() - start).count();
    if (g_runSeconds > 0) std::printf("%d fish, %zu window(s), box %.0fx%.0fx%.0f, %ld frames in %.1fs (%.1f fps)\n", (int)g_boids.size(), windows.size(), g_box.right - g_box.left, g_box.bottom - g_box.top, g_box.depth, totalFrames, total, totalFrames / total);
    SDL_Quit();
    return 0;
}
