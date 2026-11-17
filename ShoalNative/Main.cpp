#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(x) do { if ((x)) { (x)->Release(); (x) = nullptr; } } while (0)
#endif

static LONGLONG g_startCounter = 0;
static LONGLONG g_lastSimulationCounter = 0;
static bool g_isPreview = false;
static POINT g_initialCursorPosition = {};
static DWORD g_inputReadyTick = 0;
static const float kRockBufferDistance = 40.0f;
static const wchar_t* kSettingsKey = L"Software\\Shoal";

struct Settings
{
    int speedPixelsPerSecond;
    int boidCount; // Derived from fishCoverage, fish size and free screen area (or set by /boids:N).
    int randomness;
    int boidLength; // Pixels, derived from sizeTenths and the primary screen height.
    bool showFps;
    int frameRate; // Command-line override (/framerate:N) only; 0 = use vsync / frameRateMode.
    int rockCount;
    bool restrictToMonitor;
    bool vsync;
    int frameRateMode; // Used when vsync is off: 0 = 1/2x, 1-4 = 1x-4x the monitor refresh rate, 5 = unlimited.
    int sizeTenths; // Fish length in tenths of a percent of the primary screen height (10-100 = 1%-10%).
    int speedVariability; // 0 = every fish always swims at Speed; 10 = fish freely range from 1 to 1000 px/s around it.
    int fishCoverage; // Fish Amount slider position 0-100, log-mapped to the share of free screen area covered by fish.
    int tailPercent; // Tail size as a percentage of the body (10-100); the whole fish still spans boidLength.
    int colourCount; // 1-kMaxFishColours entries used in colours.
    COLORREF colours[10]; // Head colours; each fish is randomly assigned one.
    int tailShade; // -100..100: lightness change from head to tail tip; negative darkens, positive lightens.
    bool colourSort; // Fish only school with their own colour (they still keep their spacing from every fish).
    bool colourAvoid; // With colourSort, differently coloured fish also actively steer away from each other.
    int sizeVariation; // 100-300 (%): each fish's length is boidLength scaled by up to this factor smaller or larger.
    bool sizeBell; // Size spread: true = bell-shaped (most fish near boidLength), false = even (log-uniform).
    bool depthEffect; // 3D depth: larger fish swim above smaller ones and cast shadows on fish below.
    bool desktopBackground; // Draw a snapshot of the desktop behind the fish instead of black.
    bool randomPreset; // Each run uses a randomly picked saved preset instead of these settings. Not stored in presets.
};

static const int kFrameRateModeCount = 6;
static const wchar_t* kFrameRateModeNames[kFrameRateModeCount] = { L"1/2x", L"1x", L"2x", L"3x", L"4x", L"Unlimited" };
static const double kFrameRateModeMultipliers[kFrameRateModeCount] = { 0.5, 1.0, 2.0, 3.0, 4.0, 0.0 };

static const int kMaxFishColours = 10;
// Default ocean palette: silver, yellow tang, clownfish orange, blue tang, teal.
static Settings g_settings = { 42, 20, 0, 26, false, 0, 5, false, true, 1, 24, 5, 50, 33,
    5, { RGB(220, 230, 240), RGB(255, 205, 40), RGB(255, 120, 30), RGB(40, 110, 230), RGB(40, 200, 190) }, -40, false, false, 100, true, true, false };
static bool g_boidCountOverridden = false;
// Spacing, wall and rock avoidance were tuned for 20 px fish; they scale from this reference length.
static const float kReferenceBoidLength = 20.0f;

static int ReferenceScreenHeight()
{
    int height = GetSystemMetrics(SM_CYSCREEN);
    return height > 0 ? height : 1080;
}

static int BoidLengthForSize(int sizeTenths)
{
    return (std::max)(4, static_cast<int>(std::lround(ReferenceScreenHeight() * sizeTenths / 1000.0)));
}

// Longest fish the Size Variability setting can produce.
static float MaxBoidLength()
{
    return static_cast<float>(g_settings.boidLength) * g_settings.sizeVariation / 100.0f;
}

// How much closer than a reference-size fish's center the largest fish's body reaches toward a wall or rock.
static float BoidBodyOffset()
{
    return (MaxBoidLength() - kReferenceBoidLength) * 0.5f;
}
static float g_simulationDeltaSeconds = 1.0f / 60.0f;
static LARGE_INTEGER g_qpcFrequency = {};

struct Boid
{
    float x;
    float y;
    float vx;
    float vy;
    float breakawayHeading;
    float swimTurnRate;
    float swimTurnTarget;
    float length; // This fish's length in pixels (varies with Size Variability).
    float depth; // 0 = deepest, 1 = nearest the viewer; fixed for life, ranked by length (3D depth effect).
};

struct GpuBoid
{
    float x;
    float y;
    float vx;
    float vy;
    float turnRate;
    float turnTarget;
    float independentUntil;
    float nextTurnChange;
    float heading;
    float currentSpeed;
    float stopped;
    float breakawayHeading;
    float cruiseSpeed; // Speed the fish currently wants to swim at (logged to tell chosen slowness from stalls).
    float yawRate; // Smoothed heading change in rad/s; bends the tail away from turns.
    float swimPhase; // Tail-beat phase in radians, advanced at a rate set by the fish's speed and size.
    float thrust; // Smoothed tail effort: about 1 cruising, up to 1.6 accelerating, near 0 coasting while slowing down.
    float fishLength; // Pixels; fixed per fish.
    float depth; // 0 = deepest, 1 = nearest the viewer; fixed for life (3D depth effect).
};

struct GpuMonitor
{
    float left;
    float top;
    float right;
    float bottom;
};

struct Rock
{
    float x;
    float y;
    float radius;
    int pointCount;
    POINT points[6];
};

struct GpuRock
{
    float x;
    float y;
    float radius;
    float pointCount;
    float points[12];
};

struct GpuSimulationConstants
{
    float deltaSeconds;
    float speedPixelsPerSecond;
    float boidCount;
    float randomness;
    float monitorCount;
    float timeSeconds;
    float rockCount;
    float restrictToMonitor;
    float baseLength; // Set fish size; each fish's own length is in GpuBoid::fishLength.
    float speedVariability;
    float colourCount;
    float colourBias; // 1 when sorting by colour (needs 2+ colours), else 0; other colours' schooling weight is 1 - colourBias.
    float colourAvoid;
    float padding[3];
};

struct GpuRenderConstants
{
    float left;
    float top;
    float right;
    float bottom;
    float length;
    float monitorCount;
    float restrictToMonitor;
    float tailRatio; // Tail size relative to the body (0.1-1).
    float colourCount;
    float tailShade; // -1..1
    float depthEffect; // 1 = draw with depth, depth cues and shadows.
    float padding;
    float colours[10][4]; // RGB head colours (HLSL float4 array).
};

static std::vector<RECT> g_monitorBounds;
static std::vector<Boid> g_boids;
static std::vector<Rock> g_rocks;
// Rock outlines (10 rocks x 6 edges x 6 vertices) plus the FPS counter, with headroom.
static const UINT kMaxOverlayVertices = 2048;
static ID3D11Buffer* g_gpuBoidBuffer = nullptr;
static ID3D11ShaderResourceView* g_gpuBoidView = nullptr;
static ID3D11UnorderedAccessView* g_gpuBoidUav = nullptr;
static ID3D11Buffer* g_gpuMonitorBuffer = nullptr;
static ID3D11ShaderResourceView* g_gpuMonitorView = nullptr;
static ID3D11Buffer* g_gpuConstantsBuffer = nullptr;
static ID3D11ComputeShader* g_gpuBoidComputeShader = nullptr;
static ID3D11ComputeShader* g_gpuRockAvoidComputeShader = nullptr;
static ID3D11Buffer* g_gpuRockBuffer = nullptr;
static ID3D11ShaderResourceView* g_gpuRockView = nullptr;
static ID3D11Device* g_sharedDevice = nullptr;
static ID3D11DeviceContext* g_sharedContext = nullptr;
static IDXGIFactory* g_sharedFactory = nullptr;
static float g_gpuSimulationTime = 0.0f;
// Set when a window cannot get a Direct3D 11 device with compute and instanced-rendering shaders.
static bool g_gpuUnavailable = false;

static FILE* g_debugLogFile = nullptr;
static DWORD g_debugLogDurationMs = 0;
static DWORD g_debugLogStartTick = 0;
static float g_debugLogTime = 0.0f;
static float g_debugLogNextSampleTime = 0.0f;
static float g_debugLogNextSnapshotTime = 0.0f;
static ID3D11Buffer* g_gpuBoidStagingBuffer = nullptr;
static const float kDebugLogIntervalSeconds = 0.05f;
static const float kDebugLogSnapshotSeconds = 0.5f;
struct FrameSample
{
    LONGLONG counter;
    int window;
    float intervalMs;
    float simDeltaMs;
    float simCpuMs;
    float renderCpuMs;
    float presentMs;
    UINT presentCount;
    UINT presentRefreshCount;
    UINT syncRefreshCount;
    LONGLONG syncQpc;
};
static bool g_frameLogEnabled = false;
static bool g_boidLogEnabled = true;
static std::vector<FrameSample> g_frameSamples;
static int g_nextWindowIndex = 0;
static const size_t kDebugTrackedBoidCount = 8;
static const float kDebugLogNearDistance = 80.0f;

static const int kSettingsOk = 1001;
static const int kSettingsCancel = 1002;
static const int kSpeedSlider = 1101;
static const int kCountSlider = 1102;
static const int kRandomnessSlider = 1103;
static const int kSizeSlider = 1105;
static const int kShowFpsCheckbox = 1106;
static const int kFrameRateSlider = 1107;
static const int kRockCountSlider = 1108;
static const int kRestrictToMonitorCheckbox = 1109;
static const int kVSyncCheckbox = 1110;
static const int kSpeedVariabilitySlider = 1111;
static const int kTailSlider = 1112;
static const int kAddColourButton = 1113;
static const int kTailShadeSlider = 1114;
static const int kColourSortCheckbox = 1115;
static const int kColourAvoidCheckbox = 1116;
static const int kSizeVariationSlider = 1117;
static const int kSizeBellCheckbox = 1118;
static const int kDepthEffectCheckbox = 1119;
static const int kDesktopBackgroundCheckbox = 1120;
static const int kPresetCombo = 1121;
static const int kPresetSaveButton = 1122;
static const int kPresetDeleteButton = 1123;
static const int kRandomPresetCheckbox = 1124;
static Settings g_pendingSettings;
static HWND g_speedValue = nullptr;
static HWND g_countValue = nullptr;
static HWND g_randomnessValue = nullptr;
static HWND g_sizeValue = nullptr;
static HWND g_frameRateValue = nullptr;
static HWND g_rockCountValue = nullptr;
static HWND g_speedVariabilityValue = nullptr;
static HWND g_tailValue = nullptr;
static HWND g_tailShadeValue = nullptr;
static HWND g_sizeVariationValue = nullptr;
// Colour swatches (client coordinates): click to change, right-click to remove.
static const int kSwatchLeft = 510;
static const int kSwatchTop = 94;
static const int kSwatchSize = 20;
static const int kSwatchStride = 24;
static COLORREF g_customColours[16];
// Actual-size fish preview in the settings dialog (client coordinates).
static const RECT kSizePreviewRect = { 24, 500, 940, 580 };
static HFONT g_titleFont = nullptr;
static HFONT g_iconFont = nullptr;

static HWND CreateSettingsLabel(HWND parent, HINSTANCE instance, const wchar_t* text, int x, int y, int width, int height)
{
    return CreateWindowEx(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
        x, y, width, height, parent, nullptr, instance, nullptr);
}

static HWND CreateSettingsSlider(HWND parent, HINSTANCE instance, int id, int minimum, int maximum, int value, int y)
{
    HWND slider = CreateWindowEx(0, TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        150, y, 240, 30, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessage(slider, TBM_SETRANGE, TRUE, MAKELONG(minimum, maximum));
    SendMessage(slider, TBM_SETPOS, TRUE, value);
    return slider;
}

// Fish Amount: the slider sets the share of free screen area (monitors minus rocks) covered by fish bodies
// (length x length/4 each), log-mapped from 0.140625% to 7.294%. On two 16:9 monitors these ends give exactly
// 2 fish at the largest size and 10000 fish at the smallest; other setups are clamped to that range.
static const int kMinBoidCount = 2;
static const int kMaxBoidCount = 10000;
static const double kMinFishCoverage = 0.00140625;
static const double kMaxFishCoverage = 302500.0 / 4147200.0;

static double FishCoverageFraction(int position)
{
    return kMinFishCoverage * std::pow(kMaxFishCoverage / kMinFishCoverage, position / 100.0);
}

static int FishCoveragePositionFor(double fraction)
{
    double position = 100.0 * std::log(fraction / kMinFishCoverage) / std::log(kMaxFishCoverage / kMinFishCoverage);
    return position < 0.0 ? 0 : (position > 100.0 ? 100 : static_cast<int>(std::lround(position)));
}

static double FishFootprint(int boidLength)
{
    return static_cast<double>(boidLength) * boidLength * 0.25;
}

// Mean of (fish length / set length)^2 for the size spread, so the Fish Amount keeps the same covered area.
// Lengths are L * V^u: even spread u uniform in [-1, 1]; bell spread u the mean of three such values.
static double SizeAreaFactor(int sizeVariation, bool bell)
{
    if (sizeVariation <= 100)
    {
        return 1.0;
    }
    double k = 2.0 * std::log(sizeVariation / 100.0) / (bell ? 3.0 : 1.0);
    double factor = std::sinh(k) / k;
    return bell ? factor * factor * factor : factor;
}

static int BoidCountFor(int position, int boidLength, double freeArea, double areaFactor = 1.0)
{
    double count = FishCoverageFraction(position) * freeArea / (FishFootprint(boidLength) * areaFactor);
    return count < kMinBoidCount ? kMinBoidCount : (count > kMaxBoidCount ? kMaxBoidCount : static_cast<int>(std::lround(count)));
}

static double RockArea(const Rock& rock)
{
    double twiceArea = 0.0;
    for (int point = 0; point < rock.pointCount; ++point)
    {
        const POINT& a = rock.points[point];
        const POINT& b = rock.points[(point + 1) % rock.pointCount];
        twiceArea += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return std::fabs(twiceArea) * 0.5;
}

static std::vector<RECT> EnumerateMonitorRects()
{
    std::vector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rect, LPARAM data) -> BOOL
        {
            reinterpret_cast<std::vector<RECT>*>(data)->push_back(*rect);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&rects));
    return rects;
}

static double TotalArea(const std::vector<RECT>& rects)
{
    double area = 0.0;
    for (const RECT& rect : rects)
    {
        area += static_cast<double>(rect.right - rect.left) * (rect.bottom - rect.top);
    }
    return area;
}

// Expected rock area before rocks exist (settings dialog, migration): CreateRocks picks a radius uniformly from
// 90 px to a tenth of a random monitor's shorter side, 4-6 vertices at 70-100% of it (about 1.68 R^2 on average).
static double EstimatedRockArea(const std::vector<RECT>& rects, int rockCount)
{
    if (rects.empty() || rockCount <= 0)
    {
        return 0.0;
    }
    double perRock = 0.0;
    for (const RECT& rect : rects)
    {
        double maximum = (std::min)(rect.right - rect.left, rect.bottom - rect.top) / 10;
        if (maximum < 90.0) continue;
        double meanSquare = maximum > 90.0 ? (maximum * maximum * maximum - 729000.0) / (3.0 * (maximum - 90.0)) : 8100.0;
        perRock += 1.68 * meanSquare / rects.size();
    }
    return perRock * rockCount;
}

static void UpdateSettingsValueLabels(HWND hwnd)
{
    wchar_t value[32];
    HWND slider = GetDlgItem(hwnd, kSpeedSlider);
    wsprintf(value, L"%d px/s", static_cast<int>(SendMessage(slider, TBM_GETPOS, 0, 0)));
    SetWindowText(g_speedValue, value);
    {
        int coverage = static_cast<int>(SendMessage(GetDlgItem(hwnd, kCountSlider), TBM_GETPOS, 0, 0));
        int length = BoidLengthForSize(static_cast<int>(SendMessage(GetDlgItem(hwnd, kSizeSlider), TBM_GETPOS, 0, 0)));
        int rocks = static_cast<int>(SendMessage(GetDlgItem(hwnd, kRockCountSlider), TBM_GETPOS, 0, 0));
        int variation = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSizeVariationSlider), TBM_GETPOS, 0, 0));
        bool bell = SendMessage(GetDlgItem(hwnd, kSizeBellCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
        std::vector<RECT> monitors = EnumerateMonitorRects();
        int count = BoidCountFor(coverage, length, TotalArea(monitors) - EstimatedRockArea(monitors, rocks), SizeAreaFactor(variation, bell));
        int hundredths = static_cast<int>(std::lround(FishCoverageFraction(coverage) * 10000.0));
        wsprintf(value, L"%d.%02d%%\n%d fish", hundredths / 100, hundredths % 100, count);
        SetWindowText(g_countValue, value);
    }
    slider = GetDlgItem(hwnd, kRandomnessSlider);
    wsprintf(value, L"%d%%", static_cast<int>(SendMessage(slider, TBM_GETPOS, 0, 0)));
    SetWindowText(g_randomnessValue, value);
    slider = GetDlgItem(hwnd, kSizeSlider);
    int sizeTenths = static_cast<int>(SendMessage(slider, TBM_GETPOS, 0, 0));
    wsprintf(value, L"%d.%d%%", sizeTenths / 10, sizeTenths % 10);
    SetWindowText(g_sizeValue, value);
    slider = GetDlgItem(hwnd, kFrameRateSlider);
    int mode = static_cast<int>(SendMessage(slider, TBM_GETPOS, 0, 0));
    SetWindowText(g_frameRateValue, kFrameRateModeNames[mode < 0 ? 0 : (mode >= kFrameRateModeCount ? kFrameRateModeCount - 1 : mode)]);
    slider = GetDlgItem(hwnd, kRockCountSlider);
    wsprintf(value, L"%d", static_cast<int>(SendMessage(slider, TBM_GETPOS, 0, 0)));
    SetWindowText(g_rockCountValue, value);
    int variability = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSpeedVariabilitySlider), TBM_GETPOS, 0, 0));
    wsprintf(value, variability == 0 ? L"Off" : L"%d", variability);
    SetWindowText(g_speedVariabilityValue, value);
    wsprintf(value, L"%d%%", static_cast<int>(SendMessage(GetDlgItem(hwnd, kTailSlider), TBM_GETPOS, 0, 0)));
    SetWindowText(g_tailValue, value);
    int shade = static_cast<int>(SendMessage(GetDlgItem(hwnd, kTailShadeSlider), TBM_GETPOS, 0, 0));
    if (shade == 0) wsprintf(value, L"None");
    else wsprintf(value, shade < 0 ? L"%d%% darker" : L"%d%% lighter", shade < 0 ? -shade : shade);
    SetWindowText(g_tailShadeValue, value);
    int variation = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSizeVariationSlider), TBM_GETPOS, 0, 0));
    if (variation <= 100) wsprintf(value, L"Off\n(all same)");
    else wsprintf(value, L"%d%%\n\u00F7%d.%d to \u00D7%d.%d", variation, variation / 100, variation / 10 % 10, variation / 100, variation / 10 % 10);
    SetWindowText(g_sizeVariationValue, value);
}

struct Vertex
{
    float x;
    float y;
    float r;
    float g;
    float b;
    float a;
};

struct RenderWindowState
{
    HWND hwnd;
    RECT bounds; // Desktop area simulated and shown by this window.
    SIZE pixelSize; // Back-buffer size; smaller than bounds in the Control Panel preview, which shows a scaled-down screen.
    double refreshHz;

    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGISwapChain* swapChain;
    ID3D11RenderTargetView* renderTargetView;
    ID3D11InputLayout* inputLayout;
    ID3D11VertexShader* vertexShader;
    ID3D11PixelShader* pixelShader;
    ID3D11Buffer* vertexBuffer;
    ID3D11VertexShader* proceduralVertexShader;
    ID3D11PixelShader* proceduralPixelShader;
    ID3D11Buffer* renderConstantsBuffer;
    // 3D depth effect: depth/stencil target, the shadow pass shaders and its states.
    ID3D11DepthStencilView* depthStencilView;
    ID3D11DepthStencilState* fishDepthState;
    ID3D11DepthStencilState* shadowDepthState;
    ID3D11BlendState* shadowBlendState;
    ID3D11VertexShader* shadowVertexShader;
    ID3D11PixelShader* shadowPixelShader;
    // Desktop background (DesktopBackground): this monitor's screen image captured at start-up.
    ID3D11ShaderResourceView* desktopView;
    ID3D11SamplerState* desktopSampler;
    ID3D11VertexShader* backgroundVertexShader;
    ID3D11PixelShader* backgroundPixelShader;
    std::vector<Vertex> vertices;
    LONGLONG nextFrameCounter;
    DWORD fpsLastTick;
    int fpsFrameCount;
    int currentFps;
    int windowIndex;
    LONGLONG lastFrameCounter;
    double lastPresentMs;
    HANDLE frameLatencyWaitable; // Signals at this monitor's vblank when vsync is on.
    DXGI_FRAME_STATISTICS lastStats;
};

static std::vector<RenderWindowState*> g_renderWindows;

// Screen images taken before the screensaver windows open, drawn behind the fish instead of black (DesktopBackground).
struct DesktopCapture
{
    RECT rect;
    int width;
    int height;
    std::vector<unsigned char> pixels; // Top-down BGRA.
};
static std::vector<DesktopCapture> g_desktopCaptures;

static void CaptureDesktop()
{
    HDC screen = GetDC(nullptr);
    for (const RECT& rect : g_monitorBounds)
    {
        DesktopCapture capture = { rect, rect.right - rect.left, rect.bottom - rect.top, {} };
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = capture.width;
        info.bmiHeader.biHeight = -capture.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (memory && bitmap && bits)
        {
            HGDIOBJ previous = SelectObject(memory, bitmap);
            if (BitBlt(memory, 0, 0, capture.width, capture.height, screen, rect.left, rect.top, SRCCOPY))
            {
                GdiFlush();
                const unsigned char* begin = static_cast<const unsigned char*>(bits);
                capture.pixels.assign(begin, begin + static_cast<size_t>(capture.width) * capture.height * 4);
            }
            SelectObject(memory, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (!capture.pixels.empty())
        {
            g_desktopCaptures.push_back(std::move(capture));
        }
    }
    ReleaseDC(nullptr, screen);
}

static float RandomFloat(float minValue, float maxValue)
{
    float range = maxValue - minValue;
    return minValue + range * (static_cast<float>(rand()) / static_cast<float>(RAND_MAX));
}

static float Clamp(float value, float minValue, float maxValue)
{
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static float Length(float x, float y)
{
    return std::sqrt(x * x + y * y);
}

static int ReadSetting(HKEY key, const wchar_t* name, int fallback, int minimum, int maximum)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueEx(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS ||
        type != REG_DWORD)
    {
        return fallback;
    }
    return (std::max)(minimum, value > static_cast<DWORD>(maximum) ? maximum : static_cast<int>(value));
}

static void LoadSettingsFromKey(HKEY key, Settings& settings)
{
    settings.speedPixelsPerSecond = ReadSetting(key, L"Speed", settings.speedPixelsPerSecond, 1, 1000);
    int legacyCount = ReadSetting(key, L"BoidCount", 0, 0, 1000);
    settings.randomness = ReadSetting(key, L"Randomness", settings.randomness, 0, 100);
    int legacyLength = ReadSetting(key, L"Size", 0, 0, 1000);
    if (legacyLength > 0)
    {
        // Older versions stored the length in pixels.
        int tenths = static_cast<int>(std::lround(legacyLength * 1000.0 / ReferenceScreenHeight()));
        settings.sizeTenths = tenths < 10 ? 10 : (tenths > 100 ? 100 : tenths);
    }
    settings.sizeTenths = ReadSetting(key, L"SizePercentTenths", settings.sizeTenths, 10, 100);
    settings.showFps = ReadSetting(key, L"ShowFps", settings.showFps ? 1 : 0, 0, 1) != 0;
    settings.vsync = ReadSetting(key, L"VSync", settings.vsync ? 1 : 0, 0, 1) != 0;
    settings.frameRateMode = ReadSetting(key, L"FrameRateMode", settings.frameRateMode, 0, kFrameRateModeCount - 1);
    settings.rockCount = ReadSetting(key, L"RockCount", settings.rockCount, 0, 10);
    settings.restrictToMonitor = ReadSetting(key, L"RestrictToMonitor", settings.restrictToMonitor ? 1 : 0, 0, 1) != 0;
    settings.speedVariability = ReadSetting(key, L"SpeedVariability", settings.speedVariability, 0, 10);
    if (legacyCount > 0)
    {
        // Older versions stored an absolute fish count; keep the same coverage at the saved size.
        std::vector<RECT> monitors = EnumerateMonitorRects();
        double freeArea = TotalArea(monitors) - EstimatedRockArea(monitors, settings.rockCount);
        if (freeArea > 0.0)
        {
            settings.fishCoverage = FishCoveragePositionFor(legacyCount * FishFootprint(BoidLengthForSize(settings.sizeTenths)) / freeArea);
        }
    }
    settings.fishCoverage = ReadSetting(key, L"FishCoverage", settings.fishCoverage, 0, 100);
    settings.tailPercent = ReadSetting(key, L"TailPercent", settings.tailPercent, 10, 100);
    int colourCount = ReadSetting(key, L"ColourCount", 0, 0, kMaxFishColours);
    if (colourCount > 0)
    {
        settings.colourCount = colourCount;
        for (int index = 0; index < colourCount; ++index)
        {
            wchar_t name[16];
            wsprintf(name, L"Colour%d", index);
            settings.colours[index] = static_cast<COLORREF>(ReadSetting(key, name, static_cast<int>(settings.colours[index]), 0, 0xFFFFFF));
        }
    }
    // Stored offset by 100 because ReadSetting only handles non-negative DWORDs.
    settings.tailShade = ReadSetting(key, L"TailShade", settings.tailShade + 100, 0, 200) - 100;
    // Older versions stored a 0-10 ColourBias; anything above 0 now means sort by colour.
    settings.colourSort = ReadSetting(key, L"ColourSort", ReadSetting(key, L"ColourBias", settings.colourSort ? 1 : 0, 0, 10) > 0 ? 1 : 0, 0, 1) != 0;
    settings.colourAvoid = ReadSetting(key, L"ColourAvoid", settings.colourAvoid ? 1 : 0, 0, 1) != 0;
    settings.sizeVariation = ReadSetting(key, L"SizeVariation", settings.sizeVariation, 100, 300);
    settings.sizeBell = ReadSetting(key, L"SizeBell", settings.sizeBell ? 1 : 0, 0, 1) != 0;
    settings.depthEffect = ReadSetting(key, L"DepthEffect", settings.depthEffect ? 1 : 0, 0, 1) != 0;
    settings.desktopBackground = ReadSetting(key, L"DesktopBackground", settings.desktopBackground ? 1 : 0, 0, 1) != 0;
}

static void LoadSettings()
{
    HKEY key = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return;
    }
    LoadSettingsFromKey(key, g_settings);
    g_settings.randomPreset = ReadSetting(key, L"RandomPreset", g_settings.randomPreset ? 1 : 0, 0, 1) != 0;
    RegCloseKey(key);
}

static void WriteSetting(HKEY key, const wchar_t* name, int value)
{
    DWORD data = static_cast<DWORD>(value);
    RegSetValueEx(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
}

static void SaveSettingsToKey(HKEY key, const Settings& settings)
{
    WriteSetting(key, L"Speed", settings.speedPixelsPerSecond);
    WriteSetting(key, L"FishCoverage", settings.fishCoverage);
    RegDeleteValue(key, L"BoidCount");
    WriteSetting(key, L"Randomness", settings.randomness);
    RegDeleteValue(key, L"Shape");
    WriteSetting(key, L"SizePercentTenths", settings.sizeTenths);
    RegDeleteValue(key, L"Size");
    WriteSetting(key, L"ShowFps", settings.showFps ? 1 : 0);
    WriteSetting(key, L"VSync", settings.vsync ? 1 : 0);
    WriteSetting(key, L"FrameRateMode", settings.frameRateMode);
    RegDeleteValue(key, L"FrameRate");
    WriteSetting(key, L"RockCount", settings.rockCount);
    WriteSetting(key, L"RestrictToMonitor", settings.restrictToMonitor ? 1 : 0);
    WriteSetting(key, L"SpeedVariability", settings.speedVariability);
    WriteSetting(key, L"TailPercent", settings.tailPercent);
    WriteSetting(key, L"ColourCount", settings.colourCount);
    for (int index = 0; index < kMaxFishColours; ++index)
    {
        wchar_t name[16];
        wsprintf(name, L"Colour%d", index);
        if (index < settings.colourCount)
        {
            WriteSetting(key, name, static_cast<int>(settings.colours[index]));
        }
        else
        {
            RegDeleteValue(key, name);
        }
    }
    WriteSetting(key, L"TailShade", settings.tailShade + 100);
    WriteSetting(key, L"ColourSort", settings.colourSort ? 1 : 0);
    RegDeleteValue(key, L"ColourBias");
    WriteSetting(key, L"ColourAvoid", settings.colourAvoid ? 1 : 0);
    WriteSetting(key, L"SizeVariation", settings.sizeVariation);
    WriteSetting(key, L"SizeBell", settings.sizeBell ? 1 : 0);
    WriteSetting(key, L"DepthEffect", settings.depthEffect ? 1 : 0);
    WriteSetting(key, L"DesktopBackground", settings.desktopBackground ? 1 : 0);
}

static void SaveSettings()
{
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition) != ERROR_SUCCESS)
    {
        return;
    }
    SaveSettingsToKey(key, g_settings);
    WriteSetting(key, L"RandomPreset", g_settings.randomPreset ? 1 : 0);
    RegCloseKey(key);
}

// Presets: complete copies of the settings, one registry key each under Software\Shoal\Presets\<name>.
static const wchar_t* kPresetsKey = L"Software\\Shoal\\Presets";

static std::vector<std::wstring> ListPresets()
{
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, kPresetsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return names;
    }
    wchar_t name[256];
    for (DWORD index = 0;; ++index)
    {
        DWORD length = 256;
        if (RegEnumKeyEx(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        names.push_back(name);
    }
    RegCloseKey(key);
    return names;
}

static bool LoadPreset(const std::wstring& name, Settings& settings)
{
    HKEY key = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, (std::wstring(kPresetsKey) + L"\\" + name).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    LoadSettingsFromKey(key, settings);
    RegCloseKey(key);
    return true;
}

static bool SavePreset(const std::wstring& name, const Settings& settings)
{
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, (std::wstring(kPresetsKey) + L"\\" + name).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition) != ERROR_SUCCESS)
    {
        return false;
    }
    SaveSettingsToKey(key, settings);
    RegCloseKey(key);
    return true;
}

static bool DeletePreset(const std::wstring& name)
{
    HKEY key = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, kPresetsKey, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    bool deleted = RegDeleteKey(key, name.c_str()) == ERROR_SUCCESS;
    RegCloseKey(key);
    return deleted;
}

static std::wstring g_activePreset;

// With RandomPreset on, replaces g_settings with a random saved preset, avoiding the one used last time when there is a choice.
static void ApplyRandomPreset()
{
    std::vector<std::wstring> names = ListPresets();
    if (!g_settings.randomPreset || names.empty())
    {
        return;
    }
    wchar_t last[256] = {};
    DWORD lastSize = sizeof(last) - sizeof(wchar_t);
    RegGetValue(HKEY_CURRENT_USER, kSettingsKey, L"LastPreset", RRF_RT_REG_SZ, nullptr, last, &lastSize);
    if (names.size() > 1)
    {
        names.erase(std::remove(names.begin(), names.end(), std::wstring(last)), names.end());
    }
    std::wstring name = names[static_cast<size_t>(rand()) % names.size()];
    if (!LoadPreset(name, g_settings))
    {
        return;
    }
    g_activePreset = name;
    HKEY key = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueEx(key, L"LastPreset", 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()), static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static double GetRefreshRateForMonitor(HMONITOR monitor)
{
    MONITORINFOEX monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfo(monitor, &monitorInfo))
    {
        return 60.0;
    }

    // EnumDisplaySettings rounds fractional rates down (71.925 Hz reports as 71), which makes 1x pacing
    // drift against the real vblank, so prefer the exact rational rate from the display configuration.
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS && pathCount > 0)
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) == ERROR_SUCCESS)
        {
            for (UINT32 i = 0; i < pathCount; ++i)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
                sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sourceName.header.size = sizeof(sourceName);
                sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
                sourceName.header.id = paths[i].sourceInfo.id;
                const DISPLAYCONFIG_RATIONAL& rate = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS &&
                    std::wcscmp(sourceName.viewGdiDeviceName, monitorInfo.szDevice) == 0 &&
                    rate.Denominator != 0 && rate.Numerator != 0)
                {
                    return static_cast<double>(rate.Numerator) / rate.Denominator;
                }
            }
        }
    }

    DEVMODE mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettings(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode))
    {
        return 60;
    }

    return (mode.dmDisplayFrequency > 1) ? mode.dmDisplayFrequency : 60.0;
}

static HRESULT CompileShader(const char* source, const char* entryPoint, const char* target, ID3DBlob** blobOut)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entryPoint, target, flags, 0, &shaderBlob, &errorBlob);
    if (FAILED(hr) && errorBlob)
    {
        OutputDebugStringA(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
        errorBlob->Release();
    }

    *blobOut = shaderBlob;
    return hr;
}

// HLSL shared by the simulation compute shaders. Boid must match GpuBoid, Simulation must match GpuSimulationConstants.
static const char* kShaderBoidStruct =
    "struct Boid { float2 position; float2 velocity; float turnRate; float turnTarget; float independentUntil; float nextTurnChange; float heading; float currentSpeed; float stopped; float breakawayHeading; float cruiseSpeed; float yawRate; float swimPhase; float thrust; float fishLength; float depth; };\n"
    // Tail beats per second rise with speed in body lengths per second; each fish gets its own +-10% variation.
    "float swimOmega(float fishSpeed, float fishLength, uint i) { return 6.2831853f * clamp(1.5f * fishSpeed / max(fishLength, 1.0f), 0.8f, 5.0f) * (0.9f + 0.2f * frac((float)i * 0.61803399f)); }\n";

static const char* kSimulationShaderCommon =
    "struct Rock { float2 position; float radius; float pointCount; float points[12]; };\n"
    "cbuffer Simulation : register(b0) { float deltaSeconds; float speed; float boidCount; float randomness; float monitorCount; float timeSeconds; float rockCount; float restrictToMonitor; float baseLength; float speedVariability; float colourCount; float colourBias; float colourAvoid; float3 padding; };\n"
    "float sizeSpeedFactor(float fishLength) { return pow(max(fishLength, 1.0f) / max(baseLength, 1.0f), -0.2f); }\n"
    "float sizeAccelerationFactor(float fishLength) { return pow(max(fishLength, 1.0f) / max(baseLength, 1.0f), -0.5f); }\n"
    "bool pointInside(Rock rock, float2 p) { bool inside = false; for (uint i = 0, j = (uint)rock.pointCount - 1; i < (uint)rock.pointCount; j = i++) { float2 a = rock.position + float2(rock.points[i * 2], rock.points[i * 2 + 1]); float2 b = rock.position + float2(rock.points[j * 2], rock.points[j * 2 + 1]); if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside; } return inside; }\n"
    "void nearestOnRock(Rock rock, float2 p, out float distance, out float2 normal) { distance = 1e9f; float2 nearest = p; for (uint k = 0; k < (uint)rock.pointCount; ++k) { uint q = (k + 1) % (uint)rock.pointCount; float2 a = rock.position + float2(rock.points[k * 2], rock.points[k * 2 + 1]); float2 z = rock.position + float2(rock.points[q * 2], rock.points[q * 2 + 1]); float2 edge = z - a; float t = saturate(dot(p - a, edge) / max(dot(edge, edge), 0.0001f)); float2 candidate = a + edge * t; float d = length(p - candidate); if (d < distance) { distance = d; nearest = candidate; } } normal = distance > 0.0001f ? (p - nearest) / distance : normalize(p - rock.position + 0.001f); if (pointInside(rock, p)) { normal = -normal; distance = -distance; } }\n";

static bool InitializeGpuSimulation(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_gpuBoidComputeShader)
    {
        return true;
    }

    const std::string shaderSource = std::string(kShaderBoidStruct) + kSimulationShaderCommon +
        "struct Monitor { float left; float top; float right; float bottom; };\n"
        "RWStructuredBuffer<Boid> Boids : register(u0);\n"
        "StructuredBuffer<Monitor> Monitors : register(t0);\n"
        "float randomValue(uint seed) { return frac(sin((float)seed * 12.9898f) * 43758.5453f); }\n"
        "uint pcgHash(uint v) { uint s = v * 747796405u + 2891336453u; uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return (w >> 22u) ^ w; }\n"
        "float hash01(uint a, uint b) { return (float)(pcgHash(a ^ pcgHash(b)) >> 8) * (1.0f / 16777216.0f); }\n"
        // Must match the colour pick in the fish vertex shader.
        "uint fishColour(uint k) { return ((k * 2654435761u) >> 16) % max((uint)colourCount, 1u); }\n"
        "bool adjacent(Monitor m, float coordinate, bool horizontal, bool positive) { if (restrictToMonitor > 0.5f) return false; for (uint k = 0; k < monitorCount; ++k) { Monitor o = Monitors[k]; if (horizontal) { if ((positive && o.left == m.right || !positive && o.right == m.left) && coordinate >= o.top && coordinate < o.bottom) return true; } else { if ((positive && o.top == m.bottom || !positive && o.bottom == m.top) && coordinate >= o.left && coordinate < o.right) return true; } } return false; }\n"
        "void wallTerm(bool blocked, float distance, float2 normal, float lookahead, float2 dir, inout float2 push, inout float nearest, inout float2 nearestNormal) { if (!blocked) return; float approach = saturate(-dot(dir, normal)); float reach = lookahead * (0.5f + 0.5f * approach); if (distance >= reach) return; float w = 1.0f - max(distance, 0.0f) / reach; push += normal * w * w * 2.0f; if (distance < nearest) { nearest = distance; nearestNormal = normal; } }\n"
        "StructuredBuffer<Rock> Rocks : register(t1);\n"
        "void finishSwim(inout Boid b, float startHeading, float startSpeed, uint i) { float dt = max(deltaSeconds, 0.0001f); float rate = atan2(sin(b.heading - startHeading), cos(b.heading - startHeading)) / dt; b.yawRate += (rate - b.yawRate) * saturate(deltaSeconds * 6.0f); float relativeAcceleration = (b.currentSpeed - startSpeed) / dt / max(b.currentSpeed, b.fishLength); float thrustTarget = relativeAcceleration >= 0.0f ? 1.0f + min(relativeAcceleration * 3.0f, 0.6f) : max(0.03f, 1.0f + relativeAcceleration * 12.0f); b.thrust += (thrustTarget - b.thrust) * saturate(deltaSeconds * (thrustTarget > b.thrust ? 6.0f : 3.0f)); b.swimPhase += swimOmega(b.currentSpeed, b.fishLength, i) * clamp(0.25f + 0.75f * b.thrust, 0.25f, 1.45f) * deltaSeconds; if (b.swimPhase > 6.2831853f) b.swimPhase -= 6.2831853f; }\n"
        "[numthreads(64, 1, 1)] void CSMain(uint3 id : SV_DispatchThreadID) { uint i = id.x; if (i >= boidCount) return; Boid b = Boids[i]; float boidLength = b.fishLength; float startHeading = b.heading; float startSpeed = b.currentSpeed; if (b.stopped > 0.5f) { float turn = atan2(sin(b.turnTarget - b.heading), cos(b.turnTarget - b.heading)); float turnStep = min(abs(turn), 2.8f * deltaSeconds); b.heading += turnStep * (turn < 0 ? -1.0f : 1.0f); b.currentSpeed = speed * 0.01f; b.velocity = float2(cos(b.heading), sin(b.heading)) * b.currentSpeed; if (abs(turn) < 0.08f) b.stopped = 0.0f; finishSwim(b, startHeading, startSpeed, i); Boids[i] = b; return; } float2 center = 0; float2 average = 0; float2 separation = 0; float neighbors = 0; float sizeScale = boidLength / 20.0f; float bodyOffset = (boidLength - 20.0f) * 0.5f; float nearestOther = 1e9f; float2 nearestOtherPosition = 0; float neighborLogSpeed = 0; uint ownColour = fishColour(i); bool avoidColours = colourAvoid > 0.5f && colourBias > 0.0f; float2 colourPush = 0; float nearestColour = 1e9f; float2 nearestColourNormal = 0; float2 startDir = float2(cos(b.heading), sin(b.heading)); float minFishLookahead = 40.0f * max(1.0f, sqrt(sizeScale)); float fishLookahead = clamp(max(speed, b.currentSpeed) * 0.3f, minFishLookahead, max(minFishLookahead, 120.0f)); float colourReach = avoidColours ? fishLookahead + boidLength * 0.5f : 0.0f; for (uint j = 0; j < boidCount; ++j) { if (i == j) continue; Boid o = Boids[j]; float2 d = o.position - b.position; float distance = max(length(d), 0.0001f); float pairScale = (boidLength + o.fishLength) / 40.0f; float neighborRadius = 90.0f * pairScale; float separationRadius = 46.0f * pairScale; float affinity = 1.0f; if (colourBias > 0.0f && distance < max(max(nearestOther, neighborRadius), colourReach) && fishColour(j) != ownColour) { affinity = 1.0f - colourBias; if (avoidColours) wallTerm(true, distance - (boidLength + o.fishLength) * 0.25f, -d / distance, fishLookahead, startDir, colourPush, nearestColour, nearestColourNormal); } float seekDistance = distance / max(affinity, 0.0001f); if (affinity > 0.0f && distance >= neighborRadius && seekDistance < nearestOther && (restrictToMonitor < 0.5f || j % (uint)monitorCount == i % (uint)monitorCount)) { nearestOther = seekDistance; nearestOtherPosition = o.position; } if (distance < neighborRadius && affinity > 0.0f) { center += o.position * affinity; average += o.velocity * affinity; neighborLogSpeed += log(max(o.cruiseSpeed / sizeSpeedFactor(o.fishLength), 0.5f)) * affinity; neighbors += affinity; } if (distance < separationRadius) separation -= d / distance * ((separationRadius - distance) / separationRadius) * 1.4f; } float chaos = randomness * 0.01f; uint frameSeed = (uint)(timeSeconds * 1000.0f); if (timeSeconds >= b.independentUntil && chaos > 0 && hash01(i, frameSeed) < chaos * chaos * deltaSeconds) { b.independentUntil = timeSeconds + 1.5f + chaos * 3.5f + hash01(i ^ 0x68bc21ebu, frameSeed) * 1.5f; float offset = 0.6f + hash01(i ^ 0x02e5be93u, frameSeed) * 1.8f; b.breakawayHeading = b.heading + (hash01(i ^ 0x967a889bu, frameSeed) < 0.5f ? -offset : offset); } float frameScale = deltaSeconds * 60.0f; if (neighbors > 0.0001f && timeSeconds >= b.independentUntil) { float groupWeight = saturate(neighbors); center /= neighbors; average /= neighbors; b.velocity += (center - b.position) * (0.0012f / sizeScale) * groupWeight * frameScale; b.velocity += (average - b.velocity) * (1.0f - pow(0.94f, frameScale)) * groupWeight; } float seekWeight = 0.0f; if (neighbors < 4.0f && timeSeconds >= b.independentUntil && nearestOther < 1e8f) { seekWeight = (1.0f - neighbors * 0.25f) * (1.0f - chaos); b.velocity += normalize(nearestOtherPosition - b.position) * max(speed, b.currentSpeed) * 0.015f * seekWeight * frameScale; } b.velocity += separation * 0.12f * max(1.0f, sizeScale) * frameScale; uint monitorIndex = 0xffffffff; Monitor m; for (uint k = 0; k < monitorCount; ++k) { Monitor candidate = Monitors[k]; if (b.position.x >= candidate.left && b.position.x < candidate.right && b.position.y >= candidate.top && b.position.y < candidate.bottom) { monitorIndex = k; m = candidate; break; } } if (monitorIndex == 0xffffffff && monitorCount > 0) { float best = 1e30f; for (uint k = 0; k < monitorCount; ++k) { Monitor candidate = Monitors[k]; float2 outside = max(float2(candidate.left - b.position.x, candidate.top - b.position.y), float2(b.position.x - candidate.right, b.position.y - candidate.bottom)); float gap = length(max(outside, 0.0f)); if (gap < best) { best = gap; monitorIndex = k; m = candidate; } } } float2 wallPush = 0; float nearestWall = 1e9f; float2 nearestNormal = 0; float2 moveDir = length(b.velocity) > 0.001f ? normalize(b.velocity) : float2(cos(b.heading), sin(b.heading)); float comfortPeriod = 3.0f + hash01(i, 0x9e3779b9u) * 4.0f; float comfortPhase = timeSeconds / comfortPeriod + hash01(i, 0x7f4a7c15u); uint comfortStep = (uint)comfortPhase; float comfortBlend = smoothstep(0.0f, 1.0f, frac(comfortPhase)); float comfort = 0.55f + 0.6f * lerp(hash01(i ^ 0x5bd1e995u, comfortStep), hash01(i ^ 0x5bd1e995u, comfortStep + 1), comfortBlend); if (monitorIndex != 0xffffffff) { float minLookahead = 48.0f * max(1.0f, sqrt(sizeScale)); float lookahead = comfort * clamp(max(speed, b.currentSpeed) * 0.3f, minLookahead, max(minLookahead, 0.2f * min(m.right - m.left, m.bottom - m.top))); wallTerm(!adjacent(m, b.position.y, true, false), b.position.x - m.left - bodyOffset, float2(1, 0), lookahead, moveDir, wallPush, nearestWall, nearestNormal); wallTerm(!adjacent(m, b.position.y, true, true), m.right - b.position.x - bodyOffset, float2(-1, 0), lookahead, moveDir, wallPush, nearestWall, nearestNormal); wallTerm(!adjacent(m, b.position.x, false, false), b.position.y - m.top - bodyOffset, float2(0, 1), lookahead, moveDir, wallPush, nearestWall, nearestNormal); wallTerm(!adjacent(m, b.position.x, false, true), m.bottom - b.position.y - bodyOffset, float2(0, -1), lookahead, moveDir, wallPush, nearestWall, nearestNormal); } float minRockLookahead = 40.0f * max(1.0f, sqrt(sizeScale)); float rockLookahead = clamp(max(speed, b.currentSpeed) * 0.3f, minRockLookahead, max(minRockLookahead, 120.0f)); for (uint r = 0; r < (uint)rockCount; ++r) { float rockDistance; float2 rockNormal; nearestOnRock(Rocks[r], b.position, rockDistance, rockNormal); wallTerm(true, rockDistance - bodyOffset, rockNormal, rockLookahead, moveDir, wallPush, nearestWall, nearestNormal); } { float pushNow = length(wallPush); if (pushNow > 0.0001f && timeSeconds >= b.independentUntil) { float2 awayN = wallPush / pushNow; float into = -dot(moveDir, awayN); if (into > 0.2f || hash01(i ^ 0xc2b2ae35u, frameSeed) < 1.2f * deltaSeconds) { float2 along = moveDir - awayN * dot(moveDir, awayN); along = dot(along, along) > 0.0004f ? normalize(along) : float2(-awayN.y, awayN.x) * ((i & 1) ? 1.0f : -1.0f); float outAngle = 0.35f + hash01(i ^ 0x27220a95u, frameSeed) * 0.7f; float2 rebound = along * cos(outAngle) + awayN * sin(outAngle); b.breakawayHeading = atan2(rebound.y, rebound.x); b.independentUntil = timeSeconds + 0.6f + hash01(i ^ 0x165667b1u, frameSeed + 7u) * 0.8f; } } } if (timeSeconds >= b.nextTurnChange) { b.turnTarget = (randomValue(i + (uint)(timeSeconds * 13.0f)) * 1.4f) - 0.7f; b.nextTurnChange = timeSeconds + 0.7f + randomValue(i + (uint)(timeSeconds * 17.0f)) * 1.1f; } b.turnRate += (b.turnTarget - b.turnRate) * 1.8f * deltaSeconds; float velocityLength = length(b.velocity); if (velocityLength > 0.001f) { b.heading = atan2(b.velocity.y, b.velocity.x) + b.turnRate * deltaSeconds; } if (timeSeconds < b.independentUntil) { float away = atan2(sin(b.breakawayHeading - b.heading), cos(b.breakawayHeading - b.heading)); b.heading += clamp(away, -1.5f * deltaSeconds, 1.5f * deltaSeconds); } float pushLength = length(wallPush); float variability = speedVariability * 0.1f; float cruiseSpeed = speed; if (variability > 0.0f) { float moodPeriod = 1.5f + hash01(i, 0x3c6ef372u) * 3.5f; uint moodIndex = (uint)((timeSeconds + hash01(i, 0x1b873593u) * moodPeriod) / moodPeriod); float w = hash01(i ^ 0x27d4eb2fu, moodIndex); float moodSign = hash01(i ^ 0x165667b1u, moodIndex) < 0.5f + 0.4f * seekWeight ? 1.0f : -1.0f; bool excursion = hash01(i ^ 0x85ebca6bu, moodIndex) < 0.3f + 0.5f * seekWeight; float mood = moodSign * (0.1f + 0.9f * w * w * (1.0f - 0.7f * seekWeight)); float ownSpeed = excursion ? speed * pow(max(mood >= 0.0f ? 1000.0f / speed : speed, 1.0f), mood * variability) : speed * (1.0f + (w * 2.0f - 1.0f) * 0.12f * variability); float share = min(neighbors * 0.25f, 1.0f) * 0.75f; cruiseSpeed = lerp(ownSpeed, neighbors > 0 ? exp(neighborLogSpeed / neighbors) : ownSpeed, share) * sizeSpeedFactor(boidLength); cruiseSpeed *= 1.0f - 0.4f * variability * saturate(pushLength); cruiseSpeed = clamp(cruiseSpeed, 1.0f, 1000.0f); } float minimumSpeed = cruiseSpeed * 0.3f; float colourPushLength = length(colourPush); if (colourPushLength > 2.0f) colourPush *= 2.0f / colourPushLength; float2 steerPush = wallPush + colourPush * colourBias; float steerLength = length(steerPush); if (steerLength > 0.0001f) { float2 awayNormal = steerPush / steerLength; float urgency = saturate(steerLength); float2 dir = float2(cos(b.heading), sin(b.heading)); float2 tangent = dir - awayNormal * min(dot(dir, awayNormal), 0.0f); if (dot(tangent, tangent) < 0.0004f) tangent = float2(-awayNormal.y, awayNormal.x) * ((i & 1) ? 1.0f : -1.0f); float2 desired = normalize(normalize(tangent) + awayNormal * (0.15f + urgency)); float turn = atan2(desired.y * dir.x - desired.x * dir.y, dot(desired, dir)); float maxTurn = (2.0f + urgency * 10.0f) * deltaSeconds; b.heading += clamp(turn, -maxTurn, maxTurn); if (urgency > 0.25f) b.breakawayHeading = b.heading; float closingSpeed = max(0.0f, -dot(float2(cos(b.heading), sin(b.heading)), nearestNormal)) * b.currentSpeed; float brakingAcceleration = max(speed, b.currentSpeed) * 3.0f; float stoppingDistance = closingSpeed * closingSpeed / max(2.0f * brakingAcceleration, 0.001f); if (closingSpeed > 0.0f && nearestWall < stoppingDistance + 8.0f) b.currentSpeed = max(minimumSpeed, b.currentSpeed - brakingAcceleration * deltaSeconds); } b.cruiseSpeed = cruiseSpeed; float agility = sizeAccelerationFactor(boidLength); b.currentSpeed = b.currentSpeed > cruiseSpeed ? max(cruiseSpeed, b.currentSpeed - max(speed * 0.5f, (b.currentSpeed - cruiseSpeed) * 1.5f) * agility * deltaSeconds) : min(cruiseSpeed, b.currentSpeed + max(speed, cruiseSpeed) * 1.5f * agility * deltaSeconds); b.currentSpeed = max(b.currentSpeed, minimumSpeed); float2 heading2 = float2(cos(b.heading), sin(b.heading)); b.position += heading2 * b.currentSpeed * deltaSeconds; if (monitorIndex != 0xffffffff) { bool clamped = false; float extentX = max(1.0f, boidLength * (0.5f * abs(heading2.x) + 0.125f * abs(heading2.y))); float extentY = max(1.0f, boidLength * (0.5f * abs(heading2.y) + 0.125f * abs(heading2.x))); if (b.position.x < m.left + extentX && !adjacent(m, b.position.y, true, false)) { b.position.x = m.left + extentX; heading2.x = max(heading2.x, 0.15f); clamped = true; } if (b.position.x > m.right - extentX && !adjacent(m, b.position.y, true, true)) { b.position.x = m.right - extentX; heading2.x = min(heading2.x, -0.15f); clamped = true; } if (b.position.y < m.top + extentY && !adjacent(m, b.position.x, false, false)) { b.position.y = m.top + extentY; heading2.y = max(heading2.y, 0.15f); clamped = true; } if (b.position.y > m.bottom - extentY && !adjacent(m, b.position.x, false, true)) { b.position.y = m.bottom - extentY; heading2.y = min(heading2.y, -0.15f); clamped = true; } if (clamped) { heading2 = normalize(heading2); b.heading = atan2(heading2.y, heading2.x); } } b.stopped = 0.0f; b.velocity = heading2 * b.currentSpeed; finishSwim(b, startHeading, startSpeed, i); Boids[i] = b; }\n"

    ;
    ID3DBlob* computeBlob = nullptr;
    if (FAILED(CompileShader(shaderSource.c_str(), "CSMain", "cs_5_0", &computeBlob)))
    {
        return false;
    }

    HRESULT hr = device->CreateComputeShader(computeBlob->GetBufferPointer(), computeBlob->GetBufferSize(), nullptr, &g_gpuBoidComputeShader);
    computeBlob->Release();
    if (FAILED(hr)) return false;

    const std::string rockShaderSource = std::string(kShaderBoidStruct) + kSimulationShaderCommon +
        "RWStructuredBuffer<Boid> Boids : register(u0); StructuredBuffer<Rock> Rocks : register(t0);\n"
        "[numthreads(64, 1, 1)] void CSMain(uint3 id : SV_DispatchThreadID) { uint i = id.x; if (i >= boidCount) return; Boid b = Boids[i]; float boidLength = b.fishLength; for (uint r = 0; r < rockCount; ++r) { float distance; float2 normal; nearestOnRock(Rocks[r], b.position, distance, normal); float along = abs(dot(float2(cos(b.heading), sin(b.heading)), normal)); float clearance = max(0.5f, boidLength * (0.5f * along + 0.125f * sqrt(saturate(1.0f - along * along)))); if (distance < clearance) { b.position += normal * (clearance - distance); float2 dir = float2(cos(b.heading), sin(b.heading)); dir -= normal * min(dot(dir, normal), 0.0f); dir += normal * 0.15f; dir = normalize(dir); b.heading = atan2(dir.y, dir.x); b.velocity = dir * b.currentSpeed; b.stopped = 0.0f; break; } } Boids[i] = b; }\n";
    ID3DBlob* rockShaderBlob = nullptr;
    if (SUCCEEDED(CompileShader(rockShaderSource.c_str(), "CSMain", "cs_5_0", &rockShaderBlob)))
    {
        hr = device->CreateComputeShader(rockShaderBlob->GetBufferPointer(), rockShaderBlob->GetBufferSize(), nullptr, &g_gpuRockAvoidComputeShader);
        rockShaderBlob->Release();
    }

    D3D11_BUFFER_DESC boidDesc = {};
    boidDesc.ByteWidth = sizeof(GpuBoid) * static_cast<UINT>(g_boids.size());
    boidDesc.Usage = D3D11_USAGE_DEFAULT;
    boidDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    boidDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    boidDesc.StructureByteStride = sizeof(GpuBoid);
    std::vector<GpuBoid> initialBoids;
    for (size_t i = 0; i < g_boids.size(); ++i)
    {
        const Boid& boid = g_boids[i];
        float swimPhase = std::fmod(static_cast<float>(i) * 2.39996323f, 6.2831853f);
        GpuBoid gpu = { boid.x, boid.y, boid.vx, boid.vy, boid.swimTurnRate, boid.swimTurnTarget, 0.0f, 0.0f, std::atan2(boid.vy, boid.vx), static_cast<float>(g_settings.speedPixelsPerSecond), 0.0f, boid.breakawayHeading, static_cast<float>(g_settings.speedPixelsPerSecond), 0.0f, swimPhase, 1.0f, boid.length, boid.depth };
        initialBoids.push_back(gpu);
    }
    D3D11_SUBRESOURCE_DATA boidData = { initialBoids.data(), 0, 0 };
    if (FAILED(device->CreateBuffer(&boidDesc, &boidData, &g_gpuBoidBuffer))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC boidViewDesc = {};
    boidViewDesc.Format = DXGI_FORMAT_UNKNOWN;
    boidViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    boidViewDesc.Buffer.NumElements = static_cast<UINT>(g_boids.size());
    if (FAILED(device->CreateShaderResourceView(g_gpuBoidBuffer, &boidViewDesc, &g_gpuBoidView))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC boidUavDesc = {};
    boidUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    boidUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    boidUavDesc.Buffer.NumElements = static_cast<UINT>(g_boids.size());
    if (FAILED(device->CreateUnorderedAccessView(g_gpuBoidBuffer, &boidUavDesc, &g_gpuBoidUav))) return false;

    D3D11_BUFFER_DESC monitorDesc = {};
    monitorDesc.ByteWidth = sizeof(GpuMonitor) * static_cast<UINT>(g_monitorBounds.size());
    monitorDesc.Usage = D3D11_USAGE_DEFAULT;
    monitorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    monitorDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    monitorDesc.StructureByteStride = sizeof(GpuMonitor);
    std::vector<GpuMonitor> monitors;
    for (const RECT& monitor : g_monitorBounds) monitors.push_back({ static_cast<float>(monitor.left), static_cast<float>(monitor.top), static_cast<float>(monitor.right), static_cast<float>(monitor.bottom) });
    D3D11_SUBRESOURCE_DATA monitorData = { monitors.data(), 0, 0 };
    if (FAILED(device->CreateBuffer(&monitorDesc, &monitorData, &g_gpuMonitorBuffer))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC monitorViewDesc = {};
    monitorViewDesc.Format = DXGI_FORMAT_UNKNOWN;
    monitorViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    monitorViewDesc.Buffer.NumElements = static_cast<UINT>(monitors.size());
    if (FAILED(device->CreateShaderResourceView(g_gpuMonitorBuffer, &monitorViewDesc, &g_gpuMonitorView))) return false;

    if (!g_rocks.empty())
    {
        std::vector<GpuRock> gpuRocks;
        for (const Rock& rock : g_rocks)
        {
            GpuRock gpu = {};
            gpu.x = rock.x;
            gpu.y = rock.y;
            gpu.radius = rock.radius;
            gpu.pointCount = static_cast<float>(rock.pointCount);
            for (int point = 0; point < rock.pointCount; ++point)
            {
                gpu.points[point * 2] = static_cast<float>(rock.points[point].x);
                gpu.points[point * 2 + 1] = static_cast<float>(rock.points[point].y);
            }
            gpuRocks.push_back(gpu);
        }
        D3D11_BUFFER_DESC rockDesc = {};
        rockDesc.ByteWidth = sizeof(GpuRock) * static_cast<UINT>(gpuRocks.size());
        rockDesc.Usage = D3D11_USAGE_DEFAULT;
        rockDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        rockDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        rockDesc.StructureByteStride = sizeof(GpuRock);
        D3D11_SUBRESOURCE_DATA rockData = { gpuRocks.data(), 0, 0 };
        if (FAILED(device->CreateBuffer(&rockDesc, &rockData, &g_gpuRockBuffer))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC rockViewDesc = {};
        rockViewDesc.Format = DXGI_FORMAT_UNKNOWN;
        rockViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        rockViewDesc.Buffer.NumElements = static_cast<UINT>(gpuRocks.size());
        if (FAILED(device->CreateShaderResourceView(g_gpuRockBuffer, &rockViewDesc, &g_gpuRockView))) return false;
    }

    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(GpuSimulationConstants);
    constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&constantsDesc, nullptr, &g_gpuConstantsBuffer))) return false;

    g_sharedDevice = device;
    g_sharedDevice->AddRef();
    g_sharedContext = context;
    g_sharedContext->AddRef();
    return true;
}

static void WriteDebugLogSamples();

static void UpdateGpuBoids(float deltaSeconds)
{
    g_debugLogTime += deltaSeconds;
    g_gpuSimulationTime += deltaSeconds;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_sharedContext->Map(g_gpuConstantsBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        return;
    }
    GpuSimulationConstants constants = {
        deltaSeconds,
        static_cast<float>(g_settings.speedPixelsPerSecond),
        static_cast<float>(g_boids.size()),
        static_cast<float>(g_settings.randomness),
        static_cast<float>(g_monitorBounds.size()),
        g_gpuSimulationTime,
        static_cast<float>(g_rocks.size()),
        g_settings.restrictToMonitor ? 1.0f : 0.0f,
        static_cast<float>(g_settings.boidLength),
        static_cast<float>(g_settings.speedVariability),
        static_cast<float>(g_settings.colourCount),
        g_settings.colourCount > 1 && g_settings.colourSort ? 1.0f : 0.0f,
        g_settings.colourSort && g_settings.colourAvoid ? 1.0f : 0.0f,
        { 0.0f, 0.0f, 0.0f }
    };
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    g_sharedContext->Unmap(g_gpuConstantsBuffer, 0);

    g_sharedContext->CSSetShader(g_gpuBoidComputeShader, nullptr, 0);
    g_sharedContext->CSSetConstantBuffers(0, 1, &g_gpuConstantsBuffer);
    ID3D11ShaderResourceView* simulationViews[2] = { g_gpuMonitorView, g_gpuRockView };
    g_sharedContext->CSSetShaderResources(0, 2, simulationViews);
    g_sharedContext->CSSetUnorderedAccessViews(0, 1, &g_gpuBoidUav, nullptr);
    g_sharedContext->Dispatch((static_cast<UINT>(g_boids.size()) + 63) / 64, 1, 1);

    if (g_gpuRockAvoidComputeShader && g_gpuRockView)
    {
        g_sharedContext->CSSetShader(g_gpuRockAvoidComputeShader, nullptr, 0);
        g_sharedContext->CSSetShaderResources(0, 1, &g_gpuRockView);
        g_sharedContext->CSSetUnorderedAccessViews(0, 1, &g_gpuBoidUav, nullptr);
        g_sharedContext->Dispatch((static_cast<UINT>(g_boids.size()) + 63) / 64, 1, 1);
    }

    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView* nullSrvs[2] = {};
    g_sharedContext->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    g_sharedContext->CSSetShaderResources(0, 2, nullSrvs);
    WriteDebugLogSamples();
}

static bool IsPointInsideRock(const Rock& rock, float x, float y)
{
    bool inside = false;
    for (int point = 0, previous = rock.pointCount - 1; point < rock.pointCount; previous = point++)
    {
        float x1 = rock.x + static_cast<float>(rock.points[point].x);
        float y1 = rock.y + static_cast<float>(rock.points[point].y);
        float x2 = rock.x + static_cast<float>(rock.points[previous].x);
        float y2 = rock.y + static_cast<float>(rock.points[previous].y);
        if ((y1 > y) != (y2 > y) && x < (x2 - x1) * (y - y1) / (y2 - y1) + x1)
        {
            inside = !inside;
        }
    }
    return inside;
}

static float DistanceToRockWall(const Rock& rock, float x, float y)
{
    float closestDistance = 1.0e9f;
    for (int point = 0; point < rock.pointCount; ++point)
    {
        const POINT& first = rock.points[point];
        const POINT& second = rock.points[(point + 1) % rock.pointCount];
        float x1 = rock.x + static_cast<float>(first.x);
        float y1 = rock.y + static_cast<float>(first.y);
        float dx = static_cast<float>(second.x - first.x);
        float dy = static_cast<float>(second.y - first.y);
        float edgeLengthSquared = dx * dx + dy * dy;
        float t = (edgeLengthSquared > 0.001f) ? Clamp(((x - x1) * dx + (y - y1) * dy) / edgeLengthSquared, 0.0f, 1.0f) : 0.0f;
        float distance = Length(x - (x1 + dx * t), y - (y1 + dy * t));
        if (distance < closestDistance)
        {
            closestDistance = distance;
        }
    }
    return closestDistance;
}

// Clear space kept around rocks; larger fish need wider passages.
static float RockBuffer()
{
    return kRockBufferDistance + (std::max)(0.0f, BoidBodyOffset());
}

static bool IsOutsideRockBuffers(float x, float y)
{
    for (const Rock& rock : g_rocks)
    {
        if (IsPointInsideRock(rock, x, y) || DistanceToRockWall(rock, x, y) < RockBuffer())
        {
            return false;
        }
    }
    return true;
}

static void CreateBoids()
{
    g_boids.clear();
    if (g_monitorBounds.empty())
    {
        return;
    }
    if (!g_boidCountOverridden)
    {
        double freeArea = TotalArea(g_monitorBounds);
        for (const Rock& rock : g_rocks)
        {
            freeArea -= RockArea(rock);
        }
        g_settings.boidCount = BoidCountFor(g_settings.fishCoverage, g_settings.boidLength, freeArea,
            SizeAreaFactor(g_settings.sizeVariation, g_settings.sizeBell));
    }

    for (int i = 0; i < g_settings.boidCount; ++i)
    {
        const RECT& monitor = g_monitorBounds[static_cast<size_t>(i) % g_monitorBounds.size()];
        Boid boid;
        // Length = set length * V^u for u in [-1, 1]: uniform for an even spread, the mean of three for a bell curve.
        float u = RandomFloat(-1.0f, 1.0f);
        if (g_settings.sizeBell)
        {
            u = (u + RandomFloat(-1.0f, 1.0f) + RandomFloat(-1.0f, 1.0f)) / 3.0f;
        }
        boid.length = (std::max)(4.0f, static_cast<float>(g_settings.boidLength) * std::pow(g_settings.sizeVariation / 100.0f, u));
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            // RandomFloat can return its maximum, which lies outside the monitor's [left, right) range.
            float inset = boid.length;
            boid.x = RandomFloat(monitor.left + inset, monitor.right - inset);
            boid.y = RandomFloat(monitor.top + inset, monitor.bottom - inset);
            if (IsOutsideRockBuffers(boid.x, boid.y))
            {
                break;
            }
        }
        float angle = RandomFloat(0.0f, 2.0f * 3.14159265f);
        boid.vx = std::cos(angle) * static_cast<float>(g_settings.speedPixelsPerSecond);
        boid.vy = std::sin(angle) * static_cast<float>(g_settings.speedPixelsPerSecond);
        boid.breakawayHeading = angle;
        boid.swimTurnRate = RandomFloat(-0.35f, 0.35f);
        boid.swimTurnTarget = RandomFloat(-0.35f, 0.35f);
        g_boids.push_back(boid);
    }

    // Depth is each fish's rank by length, so a smaller fish is always below a larger one; equal lengths are ordered randomly.
    std::vector<std::pair<float, float>> order;
    order.reserve(g_boids.size());
    for (const Boid& boid : g_boids)
    {
        order.emplace_back(boid.length, RandomFloat(0.0f, 1.0f));
    }
    std::vector<size_t> ranked(g_boids.size());
    for (size_t index = 0; index < ranked.size(); ++index)
    {
        ranked[index] = index;
    }
    std::sort(ranked.begin(), ranked.end(), [&order](size_t a, size_t b) { return order[a] < order[b]; });
    for (size_t rank = 0; rank < ranked.size(); ++rank)
    {
        g_boids[ranked[rank]].depth = ranked.size() > 1 ? static_cast<float>(rank) / static_cast<float>(ranked.size() - 1) : 0.5f;
    }
}

static void CreateRocks()
{
    g_rocks.clear();
    const float rockBuffer = RockBuffer();
    const float bufferAndEdgeClearance = rockBuffer + 50.0f;
    for (int rockIndex = 0; rockIndex < g_settings.rockCount && !g_monitorBounds.empty(); ++rockIndex)
    {
        bool placed = false;
        for (int attempt = 0; attempt < 200 && !placed; ++attempt)
        {
            const RECT& monitor = g_monitorBounds[static_cast<size_t>(rand()) % g_monitorBounds.size()];
            int monitorWidth = monitor.right - monitor.left;
            int monitorHeight = monitor.bottom - monitor.top;
            float maximumRadius = static_cast<float>((monitorWidth < monitorHeight ? monitorWidth : monitorHeight) / 10);
            if (maximumRadius < 90.0f)
            {
                continue;
            }

            Rock rock = {};
            rock.radius = RandomFloat(90.0f, maximumRadius);
            rock.pointCount = static_cast<int>(RandomFloat(4.0f, 6.999f));
            float largestVertexRadius = 0.0f;
            bool hasMinimumVertexGap = true;
            for (int point = 0; point < rock.pointCount; ++point)
            {
                float angle = (2.0f * 3.14159265f * point) / rock.pointCount;
                float pointRadius = rock.radius * RandomFloat(0.7f, 1.0f);
                rock.points[point].x = static_cast<LONG>(std::cos(angle) * pointRadius);
                rock.points[point].y = static_cast<LONG>(std::sin(angle) * pointRadius);
                if (pointRadius > largestVertexRadius) largestVertexRadius = pointRadius;
            }
            for (int point = 0; point < rock.pointCount; ++point)
            {
                const POINT& first = rock.points[point];
                const POINT& second = rock.points[(point + 1) % rock.pointCount];
                if (Length(static_cast<float>(second.x - first.x), static_cast<float>(second.y - first.y)) < 100.0f)
                {
                    hasMinimumVertexGap = false;
                    break;
                }
            }
            if (!hasMinimumVertexGap)
            {
                continue;
            }

            float centerMargin = largestVertexRadius + bufferAndEdgeClearance;
            float minX = static_cast<float>(monitor.left) + centerMargin;
            float maxX = static_cast<float>(monitor.right) - centerMargin;
            float minY = static_cast<float>(monitor.top) + centerMargin;
            float maxY = static_cast<float>(monitor.bottom) - centerMargin;
            if (minX >= maxX || minY >= maxY)
            {
                continue;
            }
            rock.x = RandomFloat(minX, maxX);
            rock.y = RandomFloat(minY, maxY);

            bool clearOfOtherRocks = true;
            for (const Rock& other : g_rocks)
            {
                float otherLargestRadius = 0.0f;
                for (int point = 0; point < other.pointCount; ++point)
                {
                    float vertexRadius = Length(static_cast<float>(other.points[point].x), static_cast<float>(other.points[point].y));
                    if (vertexRadius > otherLargestRadius) otherLargestRadius = vertexRadius;
                }
                if (Length(rock.x - other.x, rock.y - other.y) < largestVertexRadius + otherLargestRadius + 2.0f * rockBuffer + 50.0f)
                {
                    clearOfOtherRocks = false;
                    break;
                }
            }
            if (clearOfOtherRocks)
            {
                g_rocks.push_back(rock);
                placed = true;
            }
        }
    }
}

static bool HasAdjacentMonitor(const RECT& monitor, float coordinate, bool horizontal, bool positiveDirection)
{
    if (g_settings.restrictToMonitor)
    {
        return false;
    }

    for (const RECT& other : g_monitorBounds)
    {
        if (horizontal)
        {
            bool touchesEdge = positiveDirection ? (other.left == monitor.right) : (other.right == monitor.left);
            if (touchesEdge && coordinate >= static_cast<float>(other.top) && coordinate < static_cast<float>(other.bottom))
            {
                return true;
            }
        }
        else
        {
            bool touchesEdge = positiveDirection ? (other.top == monitor.bottom) : (other.bottom == monitor.top);
            if (touchesEdge && coordinate >= static_cast<float>(other.left) && coordinate < static_cast<float>(other.right))
            {
                return true;
            }
        }
    }

    return false;
}

static int FindMonitorForPoint(float x, float y)
{
    for (size_t i = 0; i < g_monitorBounds.size(); ++i)
    {
        const RECT& monitor = g_monitorBounds[i];
        if (x >= monitor.left && x < monitor.right && y >= monitor.top && y < monitor.bottom)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

// Distance to the nearest monitor edge that blocks movement, matching the simulation's adjacency rules.
static float NearestBlockingWall(int monitorIndex, float x, float y, char* side, float* normalX, float* normalY)
{
    float distance = -1.0f;
    *side = '-';
    *normalX = 0.0f;
    *normalY = 0.0f;
    if (monitorIndex < 0)
    {
        return distance;
    }

    const RECT& monitor = g_monitorBounds[static_cast<size_t>(monitorIndex)];
    auto consider = [&](bool blocked, float candidate, char candidateSide, float nx, float ny)
    {
        if (blocked && (distance < 0.0f || candidate < distance))
        {
            distance = candidate;
            *side = candidateSide;
            *normalX = nx;
            *normalY = ny;
        }
    };
    consider(!HasAdjacentMonitor(monitor, y, true, false), x - monitor.left, 'L', 1.0f, 0.0f);
    consider(!HasAdjacentMonitor(monitor, y, true, true), monitor.right - x, 'R', -1.0f, 0.0f);
    consider(!HasAdjacentMonitor(monitor, x, false, false), y - monitor.top, 'T', 0.0f, 1.0f);
    consider(!HasAdjacentMonitor(monitor, x, false, true), monitor.bottom - y, 'B', 0.0f, -1.0f);
    return distance;
}

static void WriteDebugLogRow(size_t index, float x, float y, float vx, float vy, float heading, float currentSpeed, float stopped,
    bool independent, bool snapshot, float cruiseSpeed, float fishLength, float depth)
{
    int monitorIndex = FindMonitorForPoint(x, y);
    char side = '-';
    float normalX = 0.0f;
    float normalY = 0.0f;
    float wallDistance = NearestBlockingWall(monitorIndex, x, y, &side, &normalX, &normalY);
    float rockDistance = -1.0f;
    for (const Rock& rock : g_rocks)
    {
        float distance = IsPointInsideRock(rock, x, y) ? 0.0f : DistanceToRockWall(rock, x, y);
        if (rockDistance < 0.0f || distance < rockDistance) rockDistance = distance;
    }

    float targetSpeed = static_cast<float>(g_settings.speedPixelsPerSecond);
    bool nearObstacle = (wallDistance >= 0.0f && wallDistance < kDebugLogNearDistance) ||
        (rockDistance >= 0.0f && rockDistance < kDebugLogNearDistance);
    if (!snapshot && index >= kDebugTrackedBoidCount && !nearObstacle && currentSpeed >= (std::min)(targetSpeed, cruiseSpeed) * 0.25f)
    {
        return;
    }

    // Positive values mean the fish faces into the wall; -1 means it faces directly away.
    float facingWall = -(std::cos(heading) * normalX + std::sin(heading) * normalY);
    std::fprintf(g_debugLogFile, "%.3f,%zu,%.2f,%.2f,%.2f,%.2f,%.1f,%.3f,%.4f,%.0f,%d,%c,%.2f,%.3f,%.2f,%d,%d,%.4f,%.1f,%.3f\n",
        g_debugLogTime, index, x, y, vx, vy, heading * 57.2957795f, currentSpeed,
        targetSpeed > 0.0f ? currentSpeed / targetSpeed : 0.0f, stopped, monitorIndex, side, wallDistance,
        facingWall, rockDistance, independent ? 1 : 0, snapshot ? 1 : 0, targetSpeed > 0.0f ? cruiseSpeed / targetSpeed : 0.0f, fishLength, depth);
}

static void WriteDebugLogSamples()
{
    if (!g_debugLogFile || !g_boidLogEnabled || g_debugLogTime < g_debugLogNextSampleTime)
    {
        return;
    }
    g_debugLogNextSampleTime = g_debugLogTime + kDebugLogIntervalSeconds;
    bool snapshot = g_debugLogTime >= g_debugLogNextSnapshotTime;
    if (snapshot)
    {
        g_debugLogNextSnapshotTime = g_debugLogTime + kDebugLogSnapshotSeconds;
    }

    if (g_gpuBoidBuffer && g_sharedDevice && g_sharedContext)
    {
        if (!g_gpuBoidStagingBuffer)
        {
            D3D11_BUFFER_DESC stagingDesc = {};
            g_gpuBoidBuffer->GetDesc(&stagingDesc);
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = 0;
            if (FAILED(g_sharedDevice->CreateBuffer(&stagingDesc, nullptr, &g_gpuBoidStagingBuffer)))
            {
                return;
            }
        }

        g_sharedContext->CopyResource(g_gpuBoidStagingBuffer, g_gpuBoidBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(g_sharedContext->Map(g_gpuBoidStagingBuffer, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            return;
        }
        const GpuBoid* boids = static_cast<const GpuBoid*>(mapped.pData);
        for (size_t i = 0; i < g_boids.size(); ++i)
        {
            const GpuBoid& b = boids[i];
            WriteDebugLogRow(i, b.x, b.y, b.vx, b.vy, b.heading, b.currentSpeed, b.stopped,
                g_gpuSimulationTime < b.independentUntil, snapshot, b.cruiseSpeed, b.fishLength, b.depth);
        }
        g_sharedContext->Unmap(g_gpuBoidStagingBuffer, 0);
    }
    std::fflush(g_debugLogFile);
}

static void WriteFrameLog()
{
    if (g_frameSamples.empty() || !g_debugLogFile)
    {
        return;
    }
    std::fprintf(g_debugLogFile, "# frames: window,end_ms,interval_ms,sim_delta_ms,sim_cpu_ms,render_cpu_ms,present_ms,present_count,present_refresh,sync_refresh,sync_qpc_ms\n");
    const double toMs = 1000.0 / static_cast<double>(g_qpcFrequency.QuadPart);
    LONGLONG origin = g_frameSamples.front().counter;
    for (const FrameSample& sample : g_frameSamples)
    {
        std::fprintf(g_debugLogFile, "#F,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%.3f\n", sample.window,
            (sample.counter - origin) * toMs, sample.intervalMs, sample.simDeltaMs, sample.simCpuMs,
            sample.renderCpuMs, sample.presentMs, sample.presentCount, sample.presentRefreshCount,
            sample.syncRefreshCount, sample.syncQpc ? (sample.syncQpc - origin) * toMs : 0.0);
    }
}

// Finds /<name> or -<name> (case-insensitive) and returns the text right after the name, or nullptr.
static const wchar_t* FindOption(PWSTR commandLine, const wchar_t* name)
{
    if (!commandLine)
    {
        return nullptr;
    }
    size_t nameLength = wcslen(name);
    for (const wchar_t* cursor = commandLine; *cursor; ++cursor)
    {
        if ((cursor[0] == L'/' || cursor[0] == L'-') && _wcsnicmp(cursor + 1, name, nameLength) == 0)
        {
            return cursor + 1 + nameLength;
        }
    }
    return nullptr;
}

static bool HasFlag(PWSTR commandLine, const wchar_t* flag)
{
    const wchar_t* rest = FindOption(commandLine, flag);
    return rest && (*rest == L'\0' || *rest == L' ');
}

// Overrides for test runs without touching the registry, e.g. /speed:500 /randomness:100.
// Returns true when the option is present.
static bool ParseIntOverride(PWSTR commandLine, const wchar_t* option, int minimum, int maximum, int* target)
{
    const wchar_t* value = FindOption(commandLine, option);
    if (!value)
    {
        return false;
    }
    long parsed = wcstol(value, nullptr, 10);
    *target = static_cast<int>(parsed < minimum ? minimum : (parsed > maximum ? maximum : parsed));
    return true;
}

// Parses /size:<percent of screen height>, e.g. /size:1.9, into tenths of a percent.
static void ParseSizeOverride(PWSTR commandLine, int* sizeTenths)
{
    if (const wchar_t* value = FindOption(commandLine, L"size:"))
    {
        long tenths = std::lround(wcstod(value, nullptr) * 10.0);
        *sizeTenths = static_cast<int>(tenths < 10 ? 10 : (tenths > 100 ? 100 : tenths));
    }
}

// Accepts /debuglog or /debuglog:<seconds>; the optional duration makes runs exit automatically.
static bool ParseDebugLogCommand(PWSTR commandLine, DWORD* durationMs)
{
    const wchar_t* value = FindOption(commandLine, L"debuglog");
    if (!value)
    {
        return false;
    }
    *durationMs = (*value == L':') ? static_cast<DWORD>(wcstoul(value + 1, nullptr, 10)) * 1000 : 0;
    return true;
}

static void OpenDebugLog()
{
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileName(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return;
    }
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (!lastSlash)
    {
        return;
    }
    lastSlash[1] = L'\0';

    wchar_t logPath[MAX_PATH] = {};
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    swprintf_s(logPath, L"%slogs", path);
    CreateDirectory(logPath, nullptr);
    swprintf_s(logPath, L"%slogs\\boid-debug-%04u%02u%02u-%02u%02u%02u.csv", path,
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    if (_wfopen_s(&g_debugLogFile, logPath, L"w") != 0 || !g_debugLogFile)
    {
        g_debugLogFile = nullptr;
        return;
    }

    std::fprintf(g_debugLogFile, "# Shoal boid debug log\n");
    std::fprintf(g_debugLogFile, "# settings speed=%d boids=%d randomness=%d size=%d frameRate=%d vsync=%d frameRateMode=%d rocks=%d restrictToMonitor=%d sizePercent=%.1f speedVariability=%d fishCoverage=%d tailPercent=%d colours=%d tailShade=%d colourSort=%d colourAvoid=%d sizeVariation=%d sizeBell=%d depth=%d desktop=%d\n",
        g_settings.speedPixelsPerSecond, g_settings.boidCount, g_settings.randomness,
        g_settings.boidLength, g_settings.frameRate, g_settings.vsync ? 1 : 0, g_settings.frameRateMode, g_settings.rockCount, g_settings.restrictToMonitor ? 1 : 0, g_settings.sizeTenths / 10.0, g_settings.speedVariability, g_boidCountOverridden ? -1 : g_settings.fishCoverage, g_settings.tailPercent, g_settings.colourCount, g_settings.tailShade, g_settings.colourSort ? 1 : 0, g_settings.colourAvoid ? 1 : 0, g_settings.sizeVariation, g_settings.sizeBell ? 1 : 0, g_settings.depthEffect ? 1 : 0, g_settings.desktopBackground ? 1 : 0);
    if (!g_activePreset.empty())
    {
        std::fprintf(g_debugLogFile, "# preset %ls\n", g_activePreset.c_str());
    }
    for (size_t i = 0; i < g_monitorBounds.size(); ++i)
    {
        const RECT& monitor = g_monitorBounds[i];
        std::fprintf(g_debugLogFile, "# monitor %zu left=%ld top=%ld right=%ld bottom=%ld\n",
            i, monitor.left, monitor.top, monitor.right, monitor.bottom);
    }
    for (size_t i = 0; i < g_rocks.size(); ++i)
    {
        const Rock& rock = g_rocks[i];
        std::fprintf(g_debugLogFile, "# rock %zu", i);
        for (int point = 0; point < rock.pointCount; ++point)
        {
            std::fprintf(g_debugLogFile, " %.1f,%.1f", rock.x + rock.points[point].x, rock.y + rock.points[point].y);
        }
        std::fprintf(g_debugLogFile, "\n");
    }
    std::fprintf(g_debugLogFile, "# rows: boids 0-%zu every sample, others only within %.0f px of a wall/rock or below 25%% speed; every boid when snapshot=1\n",
        kDebugTrackedBoidCount - 1, kDebugLogNearDistance);
    std::fprintf(g_debugLogFile, "time_s,boid,x,y,vx,vy,heading_deg,current_speed,speed_fraction,stopped,monitor,wall_side,wall_distance,facing_wall,rock_distance,independent,snapshot,cruise_fraction,length,depth\n");
    std::fflush(g_debugLogFile);
}

// Rock outlines and the FPS counter; fish are drawn by the instanced GPU shader.
static void BuildOverlayVertices(RenderWindowState* state, std::vector<Vertex>& vertices)
{
    const float width = static_cast<float>(state->bounds.right - state->bounds.left);
    const float height = static_cast<float>(state->bounds.bottom - state->bounds.top);
    // Keep rock outlines about 1 pixel wide in the scaled-down preview.
    const float outlineScale = (std::max)(1.0f, 0.25f * width / static_cast<float>(state->pixelSize.cx));

    vertices.clear();
    size_t requiredVertices = 0;
    for (const Rock& rock : g_rocks)
    {
        requiredVertices += static_cast<size_t>(rock.pointCount) * 12;
    }
    if (g_settings.showFps)
    {
        requiredVertices += 168;
    }
    vertices.reserve(requiredVertices);
    auto addVertex = [&](float x, float y, float shade)
    {
        Vertex vertex;
        vertex.x = ((x - static_cast<float>(state->bounds.left)) / width) * 2.0f - 1.0f;
        vertex.y = 1.0f - ((y - static_cast<float>(state->bounds.top)) / height) * 2.0f;
        vertex.r = vertex.g = vertex.b = shade;
        vertex.a = 1.0f;
        vertices.push_back(vertex);
    };

    for (const Rock& rock : g_rocks)
    {
        for (int point = 0; point < rock.pointCount; ++point)
        {
            const POINT& first = rock.points[point];
            const POINT& second = rock.points[(point + 1) % rock.pointCount];
            float x1 = rock.x + static_cast<float>(first.x);
            float y1 = rock.y + static_cast<float>(first.y);
            float x2 = rock.x + static_cast<float>(second.x);
            float y2 = rock.y + static_cast<float>(second.y);
            float dx = x2 - x1;
            float dy = y2 - y1;
            float edgeLength = Length(dx, dy);
            if (edgeLength <= 0.001f) continue;
            float offsetX = -dy / edgeLength * 2.0f * outlineScale;
            float offsetY = dx / edgeLength * 2.0f * outlineScale;
            addVertex(x1 - offsetX, y1 - offsetY, 0.45f);
            addVertex(x2 - offsetX, y2 - offsetY, 0.45f);
            addVertex(x1 + offsetX, y1 + offsetY, 0.45f);
            addVertex(x2 - offsetX, y2 - offsetY, 0.45f);
            addVertex(x2 + offsetX, y2 + offsetY, 0.45f);
            addVertex(x1 + offsetX, y1 + offsetY, 0.45f);
        }
    }

    if (g_settings.showFps)
    {
        int fps = state->currentFps;
        int digitCount = (fps >= 1000) ? 4 : (fps >= 100) ? 3 : (fps >= 10) ? 2 : 1;
        float startX = width - 18.0f * digitCount - 12.0f;
        const float segmentThickness = 2.0f;
        const float digitWidth = 10.0f;
        const float digitHeight = 18.0f;
        const bool segments[10][7] = {
            { true, true, true, true, true, true, false },
            { false, true, true, false, false, false, false },
            { true, true, false, true, true, false, true },
            { true, true, true, true, false, false, true },
            { false, true, true, false, false, true, true },
            { true, false, true, true, false, true, true },
            { true, false, true, true, true, true, true },
            { true, true, true, false, false, false, false },
            { true, true, true, true, true, true, true },
            { true, true, true, true, false, true, true }
        };
        auto addFpsQuad = [&](float x, float y, float quadWidth, float quadHeight)
        {
            // The counter is laid out in window coordinates.
            auto addFpsVertex = [&](float px, float py)
            {
                addVertex(px + static_cast<float>(state->bounds.left), py + static_cast<float>(state->bounds.top), 1.0f);
            };
            addFpsVertex(x, y);
            addFpsVertex(x + quadWidth, y);
            addFpsVertex(x, y + quadHeight);
            addFpsVertex(x + quadWidth, y);
            addFpsVertex(x + quadWidth, y + quadHeight);
            addFpsVertex(x, y + quadHeight);
        };
        for (int digitIndex = 0; digitIndex < digitCount; ++digitIndex)
        {
            int divisor = 1;
            for (int i = 1; i < digitCount - digitIndex; ++i) divisor *= 10;
            int digit = (fps / divisor) % 10;
            float x = startX + digitIndex * 18.0f;
            float y = 10.0f;
            if (segments[digit][0]) addFpsQuad(x + 1.0f, y, digitWidth - 2.0f, segmentThickness);
            if (segments[digit][1]) addFpsQuad(x + digitWidth - segmentThickness, y + 1.0f, segmentThickness, digitHeight * 0.5f - 1.0f);
            if (segments[digit][2]) addFpsQuad(x + digitWidth - segmentThickness, y + digitHeight * 0.5f, segmentThickness, digitHeight * 0.5f - 1.0f);
            if (segments[digit][3]) addFpsQuad(x + 1.0f, y + digitHeight - segmentThickness, digitWidth - 2.0f, segmentThickness);
            if (segments[digit][4]) addFpsQuad(x, y + digitHeight * 0.5f, segmentThickness, digitHeight * 0.5f - 1.0f);
            if (segments[digit][5]) addFpsQuad(x, y + 1.0f, segmentThickness, digitHeight * 0.5f - 1.0f);
            if (segments[digit][6]) addFpsQuad(x + 1.0f, y + digitHeight * 0.5f - segmentThickness * 0.5f, digitWidth - 2.0f, segmentThickness);
        }
    }
}

static bool UseVSync()
{
    return g_settings.vsync && g_settings.frameRate == 0;
}

static void RenderFrame(RenderWindowState* state)
{
    if (!state || !state->device || !state->context || !state->renderTargetView)
    {
        return;
    }

    DWORD now = GetTickCount();
    if (state->fpsLastTick == 0) state->fpsLastTick = now;
    if (now - state->fpsLastTick >= 1000)
    {
        state->currentFps = state->fpsFrameCount;
        state->fpsFrameCount = 0;
        state->fpsLastTick = now;
    }

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    bool depthEffect = g_settings.depthEffect && state->depthStencilView && state->fishDepthState && state->shadowDepthState &&
        state->shadowBlendState && state->shadowVertexShader && state->shadowPixelShader;
    state->context->OMSetRenderTargets(1, &state->renderTargetView, depthEffect ? state->depthStencilView : nullptr);
    state->context->ClearRenderTargetView(state->renderTargetView, clearColor);
    if (depthEffect)
    {
        state->context->ClearDepthStencilView(state->depthStencilView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(state->pixelSize.cx);
    viewport.Height = static_cast<float>(state->pixelSize.cy);
    viewport.MaxDepth = 1.0f;
    state->context->RSSetViewports(1, &viewport);

    if (state->desktopView && state->desktopSampler && state->backgroundVertexShader && state->backgroundPixelShader)
    {
        state->context->IASetInputLayout(nullptr);
        state->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        state->context->VSSetShader(state->backgroundVertexShader, nullptr, 0);
        state->context->PSSetShader(state->backgroundPixelShader, nullptr, 0);
        state->context->PSSetShaderResources(0, 1, &state->desktopView);
        state->context->PSSetSamplers(0, 1, &state->desktopSampler);
        if (depthEffect)
        {
            state->context->OMSetDepthStencilState(state->fishDepthState, 0);
        }
        state->context->Draw(3, 0);
        ID3D11ShaderResourceView* nullView = nullptr;
        state->context->PSSetShaderResources(0, 1, &nullView);
    }

    {
        D3D11_MAPPED_SUBRESOURCE renderMapped = {};
        if (SUCCEEDED(state->context->Map(state->renderConstantsBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &renderMapped)))
        {
            GpuRenderConstants renderConstants = {
                static_cast<float>(state->bounds.left),
                static_cast<float>(state->bounds.top),
                static_cast<float>(state->bounds.right),
                static_cast<float>(state->bounds.bottom),
                static_cast<float>(g_settings.boidLength),
                static_cast<float>(g_monitorBounds.size()),
                g_settings.restrictToMonitor ? 1.0f : 0.0f,
                g_settings.tailPercent / 100.0f,
                static_cast<float>(g_settings.colourCount),
                g_settings.tailShade / 100.0f,
                depthEffect ? 1.0f : 0.0f
            };
            for (int index = 0; index < g_settings.colourCount; ++index)
            {
                renderConstants.colours[index][0] = GetRValue(g_settings.colours[index]) / 255.0f;
                renderConstants.colours[index][1] = GetGValue(g_settings.colours[index]) / 255.0f;
                renderConstants.colours[index][2] = GetBValue(g_settings.colours[index]) / 255.0f;
            }
            std::memcpy(renderMapped.pData, &renderConstants, sizeof(renderConstants));
            state->context->Unmap(state->renderConstantsBuffer, 0);
            state->context->IASetInputLayout(nullptr);
            state->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            state->context->VSSetShader(state->proceduralVertexShader, nullptr, 0);
            state->context->PSSetShader(state->proceduralPixelShader, nullptr, 0);
            state->context->VSSetShaderResources(0, 1, &g_gpuBoidView);
            state->context->VSSetShaderResources(1, 1, &g_gpuMonitorView);
            state->context->VSSetConstantBuffers(0, 1, &state->renderConstantsBuffer);
            state->context->PSSetConstantBuffers(0, 1, &state->renderConstantsBuffer);
            // Two quads per fish: the body oval, then the tail oval.
            if (depthEffect)
            {
                state->context->OMSetDepthStencilState(state->fishDepthState, 0);
            }
            state->context->DrawInstanced(12, static_cast<UINT>(g_boids.size()), 0, 0);
            if (depthEffect)
            {
                // Shadows after all fish, so the depth test can tell which fish are below each shadow.
                state->context->VSSetShader(state->shadowVertexShader, nullptr, 0);
                state->context->PSSetShader(state->shadowPixelShader, nullptr, 0);
                state->context->OMSetDepthStencilState(state->shadowDepthState, 0);
                state->context->OMSetBlendState(state->shadowBlendState, nullptr, 0xFFFFFFFF);
                state->context->DrawInstanced(12, static_cast<UINT>(g_boids.size()), 0, 0);
                state->context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                state->context->OMSetDepthStencilState(nullptr, 0);
                state->context->OMSetRenderTargets(1, &state->renderTargetView, nullptr);
            }
            ID3D11ShaderResourceView* nullViews[2] = {};
            state->context->VSSetShaderResources(0, 2, nullViews);
        }
    }

    BuildOverlayVertices(state, state->vertices);

    if (!state->vertices.empty())
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(state->context->Map(state->vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, state->vertices.data(), state->vertices.size() * sizeof(Vertex));
            state->context->Unmap(state->vertexBuffer, 0);
        }
        else
        {
            return;
        }

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        state->context->IASetVertexBuffers(0, 1, &state->vertexBuffer, &stride, &offset);
        state->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        state->context->IASetInputLayout(state->inputLayout);
        state->context->VSSetShader(state->vertexShader, nullptr, 0);
        state->context->PSSetShader(state->pixelShader, nullptr, 0);

        state->context->Draw(static_cast<UINT>(state->vertices.size()), 0);
    }

    LARGE_INTEGER presentStart = {};
    QueryPerformanceCounter(&presentStart);
    HRESULT presentResult = UseVSync()
        ? state->swapChain->Present(1, 0)
        : state->swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    LARGE_INTEGER presentEnd = {};
    QueryPerformanceCounter(&presentEnd);
    state->lastPresentMs = static_cast<double>(presentEnd.QuadPart - presentStart.QuadPart) * 1000.0 / g_qpcFrequency.QuadPart;
    if (SUCCEEDED(presentResult))
    {
        ++state->fpsFrameCount;
    }
    if (g_frameLogEnabled && FAILED(state->swapChain->GetFrameStatistics(&state->lastStats)))
    {
        ZeroMemory(&state->lastStats, sizeof(state->lastStats));
    }
}

static void DestroyWindowState(HWND hwnd)
{
    if (RenderWindowState* state = reinterpret_cast<RenderWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA)))
    {
        g_renderWindows.erase(std::remove(g_renderWindows.begin(), g_renderWindows.end(), state), g_renderWindows.end());
        if (state->frameLatencyWaitable) CloseHandle(state->frameLatencyWaitable);
        SAFE_RELEASE(state->renderTargetView);
        SAFE_RELEASE(state->vertexBuffer);
        SAFE_RELEASE(state->inputLayout);
        SAFE_RELEASE(state->vertexShader);
        SAFE_RELEASE(state->pixelShader);
        SAFE_RELEASE(state->renderConstantsBuffer);
        SAFE_RELEASE(state->proceduralVertexShader);
        SAFE_RELEASE(state->proceduralPixelShader);
        SAFE_RELEASE(state->depthStencilView);
        SAFE_RELEASE(state->fishDepthState);
        SAFE_RELEASE(state->shadowDepthState);
        SAFE_RELEASE(state->shadowBlendState);
        SAFE_RELEASE(state->shadowVertexShader);
        SAFE_RELEASE(state->shadowPixelShader);
        SAFE_RELEASE(state->desktopView);
        SAFE_RELEASE(state->desktopSampler);
        SAFE_RELEASE(state->backgroundVertexShader);
        SAFE_RELEASE(state->backgroundPixelShader);
        SAFE_RELEASE(state->context);
        SAFE_RELEASE(state->swapChain);
        SAFE_RELEASE(state->device);
        delete state;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
    }
}

// Frames per second for a window when not paced by a vsync waitable object; 0 means unlimited.
static double TargetFrameRate(const RenderWindowState* state)
{
    if (g_settings.frameRate > 0)
    {
        return g_settings.frameRate;
    }
    if (UseVSync())
    {
        return state->refreshHz;
    }
    return state->refreshHz * kFrameRateModeMultipliers[g_settings.frameRateMode];
}

static void InitD3DForWindow(HWND hwnd, RECT bounds)
{
    RenderWindowState* state = new RenderWindowState();
    state->hwnd = hwnd;
    state->bounds = bounds;
    state->refreshHz = 60.0;
    state->windowIndex = g_nextWindowIndex++;

    RECT client = {};
    GetClientRect(hwnd, &client);
    int width = (std::max)(1L, client.right - client.left);
    int height = (std::max)(1L, client.bottom - client.top);
    state->pixelSize = { width, height };
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = width;
    swapChainDesc.BufferDesc.Height = height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (UseVSync())
    {
        swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    HRESULT hr = E_FAIL;
    if (g_sharedDevice && g_sharedContext && g_sharedFactory)
    {
        state->device = g_sharedDevice;
        state->device->AddRef();
        state->context = g_sharedContext;
        state->context->AddRef();
        hr = g_sharedFactory->CreateSwapChain(g_sharedDevice, &swapChainDesc, &state->swapChain);
    }
    else
    {
        // Compute shaders (cs_5_0) need a feature level 11_0 GPU.
        const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            &featureLevel,
            1,
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &state->swapChain,
            &state->device,
            nullptr,
            &state->context);
    }

    if (FAILED(hr))
    {
        delete state;
        g_gpuUnavailable = true;
        return;
    }

    if (!g_sharedFactory)
    {
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(state->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        {
            adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&g_sharedFactory));
        }
        SAFE_RELEASE(adapter);
        SAFE_RELEASE(dxgiDevice);
    }

    if (swapChainDesc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
    {
        // Each monitor gets its own waitable object, so each window is paced by its own monitor's vblank.
        IDXGISwapChain2* swapChain2 = nullptr;
        if (SUCCEEDED(state->swapChain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&swapChain2))))
        {
            swapChain2->SetMaximumFrameLatency(1);
            state->frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
            swapChain2->Release();
        }
    }

    ID3D11Texture2D* backBuffer = nullptr;
    state->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer)
    {
        state->device->CreateRenderTargetView(backBuffer, nullptr, &state->renderTargetView);
        backBuffer->Release();
    }

    const char* shaderSource =
        "struct VSInput { float2 pos : POSITION; float4 color : COLOR; };\n"
        "struct PSInput { float4 pos : SV_POSITION; float4 color : COLOR; };\n"
        "PSInput VSMain(VSInput input) { PSInput output; output.pos = float4(input.pos, 0.0f, 1.0f); output.color = input.color; return output; }\n"
        "float4 PSMain(PSInput input) : SV_TARGET { return input.color; }\n";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (SUCCEEDED(CompileShader(shaderSource, "VSMain", "vs_5_0", &vsBlob)) &&
        SUCCEEDED(CompileShader(shaderSource, "PSMain", "ps_5_0", &psBlob)))
    {
        state->device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &state->vertexShader);
        state->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &state->pixelShader);

        D3D11_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };

        state->device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &state->inputLayout);
        vsBlob->Release();
        psBlob->Release();
    }

    const std::string proceduralShaderSource = std::string(kShaderBoidStruct) +
        "StructuredBuffer<Boid> Boids : register(t0);\n"
        "struct Monitor { float left; float top; float right; float bottom; };\n"
        "StructuredBuffer<Monitor> Monitors : register(t1);\n"
        "cbuffer Render : register(b0) { float4 bounds; float baseLength; float monitorCount; float restrictToMonitor; float tailRatio; float colourCount; float tailShade; float depthEffect; float padding; float4 colours[10]; };\n"
        "struct VSOut { float4 position : SV_POSITION; float2 shapeCoordinate : TEXCOORD0; float along : TEXCOORD1; nointerpolation float3 colour : COLOR0; };\n"
        // Body sway: the fish drifts sideways and yaws slightly in time with its tail beat (visual only).
        "float3 swimPose(Boid b, uint i) {\n"
        "    float omega = swimOmega(b.currentSpeed, b.fishLength, i);\n"
        "    float amplitude = min(b.fishLength * 0.04f, b.currentSpeed * 0.1f / omega) * min(b.thrust, 1.2f);\n"
        "    float2 position = b.position + float2(-sin(b.heading), cos(b.heading)) * amplitude * sin(b.swimPhase);\n"
        "    if (restrictToMonitor > 0.5f) { Monitor m = Monitors[i % (uint)monitorCount]; position = clamp(position, float2(m.left, m.top), float2(m.right - 1, m.bottom - 1)); }\n"
        "    float heading = b.heading + atan2(amplitude * omega, max(b.currentSpeed, 0.001f)) * cos(b.swimPhase);\n"
        "    return float3(position, heading);\n"
        "}\n"
        // Tail angle relative to the body: swings opposite the body's yaw (slightly behind it) and bends away from turns like a rudder.
        // The beat scales with thrust, so a fish slowing down coasts with a still tail but still steers with it.
        // Faster fish (in body lengths per second) sweep wider. Must match FishTailAngle in the settings preview.
        "float tailAngle(Boid b) {\n"
        "    float sweep = clamp(0.25f + 0.15f * b.currentSpeed / (b.fishLength * 4.0f), 0.25f, 0.6f) * min(b.thrust, 1.3f);\n"
        "    return clamp(-sweep * cos(b.swimPhase - 0.6f) - clamp(b.yawRate * 0.12f, -0.5f, 0.5f), -0.9f, 0.9f);\n"
        "}\n"
        // Fish shape (see FishLayoutFor in C++): the tail is the body oval scaled by tailRatio, overlapping it by 20% of the tail's
        // length, and the two together span exactly the fish's own length (b.fishLength). The tail pivots 85% of its half-length ahead of its centre.
        "VSOut fishVertex(uint vertexId, uint instanceId, bool shadow) {\n"
        "    float2 quad[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,-1), float2(1,1), float2(-1,1) };\n"
        "    Boid b = Boids[instanceId]; float3 pose = swimPose(b, instanceId); float2 local = quad[vertexId % 6];\n"
        // 3D depth: height 0 is deepest. Deeper fish are drawn smaller, darker and behind higher ones.
        "    float height = depthEffect > 0.5f ? b.depth : 1.0f; float fishLength = b.fishLength * (0.8f + 0.2f * height);\n"
        "    float2 direction = float2(cos(pose.z), sin(pose.z)); float2 axis; float2 center; float2 halfSize;\n"
        "    float bodyLength = fishLength / (1.0f + 0.8f * tailRatio); float2 bodyHalf = float2(0.5f, 0.15625f) * bodyLength; float2 tailHalf = bodyHalf * tailRatio;\n"
        "    if (vertexId < 6) { axis = direction; center = pose.xy + direction * (fishLength * 0.5f - bodyHalf.x); halfSize = bodyHalf; }\n"
        "    else { float tail = pose.z + tailAngle(b); axis = float2(cos(tail), sin(tail)); center = pose.xy - direction * (fishLength * 0.5f - 1.85f * tailHalf.x) - axis * 0.85f * tailHalf.x; halfSize = tailHalf; }\n"
        "    float2 position = center + axis * local.x * halfSize.x + float2(-axis.y, axis.x) * local.y * halfSize.y;\n"
        // Light from the top left: the shadow falls down and right, further for higher fish, just behind the fish itself.
        "    if (shadow) position += float2(0.7071f, 0.7071f) * fishLength * (0.04f + 0.08f * height);\n"
        "    float z = 0.5f - 0.4f * height + (shadow ? 0.00001f : 0.0f);\n"
        "    VSOut output; output.position = float4((position.x - bounds.x) / (bounds.z - bounds.x) * 2.0f - 1.0f, 1.0f - (position.y - bounds.y) / (bounds.w - bounds.y) * 2.0f, z, 1.0f); output.shapeCoordinate = local;\n"
        // 0 at the head tip, 1 at the tail tip; each fish keeps one palette colour for life.
        "    output.along = dot(pose.xy + direction * fishLength * 0.5f - position, direction) / fishLength;\n"
        "    uint colourIndex = ((instanceId * 2654435761u) >> 16) % max((uint)colourCount, 1u); output.colour = colours[colourIndex].rgb * (0.6f + 0.4f * height); return output;\n"
        "}\n"
        "VSOut VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) { return fishVertex(vertexId, instanceId, false); }\n"
        "VSOut VSShadow(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) { return fishVertex(vertexId, instanceId, true); }\n"
        // Multiplied into the fish below (blend: dest * src); the stencil keeps overlapping shadows from darkening twice.
        "float4 PSShadow(VSOut input) : SV_TARGET { if (dot(input.shapeCoordinate, input.shapeCoordinate) > 1.0f) discard; return float4(0.2f, 0.2f, 0.2f, 1); }\n"
        // Must match ShadeFishColour in the settings preview.
        "float4 PSMain(VSOut input) : SV_TARGET { if (dot(input.shapeCoordinate, input.shapeCoordinate) > 1.0f) discard; float t = saturate(input.along) * tailShade; float3 c = t < 0.0f ? input.colour * (1.0f + t) : lerp(input.colour, 1.0f, t); return float4(c, 1); }\n";
    ID3DBlob* proceduralVsBlob = nullptr;
    ID3DBlob* proceduralPsBlob = nullptr;
    if (SUCCEEDED(CompileShader(proceduralShaderSource.c_str(), "VSMain", "vs_5_0", &proceduralVsBlob)) &&
        SUCCEEDED(CompileShader(proceduralShaderSource.c_str(), "PSMain", "ps_5_0", &proceduralPsBlob)))
    {
        state->device->CreateVertexShader(proceduralVsBlob->GetBufferPointer(), proceduralVsBlob->GetBufferSize(), nullptr, &state->proceduralVertexShader);
        state->device->CreatePixelShader(proceduralPsBlob->GetBufferPointer(), proceduralPsBlob->GetBufferSize(), nullptr, &state->proceduralPixelShader);
        proceduralVsBlob->Release();
        proceduralPsBlob->Release();
    }
    if (SUCCEEDED(CompileShader(proceduralShaderSource.c_str(), "VSShadow", "vs_5_0", &proceduralVsBlob)) &&
        SUCCEEDED(CompileShader(proceduralShaderSource.c_str(), "PSShadow", "ps_5_0", &proceduralPsBlob)))
    {
        state->device->CreateVertexShader(proceduralVsBlob->GetBufferPointer(), proceduralVsBlob->GetBufferSize(), nullptr, &state->shadowVertexShader);
        state->device->CreatePixelShader(proceduralPsBlob->GetBufferPointer(), proceduralPsBlob->GetBufferSize(), nullptr, &state->shadowPixelShader);
        proceduralVsBlob->Release();
        proceduralPsBlob->Release();
    }
    {
        D3D11_TEXTURE2D_DESC depthDesc = {};
        depthDesc.Width = static_cast<UINT>(width);
        depthDesc.Height = static_cast<UINT>(height);
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* depthTexture = nullptr;
        if (SUCCEEDED(state->device->CreateTexture2D(&depthDesc, nullptr, &depthTexture)))
        {
            state->device->CreateDepthStencilView(depthTexture, nullptr, &state->depthStencilView);
            depthTexture->Release();
        }
        D3D11_DEPTH_STENCIL_DESC fishDepth = {};
        fishDepth.DepthEnable = TRUE;
        fishDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        fishDepth.DepthFunc = D3D11_COMPARISON_LESS;
        state->device->CreateDepthStencilState(&fishDepth, &state->fishDepthState);
        // Shadows: only where a lower fish (or the empty background) is, at most once per pixel.
        D3D11_DEPTH_STENCIL_DESC shadowDepth = fishDepth;
        shadowDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        shadowDepth.StencilEnable = TRUE;
        shadowDepth.StencilReadMask = 0xFF;
        shadowDepth.StencilWriteMask = 0xFF;
        shadowDepth.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        shadowDepth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT;
        shadowDepth.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        shadowDepth.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        shadowDepth.BackFace = shadowDepth.FrontFace;
        state->device->CreateDepthStencilState(&shadowDepth, &state->shadowDepthState);
        D3D11_BLEND_DESC shadowBlend = {};
        shadowBlend.RenderTarget[0].BlendEnable = TRUE;
        shadowBlend.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        shadowBlend.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        shadowBlend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        shadowBlend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        shadowBlend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        shadowBlend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        shadowBlend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        state->device->CreateBlendState(&shadowBlend, &state->shadowBlendState);
    }
    for (const DesktopCapture& capture : g_desktopCaptures)
    {
        if (!EqualRect(&capture.rect, &bounds))
        {
            continue;
        }
        // A full-screen triangle at the far depth, so fish draw over it and their shadows fall on it.
        const char* backgroundSource =
            "Texture2D Desktop : register(t0); SamplerState Linear : register(s0);\n"
            "struct BackgroundOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };\n"
            "BackgroundOut VSBackground(uint id : SV_VertexID) { BackgroundOut o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv; o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0.999f, 1); return o; }\n"
            "float4 PSBackground(BackgroundOut input) : SV_TARGET { return float4(Desktop.Sample(Linear, input.uv).rgb, 1); }\n";
        ID3DBlob* backgroundVsBlob = nullptr;
        ID3DBlob* backgroundPsBlob = nullptr;
        if (SUCCEEDED(CompileShader(backgroundSource, "VSBackground", "vs_5_0", &backgroundVsBlob)) &&
            SUCCEEDED(CompileShader(backgroundSource, "PSBackground", "ps_5_0", &backgroundPsBlob)))
        {
            state->device->CreateVertexShader(backgroundVsBlob->GetBufferPointer(), backgroundVsBlob->GetBufferSize(), nullptr, &state->backgroundVertexShader);
            state->device->CreatePixelShader(backgroundPsBlob->GetBufferPointer(), backgroundPsBlob->GetBufferSize(), nullptr, &state->backgroundPixelShader);
        }
        SAFE_RELEASE(backgroundVsBlob);
        SAFE_RELEASE(backgroundPsBlob);
        D3D11_TEXTURE2D_DESC desktopDesc = {};
        desktopDesc.Width = static_cast<UINT>(capture.width);
        desktopDesc.Height = static_cast<UINT>(capture.height);
        desktopDesc.MipLevels = 1;
        desktopDesc.ArraySize = 1;
        desktopDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desktopDesc.SampleDesc.Count = 1;
        desktopDesc.Usage = D3D11_USAGE_IMMUTABLE;
        desktopDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA desktopData = { capture.pixels.data(), static_cast<UINT>(capture.width * 4), 0 };
        ID3D11Texture2D* desktopTexture = nullptr;
        if (SUCCEEDED(state->device->CreateTexture2D(&desktopDesc, &desktopData, &desktopTexture)))
        {
            state->device->CreateShaderResourceView(desktopTexture, nullptr, &state->desktopView);
            desktopTexture->Release();
        }
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        state->device->CreateSamplerState(&samplerDesc, &state->desktopSampler);
        break;
    }

    D3D11_BUFFER_DESC renderConstantsDesc = {};
    renderConstantsDesc.ByteWidth = sizeof(GpuRenderConstants);
    renderConstantsDesc.Usage = D3D11_USAGE_DYNAMIC;
    renderConstantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    renderConstantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    state->device->CreateBuffer(&renderConstantsDesc, nullptr, &state->renderConstantsBuffer);

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(Vertex) * kMaxOverlayVertices;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(state->device->CreateBuffer(&bufferDesc, nullptr, &state->vertexBuffer)))
    {
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        DestroyWindowState(hwnd);
        g_gpuUnavailable = true;
        return;
    }

    if ((!g_sharedDevice && !InitializeGpuSimulation(state->device, state->context)) ||
        !g_gpuBoidComputeShader || !state->proceduralVertexShader || !state->proceduralPixelShader ||
        !state->renderConstantsBuffer || !state->vertexShader || !state->pixelShader || !state->inputLayout)
    {
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        DestroyWindowState(hwnd);
        g_gpuUnavailable = true;
        return;
    }
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    state->refreshHz = GetRefreshRateForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
    g_renderWindows.push_back(state);
    if (g_debugLogFile)
    {
        std::fprintf(g_debugLogFile, "# window %d bounds=%ld,%ld,%ld,%ld refresh=%.3f vsync=%d waitable=%d target_fps=%.1f\n",
            state->windowIndex, bounds.left, bounds.top, bounds.right, bounds.bottom, state->refreshHz,
            UseVSync() ? 1 : 0, state->frameLatencyWaitable ? 1 : 0, TargetFrameRate(state));
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE:
            return 0;

        case WM_PAINT:
        {
            RenderWindowState* state = reinterpret_cast<RenderWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            if (state && !state->frameLatencyWaitable)
            {
                RenderFrame(state);
            }
            ValidateRect(hwnd, nullptr);
            return 0;
        }

        case WM_ERASEBKGND:
            return TRUE;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            PostQuitMessage(0);
            return 0;

        case WM_SETCURSOR:
            if (!g_isPreview && LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(nullptr);
                return TRUE;
            }
            return DefWindowProc(hwnd, message, wParam, lParam);

        case WM_MOUSEMOVE:
        {
            POINT cursorPosition = {};
            if (!g_isPreview && GetTickCount() >= g_inputReadyTick && GetCursorPos(&cursorPosition) &&
                (cursorPosition.x != g_initialCursorPosition.x ||
                 cursorPosition.y != g_initialCursorPosition.y))
            {
                PostQuitMessage(0);
            }
            return 0;
        }

        case WM_DESTROY:
            DestroyWindowState(hwnd);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

// Full-screen window showing monitorRect. With a parent, it is a child filling windowRect that shows monitorRect scaled down.
static void CreateMonitorWindow(HINSTANCE instance, RECT monitorRect, HWND parent = nullptr, RECT windowRect = {})
{
    if (!parent) windowRect = monitorRect;
    int width = windowRect.right - windowRect.left;
    int height = windowRect.bottom - windowRect.top;

    HWND hwnd = CreateWindowEx(
        parent ? 0 : (WS_EX_TOPMOST | WS_EX_TOOLWINDOW),
        L"ShoalScreenSaverWindow",
        L"Shoal",
        parent ? WS_CHILD : WS_POPUP,
        windowRect.left,
        windowRect.top,
        width,
        height,
        parent,
        nullptr,
        instance,
        nullptr);

    if (!hwnd)
    {
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    if (!parent)
    {
        SetWindowPos(hwnd, HWND_TOPMOST, monitorRect.left, monitorRect.top, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    InitD3DForWindow(hwnd, monitorRect);
    if (g_gpuUnavailable)
    {
        DestroyWindow(hwnd);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
}

static void CenterSettingsWindow(HWND hwnd, HWND parent)
{
    RECT windowRect = {};
    GetWindowRect(hwnd, &windowRect);

    RECT workArea = {};
    if (!parent || !GetWindowRect(parent, &workArea))
    {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
    }

    int width = windowRect.right - windowRect.left;
    int height = windowRect.bottom - windowRect.top;
    int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static const UINT_PTR kPreviewTimer = 1;
static double g_previewTime = 0.0; // Seconds of preview animation.
static DWORD g_previewLastTick = 0;

// Fish shape; must match VSMain in the fish shader. The tail is the body oval scaled by tailRatio, overlapping it by
// 20% of the tail's length, so body plus tail span exactly the fish length.
struct FishLayout
{
    double bodyHalfLength;
    double bodyHalfWidth;
    double tailHalfLength;
    double tailHalfWidth;
};

static FishLayout FishLayoutFor(double length, double tailRatio)
{
    double body = length / (1.0 + 0.8 * tailRatio);
    return { body * 0.5, body * 0.15625, body * 0.5 * tailRatio, body * 0.15625 * tailRatio };
}

// Tail angle relative to the body for a fish swimming straight; matches tailAngle in the fish shader.
static double FishTailAngle(double speed, double length, double swimPhase)
{
    double sweep = (std::min)(0.6, (std::max)(0.25, 0.25 + 0.15 * speed / (length * 4.0)));
    return (std::max)(-0.9, (std::min)(0.9, -sweep * std::cos(swimPhase - 0.6)));
}

// Head-to-tail lightness change; must match PSMain in the fish shader. Returns a DIB pixel (0x00RRGGBB).
static DWORD ShadeFishColour(COLORREF colour, double shade, double along)
{
    double t = (std::max)(0.0, (std::min)(1.0, along)) * shade;
    double channel[3] = { GetRValue(colour) / 255.0, GetGValue(colour) / 255.0, GetBValue(colour) / 255.0 };
    DWORD pixel = 0;
    for (double value : channel)
    {
        value = t < 0.0 ? value * (1.0 + t) : value + (1.0 - value) * t;
        pixel = (pixel << 8) | static_cast<DWORD>(std::lround(value * 255.0));
    }
    return pixel;
}

static bool InsideOval(double x, double y, double axisX, double axisY, double halfLength, double halfWidth)
{
    double u = (x * axisX + y * axisY) / halfLength;
    double v = (y * axisX - x * axisY) / halfWidth;
    return u * u + v * v <= 1.0;
}

// Rasterizes one fish into a top-down 32-bit DIB, with the same shape and gradient as the screensaver.
static void DrawPreviewFish(DWORD* pixels, int width, int height, double x, double y, double length, double tailRatio,
    double heading, double tailAngle, COLORREF colour, double shade)
{
    FishLayout shape = FishLayoutFor(length, tailRatio);
    double dirX = std::cos(heading);
    double dirY = std::sin(heading);
    double bodyOffset = length * 0.5 - shape.bodyHalfLength;
    double bodyX = x + dirX * bodyOffset;
    double bodyY = y + dirY * bodyOffset;
    double tail = heading + tailAngle;
    double tailDirX = std::cos(tail);
    double tailDirY = std::sin(tail);
    double hinge = length * 0.5 - 1.85 * shape.tailHalfLength;
    double tailX = x - dirX * hinge - tailDirX * 0.85 * shape.tailHalfLength;
    double tailY = y - dirY * hinge - tailDirY * 0.85 * shape.tailHalfLength;
    double headX = x + dirX * length * 0.5;
    double headY = y + dirY * length * 0.5;
    int reach = static_cast<int>(std::ceil(length * 0.6)) + 1;
    int left = (std::max)(0, static_cast<int>(x) - reach);
    int right = (std::min)(width - 1, static_cast<int>(x) + reach);
    int top = (std::max)(0, static_cast<int>(y) - reach);
    int bottom = (std::min)(height - 1, static_cast<int>(y) + reach);
    for (int py = top; py <= bottom; ++py)
    {
        for (int px = left; px <= right; ++px)
        {
            double cx = px + 0.5;
            double cy = py + 0.5;
            if (!InsideOval(cx - bodyX, cy - bodyY, dirX, dirY, shape.bodyHalfLength, shape.bodyHalfWidth) &&
                !InsideOval(cx - tailX, cy - tailY, tailDirX, tailDirY, shape.tailHalfLength, shape.tailHalfWidth))
            {
                continue;
            }
            double along = ((headX - cx) * dirX + (headY - cy) * dirY) / length;
            pixels[py * width + px] = ShadeFishColour(colour, shade, along);
        }
    }
}

static void DrawColourSwatches(HDC dc)
{
    RECT area = { kSwatchLeft, kSwatchTop, kSwatchLeft + kMaxFishColours * kSwatchStride, kSwatchTop + kSwatchSize };
    FillRect(dc, &area, GetSysColorBrush(COLOR_BTNFACE));
    for (int index = 0; index < g_pendingSettings.colourCount; ++index)
    {
        RECT swatch = { kSwatchLeft + index * kSwatchStride, kSwatchTop, kSwatchLeft + index * kSwatchStride + kSwatchSize, kSwatchTop + kSwatchSize };
        HBRUSH brush = CreateSolidBrush(g_pendingSettings.colours[index]);
        FillRect(dc, &swatch, brush);
        DeleteObject(brush);
        FrameRect(dc, &swatch, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    }
}

static int SwatchAt(LPARAM lParam)
{
    int x = static_cast<short>(LOWORD(lParam)) - kSwatchLeft;
    int y = static_cast<short>(HIWORD(lParam)) - kSwatchTop;
    if (x < 0 || y < 0 || y >= kSwatchSize || x % kSwatchStride >= kSwatchSize)
    {
        return -1;
    }
    int index = x / kSwatchStride;
    return index < g_pendingSettings.colourCount ? index : -1;
}

static bool PickColour(HWND hwnd, COLORREF* colour)
{
    CHOOSECOLOR choose = {};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = hwnd;
    choose.rgbResult = *colour;
    choose.lpCustColors = g_customColours;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColor(&choose))
    {
        return false;
    }
    *colour = choose.rgbResult;
    return true;
}

// Sorting needs two or more colours; avoidance also needs sorting on.
static void UpdateColourSortControls(HWND hwnd)
{
    bool multiple = g_pendingSettings.colourCount > 1;
    EnableWindow(GetDlgItem(hwnd, kColourSortCheckbox), multiple);
    EnableWindow(GetDlgItem(hwnd, kColourAvoidCheckbox),
        multiple && SendMessage(GetDlgItem(hwnd, kColourSortCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED);
}

static void ColoursChanged(HWND hwnd)
{
    EnableWindow(GetDlgItem(hwnd, kAddColourButton), g_pendingSettings.colourCount < kMaxFishColours);
    // Colour bias and avoidance only mean something with two or more colours.
    UpdateColourSortControls(hwnd);
    RECT area = { kSwatchLeft, kSwatchTop, kSwatchLeft + kMaxFishColours * kSwatchStride, kSwatchTop + kSwatchSize };
    InvalidateRect(hwnd, &area, FALSE);
    InvalidateRect(hwnd, &kSizePreviewRect, FALSE);
}

// Copies every control's value into g_pendingSettings (colours are already kept there).
static void ReadSettingsControls(HWND hwnd)
{
    g_pendingSettings.speedPixelsPerSecond = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSpeedSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.fishCoverage = static_cast<int>(SendMessage(GetDlgItem(hwnd, kCountSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.randomness = static_cast<int>(SendMessage(GetDlgItem(hwnd, kRandomnessSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.sizeTenths = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSizeSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.boidLength = BoidLengthForSize(g_pendingSettings.sizeTenths);
    g_pendingSettings.showFps = SendMessage(GetDlgItem(hwnd, kShowFpsCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.frameRateMode = static_cast<int>(SendMessage(GetDlgItem(hwnd, kFrameRateSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.vsync = SendMessage(GetDlgItem(hwnd, kVSyncCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.rockCount = static_cast<int>(SendMessage(GetDlgItem(hwnd, kRockCountSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.speedVariability = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSpeedVariabilitySlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.tailPercent = static_cast<int>(SendMessage(GetDlgItem(hwnd, kTailSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.tailShade = static_cast<int>(SendMessage(GetDlgItem(hwnd, kTailShadeSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.colourSort = SendMessage(GetDlgItem(hwnd, kColourSortCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.colourAvoid = SendMessage(GetDlgItem(hwnd, kColourAvoidCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.sizeVariation = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSizeVariationSlider), TBM_GETPOS, 0, 0));
    g_pendingSettings.sizeBell = SendMessage(GetDlgItem(hwnd, kSizeBellCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.depthEffect = SendMessage(GetDlgItem(hwnd, kDepthEffectCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.desktopBackground = SendMessage(GetDlgItem(hwnd, kDesktopBackgroundCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.restrictToMonitor = SendMessage(GetDlgItem(hwnd, kRestrictToMonitorCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_pendingSettings.randomPreset = SendMessage(GetDlgItem(hwnd, kRandomPresetCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// Shows g_pendingSettings in every control (after loading a preset).
static void WriteSettingsControls(HWND hwnd)
{
    struct { int id; int value; } sliders[] = {
        { kSpeedSlider, g_pendingSettings.speedPixelsPerSecond }, { kCountSlider, g_pendingSettings.fishCoverage },
        { kRandomnessSlider, g_pendingSettings.randomness }, { kSizeSlider, g_pendingSettings.sizeTenths },
        { kFrameRateSlider, g_pendingSettings.frameRateMode }, { kRockCountSlider, g_pendingSettings.rockCount },
        { kSpeedVariabilitySlider, g_pendingSettings.speedVariability }, { kTailSlider, g_pendingSettings.tailPercent },
        { kTailShadeSlider, g_pendingSettings.tailShade }, { kSizeVariationSlider, g_pendingSettings.sizeVariation } };
    for (const auto& slider : sliders)
    {
        SendMessage(GetDlgItem(hwnd, slider.id), TBM_SETPOS, TRUE, slider.value);
    }
    struct { int id; bool value; } checks[] = {
        { kShowFpsCheckbox, g_pendingSettings.showFps }, { kVSyncCheckbox, g_pendingSettings.vsync },
        { kColourSortCheckbox, g_pendingSettings.colourSort }, { kColourAvoidCheckbox, g_pendingSettings.colourAvoid },
        { kSizeBellCheckbox, g_pendingSettings.sizeBell }, { kDepthEffectCheckbox, g_pendingSettings.depthEffect },
        { kDesktopBackgroundCheckbox, g_pendingSettings.desktopBackground }, { kRestrictToMonitorCheckbox, g_pendingSettings.restrictToMonitor } };
    for (const auto& check : checks)
    {
        SendMessage(GetDlgItem(hwnd, check.id), BM_SETCHECK, check.value ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    EnableWindow(GetDlgItem(hwnd, kSizeBellCheckbox), g_pendingSettings.sizeVariation > 100);
    EnableWindow(GetDlgItem(hwnd, kFrameRateSlider), !g_pendingSettings.vsync);
    ColoursChanged(hwnd);
    UpdateSettingsValueLabels(hwnd);
}

static std::wstring PresetComboText(HWND hwnd)
{
    wchar_t text[128] = {};
    GetWindowText(GetDlgItem(hwnd, kPresetCombo), text, 128);
    std::wstring name = text;
    size_t first = name.find_first_not_of(L" \t");
    size_t last = name.find_last_not_of(L" \t");
    return first == std::wstring::npos ? std::wstring() : name.substr(first, last - first + 1);
}

// Save needs a name; Delete needs the name of an existing preset.
static void UpdatePresetButtons(HWND hwnd, const std::wstring& name)
{
    HWND combo = GetDlgItem(hwnd, kPresetCombo);
    EnableWindow(GetDlgItem(hwnd, kPresetSaveButton), !name.empty());
    EnableWindow(GetDlgItem(hwnd, kRandomPresetCheckbox), SendMessage(combo, CB_GETCOUNT, 0, 0) > 0);
    EnableWindow(GetDlgItem(hwnd, kPresetDeleteButton), !name.empty() && SendMessage(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name.c_str())) != CB_ERR);
}

static void AddPresetTooltip(HWND hwnd, HWND tooltip, int id, const wchar_t* text)
{
    TOOLINFO tool = {};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = hwnd;
    tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(hwnd, id));
    tool.lpszText = const_cast<wchar_t*>(text);
    SendMessage(tooltip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&tool));
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE:
        {
            HINSTANCE instance = reinterpret_cast<LPCREATESTRUCT>(lParam)->hInstance;
            g_pendingSettings = g_settings;
            HWND title = CreateSettingsLabel(hwnd, instance, L"Shoal: Settings", 24, 14, 400, 36);
            if (!g_titleFont)
            {
                g_titleFont = CreateFont(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            }
            SendMessage(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);

            // Presets: type a name and click Save to store all settings under it; pick one to load it; Delete removes the selected one.
            CreateSettingsLabel(hwnd, instance, L"Preset", 450, 24, 50, 20);
            HWND presetCombo = CreateWindowEx(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_SORT | CBS_AUTOHSCROLL,
                510, 20, 340, 240, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPresetCombo)), instance, nullptr);
            SendMessage(presetCombo, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessage(presetCombo, CB_LIMITTEXT, 64, 0);
            for (const std::wstring& name : ListPresets())
            {
                SendMessage(presetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            }
            if (!g_iconFont)
            {
                g_iconFont = CreateFont(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
            }
            HWND saveButton = CreateWindowEx(0, L"BUTTON", L"\xE74E", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                858, 19, 38, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPresetSaveButton)), instance, nullptr);
            HWND deleteButton = CreateWindowEx(0, L"BUTTON", L"\xE74D", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                902, 19, 38, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPresetDeleteButton)), instance, nullptr);
            SendMessage(saveButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_iconFont), TRUE);
            SendMessage(deleteButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_iconFont), TRUE);
            HWND tooltip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_ALWAYSTIP,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, instance, nullptr);
            AddPresetTooltip(hwnd, tooltip, kPresetSaveButton, L"Save all settings as a preset with the typed name");
            AddPresetTooltip(hwnd, tooltip, kPresetDeleteButton, L"Delete the selected preset");
            UpdatePresetButtons(hwnd, L"");

            // Left column: sliders, one row every 40 px. Two-line labels start at the row top, one-line labels 6 px lower.
            CreateSettingsLabel(hwnd, instance, L"Speed", 24, 76, 110, 24);
            CreateSettingsSlider(hwnd, instance, kSpeedSlider, 1, 1000, g_pendingSettings.speedPixelsPerSecond, 70);
            g_speedValue = CreateSettingsLabel(hwnd, instance, L"", 400, 76, 90, 24);
            CreateSettingsLabel(hwnd, instance, L"Speed Variability", 24, 116, 120, 24);
            CreateSettingsSlider(hwnd, instance, kSpeedVariabilitySlider, 0, 10, g_pendingSettings.speedVariability, 110);
            g_speedVariabilityValue = CreateSettingsLabel(hwnd, instance, L"", 400, 116, 90, 24);
            CreateSettingsLabel(hwnd, instance, L"Fish Amount", 24, 150, 120, 18);
            CreateSettingsLabel(hwnd, instance, L"(% of free space)", 24, 168, 120, 18);
            HWND countSlider = CreateSettingsSlider(hwnd, instance, kCountSlider, 0, 100, g_pendingSettings.fishCoverage, 150);
            SendMessage(countSlider, TBM_SETTICFREQ, 10, 0);
            g_countValue = CreateSettingsLabel(hwnd, instance, L"", 400, 148, 90, 34);
            CreateSettingsLabel(hwnd, instance, L"Randomness", 24, 196, 110, 24);
            CreateSettingsSlider(hwnd, instance, kRandomnessSlider, 0, 100, g_pendingSettings.randomness, 190);
            g_randomnessValue = CreateSettingsLabel(hwnd, instance, L"", 400, 196, 90, 24);
            CreateSettingsLabel(hwnd, instance, L"Fish Size", 24, 230, 120, 18);
            CreateSettingsLabel(hwnd, instance, L"(% screen height)", 24, 248, 120, 18);
            HWND sizeSlider = CreateSettingsSlider(hwnd, instance, kSizeSlider, 10, 100, g_pendingSettings.sizeTenths, 230);
            SendMessage(sizeSlider, TBM_SETTICFREQ, 10, 0);
            g_sizeValue = CreateSettingsLabel(hwnd, instance, L"", 400, 236, 90, 24);
            CreateSettingsLabel(hwnd, instance, L"Size Variability", 24, 270, 120, 18);
            CreateSettingsLabel(hwnd, instance, L"(size range)", 24, 288, 120, 18);
            HWND variationSlider = CreateSettingsSlider(hwnd, instance, kSizeVariationSlider, 100, 300, g_pendingSettings.sizeVariation, 270);
            SendMessage(variationSlider, TBM_SETTICFREQ, 25, 0);
            SendMessage(variationSlider, TBM_SETPAGESIZE, 0, 25);
            g_sizeVariationValue = CreateSettingsLabel(hwnd, instance, L"", 400, 268, 90, 34);
            CreateSettingsLabel(hwnd, instance, L"Tail Size", 24, 310, 120, 18);
            CreateSettingsLabel(hwnd, instance, L"(% of body)", 24, 328, 120, 18);
            HWND tailSlider = CreateSettingsSlider(hwnd, instance, kTailSlider, 10, 100, g_pendingSettings.tailPercent, 310);
            SendMessage(tailSlider, TBM_SETTICFREQ, 10, 0);
            g_tailValue = CreateSettingsLabel(hwnd, instance, L"", 400, 316, 90, 24);
            CreateSettingsLabel(hwnd, instance, L"Tail Shade", 24, 350, 120, 18);
            CreateSettingsLabel(hwnd, instance, L"(head to tail)", 24, 368, 120, 18);
            HWND shadeSlider = CreateSettingsSlider(hwnd, instance, kTailShadeSlider, 0, 10, 0, 350);
            SendMessage(shadeSlider, TBM_SETRANGEMIN, FALSE, -100);
            SendMessage(shadeSlider, TBM_SETRANGEMAX, TRUE, 100);
            SendMessage(shadeSlider, TBM_SETTICFREQ, 20, 0);
            SendMessage(shadeSlider, TBM_SETPOS, TRUE, g_pendingSettings.tailShade);
            g_tailShadeValue = CreateSettingsLabel(hwnd, instance, L"", 400, 356, 90, 24);
            CreateSettingsLabel(hwnd, instance, L"Frame Rate", 24, 396, 110, 24);
            CreateSettingsSlider(hwnd, instance, kFrameRateSlider, 0, kFrameRateModeCount - 1, g_pendingSettings.frameRateMode, 390);
            g_frameRateValue = CreateSettingsLabel(hwnd, instance, L"", 400, 396, 90, 24);
            CreateSettingsLabel(hwnd, instance, L"Rocks", 24, 436, 110, 24);
            CreateSettingsSlider(hwnd, instance, kRockCountSlider, 0, 10, g_pendingSettings.rockCount, 430);
            g_rockCountValue = CreateSettingsLabel(hwnd, instance, L"", 400, 436, 90, 24);

            // Right column: colours and checkboxes.
            CreateSettingsLabel(hwnd, instance, L"Fish Colours", 510, 72, 200, 18);
            CreateWindowEx(0, L"BUTTON", L"Add...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                760, 92, 64, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddColourButton)), instance, nullptr);
            EnableWindow(GetDlgItem(hwnd, kAddColourButton), g_pendingSettings.colourCount < kMaxFishColours);
            CreateSettingsLabel(hwnd, instance, L"Click to change, right-click to remove", 510, 120, 300, 18);
            CreateWindowEx(0, L"BUTTON", L"Sort fish by colour (school only with their own colour)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 150, 430, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kColourSortCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kColourSortCheckbox), BM_SETCHECK,
                g_pendingSettings.colourSort ? BST_CHECKED : BST_UNCHECKED, 0);
            CreateWindowEx(0, L"BUTTON", L"Different colours actively avoid each other",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                530, 178, 410, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kColourAvoidCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kColourAvoidCheckbox), BM_SETCHECK,
                g_pendingSettings.colourAvoid ? BST_CHECKED : BST_UNCHECKED, 0);
            UpdateColourSortControls(hwnd);
            CreateWindowEx(0, L"BUTTON", L"Most fish near the set size (untick for an even spread)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 216, 430, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSizeBellCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kSizeBellCheckbox), BM_SETCHECK,
                g_pendingSettings.sizeBell ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(GetDlgItem(hwnd, kSizeBellCheckbox), g_pendingSettings.sizeVariation > 100);
            CreateWindowEx(0, L"BUTTON", L"3D depth (fish pass over and under each other, with shadows)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 254, 430, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDepthEffectCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kDepthEffectCheckbox), BM_SETCHECK,
                g_pendingSettings.depthEffect ? BST_CHECKED : BST_UNCHECKED, 0);
            CreateWindowEx(0, L"BUTTON", L"Show the desktop behind the fish (instead of black)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 282, 430, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDesktopBackgroundCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kDesktopBackgroundCheckbox), BM_SETCHECK,
                g_pendingSettings.desktopBackground ? BST_CHECKED : BST_UNCHECKED, 0);
            CreateWindowEx(0, L"BUTTON", L"Keep boids on their monitor",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 320, 430, 24, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRestrictToMonitorCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kRestrictToMonitorCheckbox), BM_SETCHECK,
                g_pendingSettings.restrictToMonitor ? BST_CHECKED : BST_UNCHECKED, 0);
            CreateWindowEx(0, L"BUTTON", L"V-Sync (each monitor at its own refresh rate)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 358, 430, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVSyncCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kVSyncCheckbox), BM_SETCHECK,
                g_pendingSettings.vsync ? BST_CHECKED : BST_UNCHECKED, 0);
            // The frame rate multiplier only applies without vsync.
            EnableWindow(GetDlgItem(hwnd, kFrameRateSlider), !g_pendingSettings.vsync);
            CreateWindowEx(0, L"BUTTON", L"Show FPS",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 386, 430, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowFpsCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kShowFpsCheckbox), BM_SETCHECK,
                g_pendingSettings.showFps ? BST_CHECKED : BST_UNCHECKED, 0);
            CreateWindowEx(0, L"BUTTON", L"Use a random saved preset each time the screensaver starts",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                510, 424, 430, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRandomPresetCheckbox)), instance, nullptr);
            SendMessage(GetDlgItem(hwnd, kRandomPresetCheckbox), BM_SETCHECK,
                g_pendingSettings.randomPreset ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(GetDlgItem(hwnd, kRandomPresetCheckbox), SendMessage(GetDlgItem(hwnd, kPresetCombo), CB_GETCOUNT, 0, 0) > 0);

            // Bottom: the full-width preview, then OK / Cancel.
            CreateSettingsLabel(hwnd, instance, L"Actual size preview:", 24, 476, 300, 20);
            CreateWindowEx(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                776, 596, 76, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsOk)), instance, nullptr);
            CreateWindowEx(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                864, 596, 76, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsCancel)), instance, nullptr);
            UpdateSettingsValueLabels(hwnd);
            g_previewLastTick = GetTickCount();
            SetTimer(hwnd, kPreviewTimer, 16, nullptr);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kPreviewTimer)
            {
                InvalidateRect(hwnd, &kSizePreviewRect, FALSE);
                return 0;
            }
            break;

        case WM_HSCROLL:
            UpdateSettingsValueLabels(hwnd);
            EnableWindow(GetDlgItem(hwnd, kSizeBellCheckbox), SendMessage(GetDlgItem(hwnd, kSizeVariationSlider), TBM_GETPOS, 0, 0) > 100);
            if (reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, kSizeSlider) || reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, kTailSlider) ||
                reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, kSizeVariationSlider) ||
                reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, kTailShadeSlider))
            {
                InvalidateRect(hwnd, &kSizePreviewRect, FALSE);
            }
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT paint = {};
            HDC windowDc = BeginPaint(hwnd, &paint);
            int sizeTenths = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSizeSlider), TBM_GETPOS, 0, 0));
            int speed = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSpeedSlider), TBM_GETPOS, 0, 0));
            int length = BoidLengthForSize(sizeTenths);
            double tailRatio = SendMessage(GetDlgItem(hwnd, kTailSlider), TBM_GETPOS, 0, 0) / 100.0;
            double shade = static_cast<int>(SendMessage(GetDlgItem(hwnd, kTailShadeSlider), TBM_GETPOS, 0, 0)) / 100.0;
            DrawColourSwatches(windowDc);
            int width = (std::max)(1, static_cast<int>(std::lround(FishLayoutFor(length, tailRatio).bodyHalfWidth * 2.0)));

            int variation = static_cast<int>(SendMessage(GetDlgItem(hwnd, kSizeVariationSlider), TBM_GETPOS, 0, 0));
            double spread = variation / 100.0;

            // Swim in place at the Speed slider's speed, with the same tail motion as the screensaver.
            DWORD now = GetTickCount();
            g_previewTime = std::fmod(g_previewTime + (std::min)(0.1, (now - g_previewLastTick) / 1000.0), 3600.0);
            g_previewLastTick = now;

            // Draw like the screensaver, on black at the exact on-screen pixel size: one fish per palette colour,
            // cycling through the palette every 2 s when they don't all fit. With Size Variability, at least three fish
            // show the size range from the smallest to the largest.
            int previewWidth = kSizePreviewRect.right - kSizePreviewRect.left;
            int previewHeight = kSizePreviewRect.bottom - kSizePreviewRect.top;
            HDC dc = CreateCompatibleDC(windowDc);
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = previewWidth;
            info.bmiHeader.biHeight = -previewHeight;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
            DWORD* pixels = static_cast<DWORD*>(bits);
            if (pixels)
            {
                std::memset(pixels, 0, static_cast<size_t>(previewWidth) * previewHeight * sizeof(DWORD));
                int colourCount = (std::max)(1, g_pendingSettings.colourCount);
                const int kMaxShown = 10;
                double lengths[kMaxShown];
                int shown = (std::min)(kMaxShown, (std::max)(colourCount, variation > 100 ? 3 : 1));
                double total = 0.0;
                for (; shown >= 1; --shown)
                {
                    total = 0.0;
                    for (int k = 0; k < shown; ++k)
                    {
                        double u = shown > 1 && variation > 100 ? -1.0 + 2.0 * k / (shown - 1) : 0.0;
                        lengths[k] = (std::max)(4.0, length * std::pow(spread, u));
                        total += lengths[k] * 1.2;
                    }
                    if (total <= previewWidth || shown == 1) break;
                }
                int first = colourCount > shown ? static_cast<int>((now / 2000) * shown % colourCount) : 0;
                double x = (previewWidth - total) * 0.5;
                for (int k = 0; k < shown; ++k)
                {
                    double fishLength = lengths[k];
                    double fishSpeed = speed * std::pow(fishLength / length, -0.2);
                    double omega = 6.2831853 * (std::min)(5.0, (std::max)(0.8, 1.5 * fishSpeed / fishLength));
                    double phase = std::fmod(g_previewTime * omega, 6.2831853) + k * 1.3;
                    double amplitude = (std::min)(fishLength * 0.04, fishSpeed * 0.1 / omega);
                    double yawAmplitude = std::atan2(amplitude * omega, fishSpeed);
                    x += fishLength * 0.6;
                    DrawPreviewFish(pixels, previewWidth, previewHeight, x, previewHeight / 2.0 + 8.0,
                        fishLength, tailRatio, yawAmplitude * std::cos(phase), FishTailAngle(fishSpeed, fishLength, phase),
                        g_pendingSettings.colours[(first + k) % colourCount], shade);
                    x += fishLength * 0.6;
                }
            }
            wchar_t caption[128];
            if (variation > 100)
            {
                int smallest = (std::max)(4, static_cast<int>(std::lround(length / spread)));
                int largest = static_cast<int>(std::lround(length * spread));
                wsprintf(caption, L"%d x %d px  (%d.%d%% of %d px), from %d to %d px long", length, width, sizeTenths / 10, sizeTenths % 10,
                    ReferenceScreenHeight(), smallest, largest);
            }
            else
            {
                wsprintf(caption, L"%d x %d px  (%d.%d%% of %d px)", length, width, sizeTenths / 10, sizeTenths % 10, ReferenceScreenHeight());
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(160, 160, 160));
            HGDIOBJ oldFont = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            RECT text = { 6, 4, previewWidth - 6, 20 };
            DrawText(dc, caption, -1, &text, DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            BitBlt(windowDc, kSizePreviewRect.left, kSizePreviewRect.top, previewWidth, previewHeight, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(dc);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == kColourSortCheckbox && HIWORD(wParam) == BN_CLICKED)
            {
                UpdateColourSortControls(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == kSizeBellCheckbox && HIWORD(wParam) == BN_CLICKED)
            {
                UpdateSettingsValueLabels(hwnd);
                InvalidateRect(hwnd, &kSizePreviewRect, FALSE);
                return 0;
            }
            if (LOWORD(wParam) == kVSyncCheckbox && HIWORD(wParam) == BN_CLICKED)
            {
                bool vsync = SendMessage(GetDlgItem(hwnd, kVSyncCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
                EnableWindow(GetDlgItem(hwnd, kFrameRateSlider), !vsync);
                return 0;
            }
            if (LOWORD(wParam) == kAddColourButton && g_pendingSettings.colourCount < kMaxFishColours)
            {
                COLORREF colour = g_pendingSettings.colours[g_pendingSettings.colourCount - 1];
                if (PickColour(hwnd, &colour))
                {
                    g_pendingSettings.colours[g_pendingSettings.colourCount++] = colour;
                    ColoursChanged(hwnd);
                }
                return 0;
            }
            if (LOWORD(wParam) == kPresetCombo && (HIWORD(wParam) == CBN_EDITCHANGE || HIWORD(wParam) == CBN_SELCHANGE))
            {
                HWND combo = GetDlgItem(hwnd, kPresetCombo);
                std::wstring name = PresetComboText(hwnd);
                if (HIWORD(wParam) == CBN_SELCHANGE)
                {
                    // The edit text updates after this notification, so read the chosen item itself.
                    LRESULT selected = SendMessage(combo, CB_GETCURSEL, 0, 0);
                    wchar_t text[128] = {};
                    if (selected != CB_ERR && SendMessage(combo, CB_GETLBTEXTLEN, selected, 0) < 128)
                    {
                        SendMessage(combo, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text));
                        name = text;
                        Settings loaded = g_pendingSettings;
                        if (LoadPreset(name, loaded))
                        {
                            g_pendingSettings = loaded;
                            WriteSettingsControls(hwnd);
                        }
                    }
                }
                UpdatePresetButtons(hwnd, name);
                return 0;
            }
            if (LOWORD(wParam) == kPresetSaveButton)
            {
                std::wstring name = PresetComboText(hwnd);
                HWND combo = GetDlgItem(hwnd, kPresetCombo);
                if (name.empty())
                {
                    return 0;
                }
                if (name.find(L'\\') != std::wstring::npos)
                {
                    MessageBox(hwnd, L"Preset names can't contain a backslash (\\).", L"Shoal", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                bool exists = SendMessage(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name.c_str())) != CB_ERR;
                if (exists && MessageBox(hwnd, (L"Replace the preset \"" + name + L"\" with the current settings?").c_str(), L"Shoal", MB_YESNO | MB_ICONQUESTION) != IDYES)
                {
                    return 0;
                }
                ReadSettingsControls(hwnd);
                if (!SavePreset(name, g_pendingSettings))
                {
                    MessageBox(hwnd, L"The preset couldn't be saved.", L"Shoal", MB_OK | MB_ICONERROR);
                    return 0;
                }
                if (!exists)
                {
                    SendMessage(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                }
                SendMessage(combo, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name.c_str()));
                UpdatePresetButtons(hwnd, name);
                return 0;
            }
            if (LOWORD(wParam) == kPresetDeleteButton)
            {
                std::wstring name = PresetComboText(hwnd);
                HWND combo = GetDlgItem(hwnd, kPresetCombo);
                LRESULT index = SendMessage(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name.c_str()));
                if (name.empty() || index == CB_ERR ||
                    MessageBox(hwnd, (L"Delete the preset \"" + name + L"\"?").c_str(), L"Shoal", MB_YESNO | MB_ICONQUESTION) != IDYES)
                {
                    return 0;
                }
                DeletePreset(name);
                SendMessage(combo, CB_DELETESTRING, static_cast<WPARAM>(index), 0);
                SetWindowText(combo, L"");
                UpdatePresetButtons(hwnd, L"");
                return 0;
            }
            if (LOWORD(wParam) == kSettingsOk)
            {
                ReadSettingsControls(hwnd);
                g_settings = g_pendingSettings;
                SaveSettings();
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == kSettingsCancel)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_LBUTTONDOWN:
        {
            int index = SwatchAt(lParam);
            if (index >= 0 && PickColour(hwnd, &g_pendingSettings.colours[index]))
            {
                ColoursChanged(hwnd);
            }
            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            int index = SwatchAt(lParam);
            if (index >= 0 && g_pendingSettings.colourCount > 1)
            {
                for (int k = index; k + 1 < g_pendingSettings.colourCount; ++k)
                {
                    g_pendingSettings.colours[k] = g_pendingSettings.colours[k + 1];
                }
                --g_pendingSettings.colourCount;
                ColoursChanged(hwnd);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kPreviewTimer);
            if (g_titleFont)
            {
                DeleteObject(g_titleFont);
                g_titleFont = nullptr;
            }
            if (g_iconFont)
            {
                DeleteObject(g_iconFont);
                g_iconFont = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

static int RunSettingsWindow(HINSTANCE instance, HWND parent)
{
    const wchar_t* className = L"ShoalSettingsWindow";
    WNDCLASS settingsClass = {};
    settingsClass.lpfnWndProc = SettingsWndProc;
    settingsClass.hInstance = instance;
    settingsClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    settingsClass.lpszClassName = className;

    if (!RegisterClass(&settingsClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return 1;
    }

    HWND hwnd = CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        className,
        L"Shoal Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 680,
        parent, nullptr, instance, nullptr);
    if (!hwnd)
    {
        return 1;
    }

    CenterSettingsWindow(hwnd, parent);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG message = {};
    while (GetMessage(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    return static_cast<int>(message.wParam);
}

static bool ParseScreenSaverCommand(PWSTR commandLine, wchar_t expectedCommand, HWND* parent)
{
    if (!commandLine)
    {
        return false;
    }

    while (*commandLine == L' ' || *commandLine == L'\t') ++commandLine;
    if (commandLine[0] != L'/' && commandLine[0] != L'-')
    {
        return false;
    }

    wchar_t command = commandLine[1];
    if (command != expectedCommand && command != (expectedCommand == L'c' ? L'C' : L'P'))
    {
        return false;
    }
    if (commandLine[2] != L'\0' && commandLine[2] != L':' && commandLine[2] != L' ' && commandLine[2] != L'\t')
    {
        return false;
    }

    *parent = nullptr;
    // Windows passes the window handle as "/p 1234"; "/p:1234" is also accepted.
    const wchar_t* value = commandLine + 2;
    if (*value == L':') ++value;
    while (*value == L' ' || *value == L'\t') ++value;
    if (*value >= L'0' && *value <= L'9')
    {
        *parent = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(wcstoull(value, nullptr, 10)));
    }
    return true;
}

static bool IsSettingsCommand(PWSTR commandLine, HWND* parent)
{
    return ParseScreenSaverCommand(commandLine, L'c', parent);
}

static bool IsPreviewCommand(PWSTR commandLine, HWND* parent)
{
    return ParseScreenSaverCommand(commandLine, L'p', parent);
}

static bool RegisterRendererWindowClass(HINSTANCE instance)
{
    WNDCLASS windowClass = {};
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"ShoalScreenSaverWindow";
    return RegisterClass(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// One simulation step using the real elapsed time, then the windows that are due are drawn.
static void RunFrame(const std::vector<RenderWindowState*>& ready)
{
    LARGE_INTEGER simStart = {};
    QueryPerformanceCounter(&simStart);
    float elapsed = g_lastSimulationCounter
        ? static_cast<float>(static_cast<double>(simStart.QuadPart - g_lastSimulationCounter) / g_qpcFrequency.QuadPart)
        : 1.0f / 60.0f;
    // Monitors with different vblank phases can become due within a fraction of a millisecond of each other.
    // Draw the current state then instead of stepping by an inflated minimum delta.
    bool stepped = elapsed >= 0.0001f;
    if (stepped)
    {
        g_lastSimulationCounter = simStart.QuadPart;
        g_simulationDeltaSeconds = Clamp(elapsed, 0.0001f, 0.1f);
        UpdateGpuBoids(g_simulationDeltaSeconds);
    }

    const double toMs = 1000.0 / static_cast<double>(g_qpcFrequency.QuadPart);
    LARGE_INTEGER renderStart = {};
    QueryPerformanceCounter(&renderStart);
    float simCpuMs = static_cast<float>((renderStart.QuadPart - simStart.QuadPart) * toMs);
    for (size_t i = 0; i < ready.size(); ++i)
    {
        RenderWindowState* state = ready[i];
        RenderFrame(state);
        LARGE_INTEGER frameEnd = {};
        QueryPerformanceCounter(&frameEnd);
        if (g_frameLogEnabled)
        {
            bool carriesStep = stepped && i == 0;
            FrameSample sample = {
                frameEnd.QuadPart,
                state->windowIndex,
                state->lastFrameCounter ? static_cast<float>((frameEnd.QuadPart - state->lastFrameCounter) * toMs) : 0.0f,
                carriesStep ? g_simulationDeltaSeconds * 1000.0f : 0.0f,
                carriesStep ? simCpuMs : 0.0f,
                static_cast<float>((frameEnd.QuadPart - renderStart.QuadPart) * toMs),
                static_cast<float>(state->lastPresentMs),
                state->lastStats.PresentCount,
                state->lastStats.PresentRefreshCount,
                state->lastStats.SyncRefreshCount,
                state->lastStats.SyncQPCTime.QuadPart
            };
            g_frameSamples.push_back(sample);
        }
        state->lastFrameCounter = frameEnd.QuadPart;
        renderStart = frameEnd;
    }
}

// Paces each monitor window independently:
//   - vsync: waits on each swap chain's frame latency waitable object, which signals at that monitor's vblank;
//   - no vsync: per-window deadlines from the monitor refresh rate and Frame Rate multiplier, using a
//     high-resolution waitable timer plus a short spin.
// Input stays responsive because the waits also wake on window messages.
static int RunFrameLoop()
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool raisedTimerResolution = false;
    if (!timer)
    {
        timeBeginPeriod(1);
        raisedTimerResolution = true;
        timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
    }

    std::vector<RenderWindowState*> ready;
    std::vector<HANDLE> waitHandles;
    std::vector<RenderWindowState*> waitOwners;
    MSG message = {};
    for (;;)
    {
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                if (timer) CloseHandle(timer);
                if (raisedTimerResolution) timeEndPeriod(1);
                return static_cast<int>(message.wParam);
            }
            TranslateMessage(&message);
            DispatchMessage(&message);
        }

        if (g_debugLogDurationMs > 0 && GetTickCount() - g_debugLogStartTick >= g_debugLogDurationMs)
        {
            PostQuitMessage(0);
            continue;
        }
        if (g_renderWindows.empty())
        {
            WaitMessage();
            continue;
        }

        ready.clear();
        waitHandles.clear();
        waitOwners.clear();
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        LONGLONG earliestDeadline = LLONG_MAX;
        for (RenderWindowState* state : g_renderWindows)
        {
            if (state->frameLatencyWaitable)
            {
                if (WaitForSingleObject(state->frameLatencyWaitable, 0) == WAIT_OBJECT_0)
                {
                    ready.push_back(state);
                }
                else
                {
                    waitHandles.push_back(state->frameLatencyWaitable);
                    waitOwners.push_back(state);
                }
                continue;
            }

            double fps = TargetFrameRate(state);
            if (fps <= 0.0)
            {
                ready.push_back(state);
                continue;
            }
            LONGLONG period = static_cast<LONGLONG>(static_cast<double>(g_qpcFrequency.QuadPart) / fps);
            if (now.QuadPart >= state->nextFrameCounter)
            {
                state->nextFrameCounter += period;
                if (now.QuadPart - state->nextFrameCounter > period)
                {
                    state->nextFrameCounter = now.QuadPart + period;
                }
                ready.push_back(state);
            }
            else
            {
                earliestDeadline = (std::min)(earliestDeadline, state->nextFrameCounter);
            }
        }

        if (!ready.empty())
        {
            RunFrame(ready);
            continue;
        }

        DWORD timeoutMs = 100;
        if (earliestDeadline != LLONG_MAX)
        {
            double remainingMs = static_cast<double>(earliestDeadline - now.QuadPart) * 1000.0 / g_qpcFrequency.QuadPart;
            if (remainingMs <= 1.0)
            {
                YieldProcessor();
                continue;
            }
            if (timer)
            {
                // Wake slightly early and spin the rest, since timer wake-ups can be late by a fraction of a ms.
                LARGE_INTEGER due = {};
                due.QuadPart = -static_cast<LONGLONG>((remainingMs - 0.5) * 10000.0);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                waitHandles.push_back(timer);
                waitOwners.push_back(nullptr);
            }
            else
            {
                timeoutMs = static_cast<DWORD>(remainingMs - 1.0);
            }
        }

        DWORD result = MsgWaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()),
            waitHandles.empty() ? nullptr : waitHandles.data(), FALSE, timeoutMs, QS_ALLINPUT);
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + waitHandles.size() && waitOwners[result - WAIT_OBJECT_0])
        {
            // That wait consumed this window's vblank signal; collect any other window that is due as well.
            size_t signaled = result - WAIT_OBJECT_0;
            ready.push_back(waitOwners[signaled]);
            for (size_t i = 0; i < waitOwners.size(); ++i)
            {
                if (i != signaled && waitOwners[i] && WaitForSingleObject(waitHandles[i], 0) == WAIT_OBJECT_0)
                {
                    ready.push_back(waitOwners[i]);
                }
            }
            RunFrame(ready);
        }
    }
}

static void ReportGpuUnavailable()
{
    for (RenderWindowState* state : std::vector<RenderWindowState*>(g_renderWindows))
    {
        DestroyWindow(state->hwnd);
    }
    // Closing the windows posts WM_QUIT, which would dismiss the message box immediately.
    MSG message;
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
    {
    }
    if (g_debugLogFile)
    {
        std::fprintf(g_debugLogFile, "# error D3D11 GPU Not Found\n");
        std::fclose(g_debugLogFile);
        g_debugLogFile = nullptr;
    }
    MessageBox(nullptr, L"D3D11 GPU Not Found", L"Shoal", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

static int RunPreviewWindow(HINSTANCE instance, HWND parent)
{
    if (!parent || !IsWindow(parent) || !RegisterRendererWindowClass(instance))
    {
        return 1;
    }

    RECT clientRect = {};
    GetClientRect(parent, &clientRect);
    if (clientRect.right <= clientRect.left || clientRect.bottom <= clientRect.top)
    {
        return 1;
    }

    // Simulate an area kPreviewScale times the preview's size and draw it scaled down, so the fish
    // are big enough to see in the small monitor image. The fish count follows the coverage setting
    // for that area. Rocks are never shown here; the dialog's Preview button runs the normal /s mode.
    const int kPreviewScale = 3;
    g_isPreview = true;
    g_settings.showFps = false;
    g_settings.rockCount = 0;
    RECT screenRect = { 0, 0, clientRect.right * kPreviewScale, clientRect.bottom * kPreviewScale };
    g_monitorBounds.push_back(screenRect);
    CreateBoids();
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&g_startCounter));
    g_lastSimulationCounter = 0;
    CreateMonitorWindow(instance, screenRect, parent, clientRect);
    if (g_gpuUnavailable)
    {
        // The Control Panel preview is a tiny child window, so it stays blank rather than popping up a message box.
        return 1;
    }

    return RunFrameLoop();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int)
{
    INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls), ICC_BAR_CLASSES };
    InitCommonControlsEx(&commonControls);
    srand(GetTickCount());
    QueryPerformanceFrequency(&g_qpcFrequency);
    LoadSettings();
    g_settings.boidLength = BoidLengthForSize(g_settings.sizeTenths);

    HWND settingsParent = nullptr;
    if (IsSettingsCommand(pCmdLine, &settingsParent))
    {
        return RunSettingsWindow(hInstance, settingsParent);
    }

    int randomPresetFlag = g_settings.randomPreset ? 1 : 0;
    ParseIntOverride(pCmdLine, L"randompreset:", 0, 1, &randomPresetFlag);
    g_settings.randomPreset = randomPresetFlag != 0;
    ApplyRandomPreset();
    g_settings.boidLength = BoidLengthForSize(g_settings.sizeTenths);

    HWND previewParent = nullptr;
    if (IsPreviewCommand(pCmdLine, &previewParent))
    {
        return RunPreviewWindow(hInstance, previewParent);
    }

    if (!RegisterRendererWindowClass(hInstance))
    {
        return 1;
    }

    g_monitorBounds = EnumerateMonitorRects();

    ParseIntOverride(pCmdLine, L"randomness:", 0, 100, &g_settings.randomness);
    ParseIntOverride(pCmdLine, L"speed:", 1, 1000, &g_settings.speedPixelsPerSecond);
    ParseIntOverride(pCmdLine, L"rocks:", 0, 10, &g_settings.rockCount);
    g_boidCountOverridden = ParseIntOverride(pCmdLine, L"boids:", kMinBoidCount, kMaxBoidCount, &g_settings.boidCount);
    ParseIntOverride(pCmdLine, L"coverage:", 0, 100, &g_settings.fishCoverage);
    ParseIntOverride(pCmdLine, L"speedvar:", 0, 10, &g_settings.speedVariability);
    ParseIntOverride(pCmdLine, L"tail:", 10, 100, &g_settings.tailPercent);
    ParseIntOverride(pCmdLine, L"shade:", -100, 100, &g_settings.tailShade);
    ParseSizeOverride(pCmdLine, &g_settings.sizeTenths);
    g_settings.boidLength = BoidLengthForSize(g_settings.sizeTenths);
    ParseIntOverride(pCmdLine, L"framerate:", 1, 100000, &g_settings.frameRate);
    ParseIntOverride(pCmdLine, L"fpsmode:", 0, kFrameRateModeCount - 1, &g_settings.frameRateMode);
    int overrideFlag = g_settings.restrictToMonitor ? 1 : 0;
    ParseIntOverride(pCmdLine, L"restrict:", 0, 1, &overrideFlag);
    g_settings.restrictToMonitor = overrideFlag != 0;
    overrideFlag = g_settings.showFps ? 1 : 0;
    ParseIntOverride(pCmdLine, L"showfps:", 0, 1, &overrideFlag);
    g_settings.showFps = overrideFlag != 0;
    overrideFlag = g_settings.vsync ? 1 : 0;
    ParseIntOverride(pCmdLine, L"vsync:", 0, 1, &overrideFlag);
    g_settings.vsync = overrideFlag != 0;
    overrideFlag = g_settings.colourSort ? 1 : 0;
    ParseIntOverride(pCmdLine, L"coloursort:", 0, 1, &overrideFlag);
    g_settings.colourSort = overrideFlag != 0;
    overrideFlag = g_settings.colourAvoid ? 1 : 0;
    ParseIntOverride(pCmdLine, L"colouravoid:", 0, 1, &overrideFlag);
    g_settings.colourAvoid = overrideFlag != 0;
    ParseIntOverride(pCmdLine, L"sizevar:", 100, 300, &g_settings.sizeVariation);
    overrideFlag = g_settings.sizeBell ? 1 : 0;
    ParseIntOverride(pCmdLine, L"sizebell:", 0, 1, &overrideFlag);
    g_settings.sizeBell = overrideFlag != 0;
    overrideFlag = g_settings.depthEffect ? 1 : 0;
    ParseIntOverride(pCmdLine, L"depth:", 0, 1, &overrideFlag);
    g_settings.depthEffect = overrideFlag != 0;
    overrideFlag = g_settings.desktopBackground ? 1 : 0;
    ParseIntOverride(pCmdLine, L"desktop:", 0, 1, &overrideFlag);
    g_settings.desktopBackground = overrideFlag != 0;
    g_boidLogEnabled = !HasFlag(pCmdLine, L"noboidlog");
    CreateRocks();
    CreateBoids();
    if (ParseDebugLogCommand(pCmdLine, &g_debugLogDurationMs))
    {
        OpenDebugLog();
        g_frameLogEnabled = g_debugLogFile != nullptr;
        g_frameSamples.reserve(1 << 20);
    }
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&g_startCounter));
    g_lastSimulationCounter = 0;

    if (g_settings.desktopBackground)
    {
        CaptureDesktop();
    }
    for (const RECT& monitorRect : g_monitorBounds)
    {
        CreateMonitorWindow(hInstance, monitorRect);
        if (g_gpuUnavailable)
        {
            ReportGpuUnavailable();
            return 1;
        }
    }
    g_desktopCaptures.clear();
    g_desktopCaptures.shrink_to_fit();

    GetCursorPos(&g_initialCursorPosition);
    g_inputReadyTick = GetTickCount() + 1000;
    g_debugLogStartTick = GetTickCount();

    // Hide the cursor until the screensaver closes; WM_SETCURSOR also clears it over the windows.
    while (ShowCursor(FALSE) >= 0) {}
    SetCursor(nullptr);

    int exitCode = RunFrameLoop();

    while (ShowCursor(TRUE) < 0) {}

    SAFE_RELEASE(g_gpuBoidStagingBuffer);
    WriteFrameLog();
    if (g_debugLogFile)
    {
        std::fclose(g_debugLogFile);
        g_debugLogFile = nullptr;
    }
    return exitCode;
}
