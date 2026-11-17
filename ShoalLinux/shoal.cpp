// Shoal for Linux: SDL2 windows (one per display) around the shared engine in ../ShoalCore, which does the world
// setup, compute-shader simulation and drawing. This file is settings, command line, windows and frame pacing.
#include <SDL.h>
#include <SDL_image.h>

#include "shoal_engine.h"
#include "shoal_linux.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

struct Settings
{
    ShoalSettings core;
    bool vsync = true;
    int frameRateMode = 1;       // without vsync: 0=1/2x 1=1x 2=2x 3=3x 4=4x 5=unlimited
    int frameRate = 0;           // fixed rate, overrides the mode
    int backgroundMode = 0;      // 0 = water colour, 1 = picture
    std::string backgroundImage; // empty = Plasma's default wallpaper
    std::string gpu;             // PCI address; empty = the dedicated GPU if there is one; "primary" = no offload
};

static Settings g_settings;
static bool g_windowed = false;
static int g_runSeconds = 0;
// Development: follow fish 0 magnified, and save frames (window 0) as PNGs.
static bool g_follow = false;
static float g_zoom = 4.0f;
static std::string g_captureDir;
static int g_captureFrames = 60;

// ---------------------------------------------------------------- settings
static std::string ConfigPath()
{
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? xdg : (std::string(home ? home : ".") + "/.config");
    return base + "/shoal/settings.conf";
}

static std::string Lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

static unsigned ParseColour(std::string v)
{
    if (!v.empty() && v[0] == '#') v = v.substr(1);
    return (unsigned)std::strtoul(v.c_str(), nullptr, 16) & 0xFFFFFF;
}

// "1,3,koi" -> species bit mask (numbers are 1-based, names as in kShoalSpeciesNames; "all" = every species).
static uint32_t ParseSpecies(const std::string& value)
{
    uint32_t mask = 0;
    size_t start = 0;
    while (start <= value.size())
    {
        size_t end = value.find_first_of(", ", start);
        if (end == std::string::npos) end = value.size();
        std::string item = Lower(value.substr(start, end - start));
        if (item == "all") mask = (1u << kShoalSpeciesCount) - 1;
        else if (!item.empty() && std::isdigit((unsigned char)item[0]))
        {
            int n = std::atoi(item.c_str());
            if (n >= 1 && n <= kShoalSpeciesCount) mask |= 1u << (n - 1);
        }
        else
            for (int i = 0; i < kShoalSpeciesCount; ++i)
                if (!item.empty() && Lower(kShoalSpeciesNames[i]) == item) mask |= 1u << i;
        start = end + 1;
    }
    return mask;
}

static std::string SpeciesList(uint32_t mask)
{
    std::string out;
    for (int i = 0; i < kShoalSpeciesCount; ++i)
        if (mask & (1u << i)) out += (out.empty() ? "" : ",") + std::to_string(i + 1);
    return out;
}

static bool ApplySetting(const std::string& rawName, const std::string& value)
{
    std::string n = Lower(rawName);
    Settings& g = g_settings;
    ShoalSettings& s = g.core;
    auto I = [&](int lo, int hi, int* t) { *t = std::max(lo, std::min(hi, (int)std::strtol(value.c_str(), nullptr, 10))); return true; };
    auto B = [&](bool* t) { *t = std::strtol(value.c_str(), nullptr, 10) != 0; return true; };
    if (n == "speed") return I(1, 1000, &s.speed);
    if (n == "randomness") return I(0, 100, &s.randomness);
    if (n == "speedvar" || n == "speedvariability") return I(0, 10, &s.speedVariability);
    if (n == "coverage" || n == "fishcoverage") return I(0, 100, &s.fishCoverage);
    if (n == "boids") return I(2, 10000, &s.boidCount);
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
    if (n == "vsync") return B(&g.vsync);
    if (n == "fpsmode" || n == "frameratemode") return I(0, 5, &g.frameRateMode);
    if (n == "framerate") { g.vsync = false; return I(1, 100000, &g.frameRate); }
    if (n == "colourcount") return I(1, 10, &s.colourCount);
    if (n.size() == 7 && n.compare(0, 6, "colour") == 0 && std::isdigit((unsigned char)n[6])) { s.colours[n[6] - '0'] = ParseColour(value); return true; }
    if (n == "backgroundmode" || n == "background") return I(0, 1, &g.backgroundMode);
    if (n == "backgroundcolor" || n == "backgroundcolour" || n == "water") { s.background = ParseColour(value); return true; }
    if (n == "backgroundimage" || n == "picture")
    {
        g.backgroundImage = value.rfind("file://", 0) == 0 ? value.substr(7) : value;
        if (!value.empty() && n == "picture") g.backgroundMode = 1;
        return true;
    }
    if (n == "backgrounddim" || n == "dim") return I(0, 100, &s.pictureDim);
    if (n == "gpu") { g.gpu = value; return true; }
    if (n == "species") { uint32_t m = ParseSpecies(value); if (m) s.speciesMask = m; return true; }
    if (n == "speciesmode")
    {
        std::string v = Lower(value);
        s.speciesMode = v == "colour" || v == "color" || v == "bycolour" || v == "1" ? 1 : 0;
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
    const Settings& g = g_settings;
    const ShoalSettings& s = g.core;
    f << "# Shoal settings. Same names and ranges as the Windows version's registry values (see README.md).\n"
      << "Speed=" << s.speed << "\nSpeedVariability=" << s.speedVariability << "\nFishCoverage=" << s.fishCoverage
      << "\nRandomness=" << s.randomness << "\nSize=" << s.sizeTenths / 10.0 << "   # percent of screen height\n"
      << "SizeVariation=" << s.sizeVariation << "\nSizeBell=" << s.sizeBell << "\nTailPercent=" << s.tailPercent
      << "\nTailShade=" << s.tailShade << "\nDepthEffect=" << s.depthEffect << "\nRockCount=" << s.rockCount
      << "\nRestrictToMonitor=" << s.restrictToMonitor << "\nShowFps=" << s.showFps << "\nVSync=" << g.vsync
      << "\nFrameRateMode=" << g.frameRateMode << "   # when VSync=0: 0=1/2x 1=1x 2=2x 3=3x 4=4x 5=unlimited\n"
      << "ColourSort=" << s.colourSort << "\nColourAvoid=" << s.colourAvoid << "\nColourCount=" << s.colourCount << "\n";
    char buf[32];
    for (int i = 0; i < s.colourCount; ++i) { std::snprintf(buf, sizeof buf, "Colour%d=#%06x\n", i, s.colours[i]); f << buf; }
    std::snprintf(buf, sizeof buf, "#%06x", s.background);
    f << "BackgroundMode=" << g.backgroundMode << "   # 0=water colour 1=picture\nBackgroundColor=" << buf
      << "\nBackgroundImage=" << g.backgroundImage << "   # empty = Plasma's default wallpaper\nBackgroundDim=" << s.pictureDim
      << "   # darken the picture, percent\nGpu=" << g.gpu << "   # PCI address; empty = dedicated GPU; primary = no offload\n"
      << "Species=" << SpeciesList(s.speciesMask) << "   # 1 Classic 2 Sardine 3 Goldfish 4 Koi 5 Tuna 6 Trout 7 Pike 8 Tetra 9 Fantail 10 Mackerel\n"
      << "SpeciesMode=" << s.speciesMode << "   # 0 = random mix, 1 = one species per colour\n";
}

static void LoadSettings()
{
    std::string path = ConfigPath();
    std::ifstream f(path);
    if (!f) { WriteDefaultConfig(path); return; }
    std::string line;
    while (std::getline(f, line))
    {
        if (line.find('#') == 0) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq), value = line.substr(eq + 1);
        size_t c = value.find("   #");
        if (c != std::string::npos) value = value.substr(0, c);
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
        while (!value.empty() && std::isspace((unsigned char)value.front())) value.erase(value.begin());
        ApplySetting(name, value);
    }
}

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
        if (n == "follow") { g_follow = true; continue; }
        if (n == "zoom") { g_zoom = std::max(1.0f, (float)std::atof(value.c_str())); continue; }
        if (n == "capture") { g_captureDir = value; continue; }
        if (n == "frames") { g_captureFrames = std::max(1, std::atoi(value.c_str())); continue; }
        if ((n == "boids" || n == "size" || n == "speed" || n == "picture" || n == "gpu" || n == "species" || n == "speciesmode") && value.empty() && i + 1 < argc) value = argv[++i];
        if (!ApplySetting(n, value)) std::fprintf(stderr, "shoal: ignoring unknown option '%s'\n", a.c_str());
    }
}

// ---------------------------------------------------------------- background picture
static void LoadPicture(ShoalEngine& engine, int width, int height)
{
    std::string path = g_settings.backgroundImage;
    if (path.empty()) path = ShoalDefaultWallpaper(width, height);
    if (path.empty()) { std::fprintf(stderr, "shoal: no picture (Plasma's wallpapers aren't installed); using the water colour\n"); return; }
    SDL_Surface* loaded = IMG_Load(path.c_str());
    if (!loaded) { std::fprintf(stderr, "shoal: can't load picture %s: %s\n", path.c_str(), IMG_GetError()); return; }
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ABGR8888, 0); // bytes R,G,B,A
    SDL_FreeSurface(loaded);
    if (!rgba) return;
    std::vector<uint8_t> pixels((size_t)rgba->w * rgba->h * 4);
    for (int y = 0; y < rgba->h; ++y)
        std::memcpy(&pixels[(size_t)y * rgba->w * 4], (const uint8_t*)rgba->pixels + (size_t)y * rgba->pitch, (size_t)rgba->w * 4);
    engine.setPicture(pixels.data(), rgba->w, rgba->h);
    std::fprintf(stderr, "shoal: picture %s (%dx%d)\n", path.c_str(), rgba->w, rgba->h);
    SDL_FreeSurface(rgba);
}

// ---------------------------------------------------------------- main
struct Window
{
    SDL_Window* window = nullptr;
    ShoalRect bounds;
    double refreshHz = 60.0;
};

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

    // GPU: must be chosen before SDL loads the GL library.
    if (Lower(g_settings.gpu) != "primary")
    {
        std::string gpu = ShoalResolveGpu(g_settings.gpu);
        if (!gpu.empty()) std::fprintf(stderr, "shoal: GPU %s\n", ShoalUsePrimeGpu(gpu).c_str());
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    std::vector<Window> windows;
    if (g_windowed)
        windows.push_back({ nullptr, { 0, 0, 1280, 720 }, 60.0 });
    else
    {
        for (int d = 0; d < SDL_GetNumVideoDisplays(); ++d)
        {
            SDL_Rect r;
            if (SDL_GetDisplayBounds(d, &r) != 0) continue;
            SDL_DisplayMode dm;
            double hz = 60.0;
            if (SDL_GetCurrentDisplayMode(d, &dm) == 0 && dm.refresh_rate > 0) hz = dm.refresh_rate;
            windows.push_back({ nullptr, { (float)r.x, (float)r.y, (float)(r.x + r.w), (float)(r.y + r.h) }, hz });
        }
    }
    if (windows.empty()) { std::fprintf(stderr, "No displays found.\n"); return 1; }

    ShoalEngine engine;
    SDL_GLContext ctx = nullptr;
    for (Window& w : windows)
    {
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
        if (!g_windowed) flags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN_DESKTOP;
        const int x = (int)w.bounds.left, y = (int)w.bounds.top, ww = (int)w.bounds.width(), wh = (int)w.bounds.height();
        w.window = SDL_CreateWindow("Shoal", g_windowed ? SDL_WINDOWPOS_CENTERED : x, g_windowed ? SDL_WINDOWPOS_CENTERED : y, ww, wh, flags);
        if (!w.window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
        if (!g_windowed) { SDL_SetWindowPosition(w.window, x, y); SDL_SetWindowFullscreen(w.window, SDL_WINDOW_FULLSCREEN_DESKTOP); }
        if (!ctx)
        {
            ctx = SDL_GL_CreateContext(w.window);
            if (!ctx) { std::fprintf(stderr, "OpenGL 4.3 context failed: %s\n", SDL_GetError()); return 1; }
            std::string error;
            if (!engine.init(&error)) { std::fprintf(stderr, "GPU initialisation failed: %s\n", error.c_str()); return 1; }
        }
    }
    if (!g_windowed) SDL_ShowCursor(SDL_DISABLE);

    std::vector<ShoalRect> monitors;
    for (const Window& w : windows) monitors.push_back(w.bounds);
    g_settings.core.picture = g_settings.backgroundMode == 1;
    engine.setSettings(g_settings.core);
    engine.setMonitors(monitors, monitors[0].height());
    if (g_settings.core.picture) LoadPicture(engine, (int)monitors[0].width(), (int)monitors[0].height());

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
    int captured = 0;
    if (!g_captureDir.empty()) std::system(("mkdir -p '" + g_captureDir + "'").c_str());
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

        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::max(0.0001f, std::min(dt, 0.1f));
        SDL_GL_MakeCurrent(windows[due[0]].window, ctx);
        engine.step(dt);
        for (size_t i : due)
        {
            Window& w = windows[i];
            SDL_GL_MakeCurrent(w.window, ctx);
            SDL_GL_SetSwapInterval(vsync ? 1 : 0);
            ShoalView view;
            view.bounds = w.bounds;
            SDL_GL_GetDrawableSize(w.window, &view.pixelWidth, &view.pixelHeight);
            float fish[4];
            if (g_follow && engine.fishState(0, fish))
            {
                const float hw = w.bounds.width() * 0.5f / g_zoom, hh = w.bounds.height() * 0.5f / g_zoom;
                view.bounds = { fish[0] - hw, fish[1] - hh, fish[0] + hw, fish[1] + hh };
            }
            engine.draw(view);
            if (i == 0 && !g_captureDir.empty() && elapsed > 1.5 && captured < g_captureFrames)
            {
                // Read the back buffer before the swap, bottom-up rows flipped into a top-down surface.
                SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, view.pixelWidth, view.pixelHeight, 32, SDL_PIXELFORMAT_ABGR8888);
                std::vector<uint8_t> rows((size_t)view.pixelWidth * view.pixelHeight * 4);
                engine.readPixels(view.pixelWidth, view.pixelHeight, rows.data());
                for (int y = 0; y < view.pixelHeight; ++y)
                    std::memcpy((uint8_t*)shot->pixels + (size_t)y * shot->pitch, &rows[(size_t)(view.pixelHeight - 1 - y) * view.pixelWidth * 4], (size_t)view.pixelWidth * 4);
                char name[64];
                std::snprintf(name, sizeof name, "/frame%04d.png", captured++);
                IMG_SavePNG(shot, (g_captureDir + name).c_str());
                SDL_FreeSurface(shot);
                if (captured == g_captureFrames) running = false;
            }
            SDL_GL_SwapWindow(w.window);
        }
        ++totalFrames;
    }

    double total = std::chrono::duration<double>(Clock::now() - start).count();
    if (g_runSeconds > 0)
        std::printf("%d fish, %zu window(s), %ld frames in %.1fs (%.1f fps)\n", engine.fishCount(), windows.size(), totalFrames, total, totalFrames / total);
    SDL_Quit();
    return 0;
}
