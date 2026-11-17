// Shoal engine: world setup, the compute-shader simulation and the instanced fish draw (OpenGL 4.3 core).
// Shared by every desktop build: ShoalLinux (SDL), ShoalWallpaper (Plasma/Qt) and ShoalWindows (Win32/WGL).
// ShoalAndroid is a Java port of the same code. No toolkit types here, only the C++ standard library.
//
// Coordinates are "world" pixels: every monitor (water area) is a rectangle in one shared space, as on a desktop with
// several screens. A host steps the simulation once per frame and then draws one or more views of it (one per window).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ShoalRect
{
    float left = 0, top = 0, right = 0, bottom = 0;
    float width() const { return right - left; }
    float height() const { return bottom - top; }
    bool empty() const { return right <= left || bottom <= top; }
    bool operator==(const ShoalRect& o) const { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
    bool operator!=(const ShoalRect& o) const { return !(*this == o); }
};

// Same layout as Rock in the shaders: centre, radius, point count, up to 6 points relative to the centre.
// A negative radius makes the obstacle soft: fish steer around it but are never pushed out of it.
struct ShoalObstacle { float x, y, radius, pointCount, points[12]; };

struct ShoalSettings
{
    int speed = 42;
    int speedVariability = 5;
    int fishCoverage = 50;       // fish count from screen coverage, unless boidCount > 0
    int boidCount = 0;
    int randomness = 0;
    int sizeTenths = 24;         // fish length in tenths of a percent of the reference height
    int sizeVariation = 100;
    bool sizeBell = true;
    int tailPercent = 33;
    int tailShade = -40;
    bool depthEffect = true;
    int rockCount = 5;
    bool restrictToMonitor = false;
    bool colourSort = false;
    bool colourAvoid = false;
    int colourCount = 5;
    uint32_t colours[10] = { 0xdce6f0, 0xffcd28, 0xff781e, 0x286ee6, 0x28c8be }; // 0xRRGGBB
    bool showFps = false;

    // Fish species (shapes in shaders/fish.vert): bit n = species n+1 swims. Mode 0 = a random mix,
    // 1 = one species per colour (colour n gets the n-th enabled species, cycling).
    uint32_t speciesMask = 0x3FF;
    int speciesMode = 0;

    // Background. Picture mode needs setPicture(); without a picture the water colour is used.
    uint32_t background = 0x000000; // water colour, 0xRRGGBB
    bool picture = false;
    int pictureDim = 0;             // darken the picture, percent
    bool transparent = false;       // the host draws the background underneath (premultiplied alpha), e.g. a QML Image
};

// One window's view of the world.
struct ShoalView
{
    ShoalRect bounds;            // world rectangle shown by the window
    int pixelWidth = 0, pixelHeight = 0;
    bool fpsAtBottom = false;    // frame-rate counter in the bottom right corner instead of the top right
    int fps = -1;                // the number the counter shows; -1 = the engine's steps per second (one window per step)
    int picture = 0;             // which background picture slot this view shows (see setPicture)
};

struct ShoalEngineState;

class ShoalEngine
{
public:
    ShoalEngine();
    ~ShoalEngine(); // frees GL objects: call with the engine's context current

    // Compiles the shaders and creates buffers. Needs a current OpenGL 4.3 core context.
    bool init(std::string* error = nullptr);

    // State changes are recorded here and applied by the next step()/draw(), so they can be called at any time.
    void setSettings(const ShoalSettings& s);   // rebuilds the shoal when a setting that shapes it changes
    // The water areas, and the height that fish sizes are relative to (usually the first screen's).
    // The shoal is rebuilt when the reference height or the number of monitors changes.
    void setMonitors(const std::vector<ShoalRect>& monitors, float referenceHeight);
    void rebuild();                             // new random shoal and rocks on the next step
    // Obstacles that aren't drawn (desktop icons, windows). `generation` changes whenever the list does.
    void setObstacles(const std::vector<ShoalObstacle>& obstacles, int generation);
    void setDynamicObstacles(const std::vector<ShoalObstacle>& obstacles); // replaced every frame (mouse pointer)
    // Background picture, RGBA8 rows top to bottom; nullptr removes it. The data is copied. Without `area` each view
    // shows the picture's middle, covering the view without stretching; with it, the picture lies on that world
    // rectangle and each view shows its own part (a capture of the whole desktop, one monitor per view).
    // `slot` lets views show different pictures (one per monitor, as on a Windows desktop); see ShoalView::picture.
    void setPicture(const uint8_t* rgba, int width, int height, const ShoalRect* area = nullptr, int slot = 0);

    float clockDelta();          // seconds since the last call, smoothed over pauses and clamped like the hosts expect
    void step(float dt);         // advances the simulation once
    void draw(const ShoalView& view); // clears and draws the view into the currently bound draw framebuffer

    // Position (x, y), heading and speed of one fish, read back from the GPU (slow; for following a fish while debugging).
    bool fishState(int index, float out[4]);
    // RGBA8 pixels of the bound read framebuffer, bottom row first (for saving frames).
    void readPixels(int width, int height, uint8_t* rgba);
    int fishCount() const;
    int rockCount() const;       // drawn rocks, not obstacles
    int fps() const;             // simulation steps in the last second
    float boidLength() const;
    const std::string& error() const;

    // Where informational lines go (default: stderr). The wallpaper routes them to the journal.
    static void setLogger(std::function<void(const std::string&)> log);

private:
    std::unique_ptr<ShoalEngineState> d;
};

// The species, in the order of the table in fish.vert (setting numbers are 1-based).
constexpr int kShoalSpeciesCount = 10;
extern const char* const kShoalSpeciesNames[kShoalSpeciesCount];

// Shader sources, embedded by ShoalCore/embed_shaders.py into each host's build.
const char* ShoalShaderSource(const char* name);
