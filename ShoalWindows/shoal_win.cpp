// Shoal for Windows: the screensaver (.scr) around the shared engine in ../ShoalCore (OpenGL 4.3 through WGL).
// The settings, registry values, presets and settings dialog come from the original Direct3D 11 build
// (../ShoalNative/Main.cpp); the simulation, drawing and shaders are the same ones the Linux, Plasma and Android
// builds use. Built from Linux with clang-cl (see build.sh).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "shoal_gl.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <shellapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shlguid.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include "shoal_engine.h"
#include "shoal_fishshape.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "uuid.lib")

static bool g_isPreview = false;
static bool g_isWallpaper = false;         // "/wallpaper": behind the desktop icons, input goes to the desktop
static UINT g_presentInterval = 1;         // V-Sync: present every n-th refresh (the wallpaper's frame-rate setting)
static void (*g_loopHook)() = nullptr;     // called by the frame loop every turn (the wallpaper's housekeeping)
static POINT g_initialCursorPosition = {};
static DWORD g_inputReadyTick = 0;
static const wchar_t* kSettingsKey = L"Software\\Shoal";
static std::vector<RECT> g_monitorBounds;

// Starts this program again with `args` (e.g. "/c"). Not ShellExecute: Windows' "open" verb for a .scr runs the
// screensaver ("%1" /S) and drops the arguments.
static bool LaunchSelf(const wchar_t* args)
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileName(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + args;
    STARTUPINFO startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (!CreateProcess(exe, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

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
    unsigned speciesMask; // Fish shapes that swim: bit n = species n+1 (the table in ShoalCore/shaders/fish.vert).
    int speciesMode; // 0 = a random mix of the chosen species, 1 = one species per colour.
};

static const int kFrameRateModeCount = 6;
static const wchar_t* kFrameRateModeNames[kFrameRateModeCount] = { L"1/2x", L"1x", L"2x", L"3x", L"4x", L"Unlimited" };
static const double kFrameRateModeMultipliers[kFrameRateModeCount] = { 0.5, 1.0, 2.0, 3.0, 4.0, 0.0 };

static const int kMaxFishColours = 10;
// Default ocean palette: silver, yellow tang, clownfish orange, blue tang, teal.
static Settings g_settings = { 42, 20, 0, 26, false, 0, 5, false, true, 1, 24, 5, 50, 33,
    5, { RGB(220, 230, 240), RGB(255, 205, 40), RGB(255, 120, 30), RGB(40, 110, 230), RGB(40, 200, 190) }, -40, false, false, 100, true, true, false, false, 0x3FF, 0 };
static bool g_boidCountOverridden = false;
// Spacing, wall and rock avoidance were tuned for 20 px fish; they scale from this reference length.
static int ReferenceScreenHeight()
{
    int height = GetSystemMetrics(SM_CYSCREEN);
    return height > 0 ? height : 1080;
}

static int BoidLengthForSize(int sizeTenths)
{
    return (std::max)(4, static_cast<int>(std::lround(ReferenceScreenHeight() * sizeTenths / 1000.0)));
}
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
static const int kSpeciesCheckbox0 = 1130; // ..1139, one per species
static const int kSpeciesModeCombo = 1140;
static const int kWallpaperButton = 1150;
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
static const RECT kSizePreviewRect = { 24, 580, 940, 660 };
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
    settings.speciesMask = static_cast<unsigned>(ReadSetting(key, L"SpeciesMask", static_cast<int>(settings.speciesMask), 1, (1 << kShoalSpeciesCount) - 1));
    settings.speciesMode = ReadSetting(key, L"SpeciesMode", settings.speciesMode, 0, 1);
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
    WriteSetting(key, L"SpeciesMask", static_cast<int>(settings.speciesMask));
    WriteSetting(key, L"SpeciesMode", settings.speciesMode);
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

// Head-to-tail lightness change; must match fish.frag. Returns a DIB pixel (0x00RRGGBB).
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

static bool InsidePolygon(const ShoalPoint* p, size_t n, float x, float y)
{
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
        if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x) inside = !inside;
    return inside;
}

// The fish, swimming in place: the same outline, species, wave and shading as the screensaver (shoal_fishshape),
// rasterized into a top-down 32-bit DIB. (x, y) is the middle of the fish; it faces right.
static void DrawPreviewFish(DWORD* pixels, int width, int height, double x, double y, double length, double tailRatio,
    double phase, int species, COLORREF colour, double shade)
{
    const std::vector<ShoalFishShape>& shapes = ShoalSpeciesShapes();
    if (shapes.empty()) return;
    const ShoalFishShape& shape = shapes[(size_t)species % shapes.size()];
    ShoalFishOutline o = ShoalFishOutlineFor(shape, (float)length, (float)(tailRatio / 0.33), 1.0f, (float)phase);
    // The outline has its nose at (0, 0), facing +x with the body behind it: move the nose to x + length / 2.
    const float nx = (float)(x + length * 0.5), ny = (float)y;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (ShoalPoint& p : o.body) { p.x += nx; p.y += ny; minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x); minY = (std::min)(minY, p.y); maxY = (std::max)(maxY, p.y); }
    for (auto& fin : o.fins) for (ShoalPoint& p : fin) { p.x += nx; p.y += ny; minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x); minY = (std::min)(minY, p.y); maxY = (std::max)(maxY, p.y); }
    int left = (std::max)(0, (int)std::floor(minX)), right = (std::min)(width - 1, (int)std::ceil(maxX));
    int top = (std::max)(0, (int)std::floor(minY)), bottom = (std::min)(height - 1, (int)std::ceil(maxY));
    for (int py = top; py <= bottom; ++py)
    {
        for (int px = left; px <= right; ++px)
        {
            float cx = px + 0.5f, cy = py + 0.5f;
            if (!InsidePolygon(o.body.data(), o.body.size(), cx, cy) && !InsidePolygon(o.fins[0], 3, cx, cy) &&
                !InsidePolygon(o.fins[1], 3, cx, cy))
                continue;
            pixels[py * width + px] = ShadeFishColour(colour, shade, (nx - cx) / length);
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
    unsigned mask = 0;
    for (int i = 0; i < kShoalSpeciesCount; ++i)
        if (SendMessage(GetDlgItem(hwnd, kSpeciesCheckbox0 + i), BM_GETCHECK, 0, 0) == BST_CHECKED) mask |= 1u << i;
    g_pendingSettings.speciesMask = mask ? mask : 1u;
    LRESULT mode = SendMessage(GetDlgItem(hwnd, kSpeciesModeCombo), CB_GETCURSEL, 0, 0);
    g_pendingSettings.speciesMode = mode == 1 ? 1 : 0;
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
    for (int i = 0; i < kShoalSpeciesCount; ++i)
        SendMessage(GetDlgItem(hwnd, kSpeciesCheckbox0 + i), BM_SETCHECK, (g_pendingSettings.speciesMask >> i) & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(GetDlgItem(hwnd, kSpeciesModeCombo), CB_SETCURSEL, g_pendingSettings.speciesMode, 0);
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

            // Species: which fish shapes swim (shapes in ShoalCore/shaders/fish.vert), and how they are mixed.
            CreateSettingsLabel(hwnd, instance, L"Fish Species", 24, 470, 200, 18);
            for (int i = 0; i < kShoalSpeciesCount; ++i)
            {
                wchar_t text[32];
                swprintf_s(text, L"%d %hs", i + 1, kShoalSpeciesNames[i]);
                CreateWindowEx(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    24 + (i % 5) * 150, 492 + (i / 5) * 26, 145, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeciesCheckbox0 + i)), instance, nullptr);
                SendMessage(GetDlgItem(hwnd, kSpeciesCheckbox0 + i), BM_SETCHECK,
                    (g_pendingSettings.speciesMask >> i) & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            CreateSettingsLabel(hwnd, instance, L"Species mix", 790, 470, 150, 18);
            HWND mix = CreateWindowEx(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                790, 492, 150, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeciesModeCombo)), instance, nullptr);
            SendMessage(mix, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessage(mix, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Random mix"));
            SendMessage(mix, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"One per colour"));
            SendMessage(mix, CB_SETCURSEL, g_pendingSettings.speciesMode, 0);

            // Bottom: the full-width preview, then OK / Cancel.
            CreateSettingsLabel(hwnd, instance, L"Actual size preview:", 24, 556, 300, 20);
            CreateWindowEx(0, L"BUTTON", L"Start live wallpaper", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                24, 676, 170, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWallpaperButton)), instance, nullptr);
            CreateWindowEx(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                776, 676, 76, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsOk)), instance, nullptr);
            CreateWindowEx(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                864, 676, 76, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsCancel)), instance, nullptr);
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
            const std::vector<ShoalFishShape>& shapes = ShoalSpeciesShapes();
            const double classicWidth = shapes.empty() ? 0.156 : shapes[0].width, classicTail = shapes.empty() ? 0.26 : shapes[0].tail;
            int width = (std::max)(1, static_cast<int>(std::lround(length / (1.0 + classicTail * tailRatio / 0.33) * classicWidth * 2.0)));
            std::vector<int> speciesList;
            for (int i = 0; i < kShoalSpeciesCount; ++i)
                if (SendMessage(GetDlgItem(hwnd, kSpeciesCheckbox0 + i), BM_GETCHECK, 0, 0) == BST_CHECKED) speciesList.push_back(i);
            if (speciesList.empty()) speciesList.push_back(0);
            bool speciesPerColour = SendMessage(GetDlgItem(hwnd, kSpeciesModeCombo), CB_GETCURSEL, 0, 0) == 1;

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
                    // The tail beat is tied to distance swum, as in sim.comp: one beat per 0.665 fish lengths.
                    double fishSpeed = speed * std::pow(fishLength / length, -0.2);
                    double phase = std::fmod(g_previewTime * 6.2831853 * fishSpeed / (0.665 * fishLength), 6.2831853) + k * 1.3;
                    int colourIndex = (first + k) % colourCount;
                    int species = speciesList[(speciesPerColour ? colourIndex : first + k) % speciesList.size()];
                    x += fishLength * 0.6;
                    DrawPreviewFish(pixels, previewWidth, previewHeight, x, previewHeight / 2.0 + 8.0,
                        fishLength, tailRatio, phase, species, g_pendingSettings.colours[colourIndex], shade);
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
            if (LOWORD(wParam) >= kSpeciesCheckbox0 && LOWORD(wParam) < kSpeciesCheckbox0 + kShoalSpeciesCount && HIWORD(wParam) == BN_CLICKED)
            {
                // Keep at least one species.
                bool any = false;
                for (int i = 0; i < kShoalSpeciesCount; ++i)
                    any = any || SendMessage(GetDlgItem(hwnd, kSpeciesCheckbox0 + i), BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (!any) SendMessage(GetDlgItem(hwnd, LOWORD(wParam)), BM_SETCHECK, BST_CHECKED, 0);
                InvalidateRect(hwnd, &kSizePreviewRect, FALSE);
                return 0;
            }
            if (LOWORD(wParam) == kWallpaperButton)
            {
                // The fish behind the desktop icons (a separate process with a tray icon); it uses these settings.
                LaunchSelf(L"/wallpaper");
                return 0;
            }
            if (LOWORD(wParam) == kSpeciesModeCombo && HIWORD(wParam) == CBN_SELCHANGE)
            {
                InvalidateRect(hwnd, &kSizePreviewRect, FALSE);
                return 0;
            }
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
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
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
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 760,
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
    windowClass.style = CS_OWNDC; // a GL pixel format and DC per window
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"ShoalScreenSaverWindow";
    return RegisterClass(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// ---------------------------------------------------------------------------------------------------------------- engine
// One ShoalEngine and one OpenGL 4.3 core context, shared by every monitor window (each window's DC has the same pixel
// format, so the context can be made current on any of them). The simulation steps once per frame, then each window
// that is due draws its view of it.

static ShoalEngine* g_engine = nullptr;
static HGLRC g_glContext = nullptr;
static std::string g_glError;
typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC_)(int);
typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC_)(HDC, HGLRC, const int*);
static PFNWGLSWAPINTERVALEXTPROC_ g_wglSwapInterval = nullptr;
static FILE* g_log = nullptr;
static int g_runSeconds = 0;


// %LOCALAPPDATA%\Shoal\shoal.log: GL renderer, fish count, errors (useful when the screensaver can't be watched).
static void OpenLog(const wchar_t* name = L"shoal.log")
{
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariable(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 32) return;
    wcscat_s(path, L"\\Shoal");
    CreateDirectory(path, nullptr);
    wcscat_s(path, L"\\");
    wcscat_s(path, name);
    g_log = _wfsopen(path, L"w", _SH_DENYNO); // readable while Shoal runs
    ShoalEngine::setLogger([](const std::string& line) {
        if (g_log) { std::fprintf(g_log, "%s\n", line.c_str()); std::fflush(g_log); }
        OutputDebugStringA(("Shoal: " + line + "\n").c_str());
    });
}

static void Log(const char* format, ...)
{
    if (!g_log) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// Vertical blank of one monitor, as an event: a thread waits with D3DKMTWaitForVerticalBlankEvent (gdi32) and sets
// an auto-reset event at each blank, like the original's per-swap-chain frame latency waitable objects. This lets
// every monitor draw at its own refresh rate from one thread and one GL context.
struct D3DKMT_OPENADAPTERFROMHDC_ { HDC hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
struct D3DKMT_WAITFORVERTICALBLANKEVENT_ { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; };
struct D3DKMT_CLOSEADAPTER_ { UINT hAdapter; };
typedef LONG(APIENTRY* OpenAdapterFromHdcFn)(D3DKMT_OPENADAPTERFROMHDC_*);
typedef LONG(APIENTRY* WaitForVerticalBlankEventFn)(const D3DKMT_WAITFORVERTICALBLANKEVENT_*);
typedef LONG(APIENTRY* CloseAdapterFn)(const D3DKMT_CLOSEADAPTER_*);

struct VBlankWaiter
{
    HANDLE event = nullptr;
    HANDLE thread = nullptr;
    volatile LONG stop = 0;
    volatile LONG blanks = 0, failures = 0; // diagnostics
    D3DKMT_WAITFORVERTICALBLANKEVENT_ wait = {};
    WaitForVerticalBlankEventFn waitFn = nullptr;
    CloseAdapterFn closeFn = nullptr;

    static DWORD WINAPI Run(void* p)
    {
        VBlankWaiter* w = static_cast<VBlankWaiter*>(p);
        while (!w->stop)
        {
            if (w->waitFn(&w->wait) != 0) { InterlockedIncrement(&w->failures); Sleep(4); continue; } // e.g. monitor off
            InterlockedIncrement(&w->blanks);
            SetEvent(w->event);
        }
        return 0;
    }

    // Null if the monitor's vertical blank can't be waited for (then the window is paced by deadlines instead).
    static VBlankWaiter* Create(HMONITOR monitor)
    {
        HMODULE gdi = GetModuleHandle(L"gdi32.dll");
        auto openFn = reinterpret_cast<OpenAdapterFromHdcFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"));
        auto waitFn = reinterpret_cast<WaitForVerticalBlankEventFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));
        auto closeFn = reinterpret_cast<CloseAdapterFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        MONITORINFOEX info = {};
        info.cbSize = sizeof(info);
        if (!openFn || !waitFn || !closeFn || !GetMonitorInfo(monitor, &info)) return nullptr;
        HDC dc = CreateDC(nullptr, info.szDevice, nullptr, nullptr);
        if (!dc) return nullptr;
        D3DKMT_OPENADAPTERFROMHDC_ open = {};
        open.hDc = dc;
        LONG status = openFn(&open);
        DeleteDC(dc);
        if (status != 0) return nullptr;
        VBlankWaiter* w = new VBlankWaiter();
        w->wait = { open.hAdapter, 0, open.VidPnSourceId };
        w->waitFn = waitFn;
        w->closeFn = closeFn;
        w->event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        w->thread = CreateThread(nullptr, 0, Run, w, 0, nullptr);
        if (w->thread) SetThreadPriority(w->thread, THREAD_PRIORITY_TIME_CRITICAL);
        return w;
    }

    ~VBlankWaiter()
    {
        stop = 1;
        if (thread) { WaitForSingleObject(thread, 200); CloseHandle(thread); }
        if (event) CloseHandle(event);
        D3DKMT_CLOSEADAPTER_ close = { wait.hAdapter };
        if (closeFn) closeFn(&close);
    }
};

// ---------------------------------------------------------------------------------------------------------------- DXGI
// Frames reach the screen through DXGI flip-model swap chains, one per monitor, like the original Direct3D 11 build:
// OpenGL draws into an offscreen framebuffer, which is copied (flipped upright) into a Direct3D texture shared through
// WGL_NV_DX_interop2; Direct3D copies that to the swap chain and presents it. Each swap chain's frame latency waitable
// object signals when that monitor is ready for the next frame, so every monitor runs at its own refresh rate, and
// the driver and compositor handle full-screen presentation as they do for any DXGI program. Presenting OpenGL
// windows directly with SwapBuffers left the presentation mode up to the driver (two windows from one context, mixed
// refresh rates), and monitors could stall. Without the interop extension, that direct path is still the fallback.
typedef HANDLE(WINAPI* PFNWGLDXOPENDEVICENVPROC_)(void*);
typedef BOOL(WINAPI* PFNWGLDXCLOSEDEVICENVPROC_)(HANDLE);
typedef HANDLE(WINAPI* PFNWGLDXREGISTEROBJECTNVPROC_)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* PFNWGLDXUNREGISTEROBJECTNVPROC_)(HANDLE, HANDLE);
typedef BOOL(WINAPI* PFNWGLDXLOCKOBJECTSNVPROC_)(HANDLE, GLint, HANDLE*);
static PFNWGLDXOPENDEVICENVPROC_ g_wglDXOpenDevice = nullptr;
static PFNWGLDXCLOSEDEVICENVPROC_ g_wglDXCloseDevice = nullptr;
static PFNWGLDXREGISTEROBJECTNVPROC_ g_wglDXRegisterObject = nullptr;
static PFNWGLDXUNREGISTEROBJECTNVPROC_ g_wglDXUnregisterObject = nullptr;
static PFNWGLDXLOCKOBJECTSNVPROC_ g_wglDXLockObjects = nullptr, g_wglDXUnlockObjects = nullptr;
static const GLenum kWglAccessWriteDiscard = 0x0002;
static ID3D11Device* g_d3d = nullptr;
static ID3D11DeviceContext* g_d3dContext = nullptr;
static IDXGIFactory2* g_dxgiFactory = nullptr;
static HANDLE g_interopDevice = nullptr;
static bool g_allowTearing = false;
static HWND g_glWindow = nullptr; // hidden window that owns the GL context's pixel format (DXGI path)
static HDC g_glDc = nullptr;

struct RenderWindowState
{
    HWND hwnd;
    HDC dc;
    RECT bounds;     // Desktop area simulated and shown by this window.
    SIZE pixelSize;  // Drawing size; smaller than bounds in the Control Panel preview, which shows a scaled-down screen.
    double refreshHz;
    bool vsyncWindow; // V-Sync without a vblank event: this window's buffer swap waits for the vertical blank
    VBlankWaiter* vblank; // fallback path with V-Sync: signals at this monitor's vertical blank
    LONGLONG nextFrameCounter;
    // DXGI path
    IDXGISwapChain1* swapChain = nullptr;
    HANDLE frameWaitable = nullptr;     // signals when the swap chain can take the next frame (V-Sync pacing)
    ID3D11Texture2D* shared = nullptr;  // OpenGL draws into this (through the interop), D3D copies it to the swap chain
    HANDLE sharedInterop = nullptr;
    GLuint sharedTexture = 0, outFbo = 0, renderFbo = 0, colourRb = 0, depthRb = 0;
    // The event the loop waits for before drawing this window with V-Sync (frame latency waitable or vblank), or null.
    HANDLE PaceEvent() const { return g_isWallpaper && vblank ? vblank->event : frameWaitable ? frameWaitable : vblank ? vblank->event : nullptr; }
    unsigned blanks = 0; // live wallpaper: vertical blanks seen, for drawing every n-th one
    // Frame-rate counter for this window, and diagnostics for the log.
    bool paused = false;  // not drawn (live wallpaper: a window covers this monitor, or everything is paused)
    int picture = 0;      // background picture slot (live wallpaper: this monitor's own wallpaper)
    int fps = 0, framesThisSecond = 0;
    DWORD fpsTick = 0;
    long frames = 0;
    double drawMsTotal = 0, drawMsMax = 0;
    LONGLONG lastDraw = 0;     // frame pacing diagnostics: intervals between this window's frames
    int late = 0, doubled = 0; // intervals over 1.5x / under 0.5x the expected one
};
static std::vector<RenderWindowState*> g_renderWindows;
static LARGE_INTEGER g_qpcFrequency = {};

static bool UseVSync()
{
    return g_settings.vsync && g_settings.frameRate == 0;
}

// Frames per second for a window when not paced by V-Sync; 0 means unlimited.
static double TargetFrameRate(const RenderWindowState* state)
{
    if (g_settings.frameRate > 0) return g_settings.frameRate;
    // Live wallpaper: the windows are all children of the desktop, which the compositor paces at the primary monitor's
    // rate, so each one keeps its own monitor's rate (divided by the wallpaper's frame-rate option) by deadline.
    if (g_isWallpaper) return state->refreshHz / (std::max)(1u, g_presentInterval); // only without a vblank waiter
    if (UseVSync()) return state->vsyncWindow ? 0.0 : state->refreshHz;
    return state->refreshHz * kFrameRateModeMultipliers[g_settings.frameRateMode];
}

static bool SetGLPixelFormat(HDC dc)
{
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int format = ChoosePixelFormat(dc, &pfd);
    return format != 0 && SetPixelFormat(dc, format, &pfd);
}

// D3D11 device, DXGI factory and the GL/D3D interop device; false (and nothing kept) if any part is missing.
static bool InitDxgiInterop()
{
    g_wglDXOpenDevice = reinterpret_cast<PFNWGLDXOPENDEVICENVPROC_>(wglGetProcAddress("wglDXOpenDeviceNV"));
    g_wglDXCloseDevice = reinterpret_cast<PFNWGLDXCLOSEDEVICENVPROC_>(wglGetProcAddress("wglDXCloseDeviceNV"));
    g_wglDXRegisterObject = reinterpret_cast<PFNWGLDXREGISTEROBJECTNVPROC_>(wglGetProcAddress("wglDXRegisterObjectNV"));
    g_wglDXUnregisterObject = reinterpret_cast<PFNWGLDXUNREGISTEROBJECTNVPROC_>(wglGetProcAddress("wglDXUnregisterObjectNV"));
    g_wglDXLockObjects = reinterpret_cast<PFNWGLDXLOCKOBJECTSNVPROC_>(wglGetProcAddress("wglDXLockObjectsNV"));
    g_wglDXUnlockObjects = reinterpret_cast<PFNWGLDXLOCKOBJECTSNVPROC_>(wglGetProcAddress("wglDXUnlockObjectsNV"));
    if (!g_wglDXOpenDevice || !g_wglDXCloseDevice || !g_wglDXRegisterObject || !g_wglDXUnregisterObject || !g_wglDXLockObjects || !g_wglDXUnlockObjects)
    {
        Log("no WGL_NV_DX_interop2: presenting with OpenGL directly");
        return false;
    }
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) return false;
    // The D3D device must be on the GPU OpenGL renders with: the adapter whose name matches GL_RENDERER, else the first.
    std::string renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
        if (!chosen && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && renderer.find(name) != std::string::npos) { chosen = adapter; continue; }
        adapter->Release();
    }
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr, &g_d3dContext);
    if (chosen) chosen->Release();
    if (FAILED(hr)) { factory->Release(); Log("D3D11CreateDevice failed (0x%08lx): presenting with OpenGL directly", hr); return false; }
    g_interopDevice = g_wglDXOpenDevice(g_d3d);
    if (!g_interopDevice)
    {
        g_d3dContext->Release(); g_d3d->Release(); factory->Release();
        g_d3d = nullptr; g_d3dContext = nullptr;
        Log("wglDXOpenDeviceNV failed: presenting with OpenGL directly");
        return false;
    }
    g_dxgiFactory = factory;
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5))))
    {
        BOOL tearing = FALSE;
        g_allowTearing = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)) && tearing;
        factory5->Release();
    }
    Log("presenting through DXGI flip-model swap chains (tearing %s without V-Sync)", g_allowTearing ? "allowed" : "not supported");
    return true;
}

// The OpenGL 4.3 core context, created on the first window: a legacy context is needed first to find
// wglCreateContextAttribsARB.
static bool CreateGLContext(HDC dc)
{
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy || !wglMakeCurrent(dc, legacy))
    {
        g_glError = "no OpenGL support for this window (wglCreateContext failed)";
        if (legacy) wglDeleteContext(legacy);
        return false;
    }
    auto createAttribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC_>(wglGetProcAddress("wglCreateContextAttribsARB"));
    g_wglSwapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC_>(wglGetProcAddress("wglSwapIntervalEXT"));
    HGLRC core = nullptr;
    if (createAttribs)
    {
        const int attribs[] = { 0x2091 /*MAJOR*/, 4, 0x2092 /*MINOR*/, 3, 0x9126 /*PROFILE_MASK*/, 0x0001 /*CORE*/, 0 };
        core = createAttribs(dc, nullptr, attribs);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    if (!core || !wglMakeCurrent(dc, core))
    {
        g_glError = "the graphics driver has no OpenGL 4.3 core profile";
        if (core) wglDeleteContext(core);
        return false;
    }
    if (!g_wglSwapInterval)
        g_wglSwapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC_>(wglGetProcAddress("wglSwapIntervalEXT"));
    g_glContext = core;
    g_engine = new ShoalEngine();
    std::string error;
    if (!g_engine->init(&error))
    {
        g_glError = error;
        return false;
    }
    return true;
}

// The GL context for the DXGI path lives on a hidden window, so it is never switched between monitor windows.
static bool CreateHiddenGLContext(HINSTANCE instance)
{
    WNDCLASS windowClass = {};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = DefWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"ShoalGLWindow";
    RegisterClass(&windowClass);
    g_glWindow = CreateWindowEx(0, L"ShoalGLWindow", L"Shoal GL", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
    g_glDc = g_glWindow ? GetDC(g_glWindow) : nullptr;
    if (!g_glDc || !SetGLPixelFormat(g_glDc)) { g_glError = "no OpenGL pixel format with depth and stencil"; return false; }
    return CreateGLContext(g_glDc);
}

static bool UsingDxgi() { return g_interopDevice != nullptr; }

static void ReleaseDxgiWindow(RenderWindowState* state)
{
    if (state->sharedInterop) g_wglDXUnregisterObject(g_interopDevice, state->sharedInterop);
    if (state->sharedTexture) glDeleteTextures(1, &state->sharedTexture);
    if (state->outFbo) glDeleteFramebuffers(1, &state->outFbo);
    if (state->renderFbo) glDeleteFramebuffers(1, &state->renderFbo);
    GLuint rbs[] = { state->colourRb, state->depthRb };
    glDeleteRenderbuffers(2, rbs);
    if (state->shared) state->shared->Release();
    if (state->frameWaitable) CloseHandle(state->frameWaitable);
    if (state->swapChain) state->swapChain->Release();
    state->sharedInterop = nullptr; state->sharedTexture = state->outFbo = state->renderFbo = state->colourRb = state->depthRb = 0;
    state->shared = nullptr; state->frameWaitable = nullptr; state->swapChain = nullptr;
}

// Swap chain, shared texture and framebuffers for one window. The GL context must be current.
static bool CreateDxgiWindow(RenderWindowState* state)
{
    const UINT w = (UINT)state->pixelSize.cx, h = (UINT)state->pixelSize.cy;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    HRESULT hr = g_dxgiFactory->CreateSwapChainForHwnd(g_d3d, state->hwnd, &desc, nullptr, nullptr, &state->swapChain);
    if (FAILED(hr)) { Log("CreateSwapChainForHwnd failed (0x%08lx)", hr); return false; }
    g_dxgiFactory->MakeWindowAssociation(state->hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGISwapChain2* chain2 = nullptr;
    if (SUCCEEDED(state->swapChain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&chain2))))
    {
        chain2->SetMaximumFrameLatency(1);
        state->frameWaitable = chain2->GetFrameLatencyWaitableObject();
        chain2->Release();
    }
    D3D11_TEXTURE2D_DESC tex = {};
    tex.Width = w;
    tex.Height = h;
    tex.MipLevels = 1;
    tex.ArraySize = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_DEFAULT;
    tex.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d->CreateTexture2D(&tex, nullptr, &state->shared))) { Log("CreateTexture2D failed"); return false; }
    glGenTextures(1, &state->sharedTexture);
    state->sharedInterop = g_wglDXRegisterObject(g_interopDevice, state->shared, state->sharedTexture, GL_TEXTURE_2D, kWglAccessWriteDiscard);
    if (!state->sharedInterop) { Log("wglDXRegisterObjectNV failed"); return false; }
    // Output framebuffer: the shared texture (attached while locked).
    glGenFramebuffers(1, &state->outFbo);
    g_wglDXLockObjects(g_interopDevice, 1, &state->sharedInterop);
    glBindFramebuffer(GL_FRAMEBUFFER, state->outFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, state->sharedTexture, 0);
    GLenum outStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    g_wglDXUnlockObjects(g_interopDevice, 1, &state->sharedInterop);
    // Render framebuffer: colour plus depth/stencil (the engine's 3D depth and shadow pass need both).
    glGenFramebuffers(1, &state->renderFbo);
    glGenRenderbuffers(1, &state->colourRb);
    glGenRenderbuffers(1, &state->depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, state->colourRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)w, (GLsizei)h);
    glBindRenderbuffer(GL_RENDERBUFFER, state->depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
    glBindFramebuffer(GL_FRAMEBUFFER, state->renderFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, state->colourRb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, state->depthRb);
    GLenum renderStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (outStatus != GL_FRAMEBUFFER_COMPLETE || renderStatus != GL_FRAMEBUFFER_COMPLETE)
    {
        Log("framebuffers incomplete (0x%x, 0x%x)", outStatus, renderStatus);
        return false;
    }
    return true;
}

static void DrawWindow(RenderWindowState* state)
{
    if (!g_engine) return;
    if (!UsingDxgi())
    {
        if (!wglMakeCurrent(state->dc, g_glContext)) return;
        if (g_wglSwapInterval) g_wglSwapInterval(state->vsyncWindow ? 1 : 0);
    }
    ShoalView view;
    view.bounds = { (float)state->bounds.left, (float)state->bounds.top, (float)state->bounds.right, (float)state->bounds.bottom };
    view.pixelWidth = state->pixelSize.cx;
    view.pixelHeight = state->pixelSize.cy;
    DWORD tick = GetTickCount();
    if (tick - state->fpsTick >= 1000) { state->fps = state->framesThisSecond; state->framesThisSecond = 0; state->fpsTick = tick; }
    view.fps = state->fps; // this monitor's own frame rate (the engine steps once for all windows)
    view.picture = state->picture;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (state->lastDraw)
    {
        const double expected = 1000.0 * (g_isWallpaper ? (std::max)(1u, g_presentInterval) : 1u) / state->refreshHz;
        const double interval = (t0.QuadPart - state->lastDraw) * 1000.0 / g_qpcFrequency.QuadPart;
        if (interval > expected * 1.5) ++state->late;
        if (interval < expected * 0.5) ++state->doubled;
    }
    state->lastDraw = t0.QuadPart;
    if (UsingDxgi())
    {
        // Draw offscreen, copy upright into the shared texture, then present it through the swap chain.
        const GLint w = state->pixelSize.cx, h = state->pixelSize.cy;
        glBindFramebuffer(GL_FRAMEBUFFER, state->renderFbo);
        g_engine->draw(view);
        g_wglDXLockObjects(g_interopDevice, 1, &state->sharedInterop);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, state->renderFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state->outFbo);
        glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST); // GL rows are bottom-up, D3D top-down
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        g_wglDXUnlockObjects(g_interopDevice, 1, &state->sharedInterop);
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(state->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
        {
            g_d3dContext->CopyResource(backBuffer, state->shared);
            backBuffer->Release();
        }
        const bool vsync = UseVSync();
        // The wallpaper is drawn just after its monitor's blank and composed by DWM, so it doesn't wait in Present.
        state->swapChain->Present(g_isWallpaper ? (state->vblank ? 0 : 1) : vsync ? 1 : 0,
            !vsync && !g_isWallpaper && g_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
    else
    {
        g_engine->draw(view);
        SwapBuffers(state->dc);
    }
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpcFrequency.QuadPart;
    state->drawMsTotal += ms;
    state->drawMsMax = (std::max)(state->drawMsMax, ms);
    ++state->framesThisSecond;
    ++state->frames;
}

static void DestroyWindowState(HWND hwnd)
{
    if (RenderWindowState* state = reinterpret_cast<RenderWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA)))
    {
        g_renderWindows.erase(std::remove(g_renderWindows.begin(), g_renderWindows.end(), state), g_renderWindows.end());
        if (g_glContext) wglMakeCurrent(UsingDxgi() ? g_glDc : state->dc, g_glContext);
        if (UsingDxgi()) ReleaseDxgiWindow(state);
        if (g_renderWindows.empty() && g_glContext)
        {
            // The engine's GL objects go with the context; free them while it is still current.
            delete g_engine;
            g_engine = nullptr;
            if (g_interopDevice) { g_wglDXCloseDevice(g_interopDevice); g_interopDevice = nullptr; }
            if (g_d3dContext) { g_d3dContext->Release(); g_d3dContext = nullptr; }
            if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
            if (g_dxgiFactory) { g_dxgiFactory->Release(); g_dxgiFactory = nullptr; }
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(g_glContext);
            g_glContext = nullptr;
            if (g_glWindow) { ReleaseDC(g_glWindow, g_glDc); DestroyWindow(g_glWindow); g_glWindow = nullptr; g_glDc = nullptr; }
        }
        ReleaseDC(hwnd, state->dc);
        delete state->vblank;
        delete state;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;

        case WM_ERASEBKGND:
            return TRUE;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (!g_isPreview && !g_isWallpaper && GetTickCount() >= g_inputReadyTick) PostQuitMessage(0);
            return 0;

        case WM_SETCURSOR:
            if (!g_isPreview && !g_isWallpaper && LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(nullptr);
                return TRUE;
            }
            return DefWindowProc(hwnd, message, wParam, lParam);

        case WM_MOUSEMOVE:
        {
            POINT cursorPosition = {};
            if (!g_isPreview && !g_isWallpaper && GetTickCount() >= g_inputReadyTick && GetCursorPos(&cursorPosition) &&
                (std::abs(cursorPosition.x - g_initialCursorPosition.x) > 4 || std::abs(cursorPosition.y - g_initialCursorPosition.y) > 4))
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
static bool CreateMonitorWindow(HINSTANCE instance, RECT monitorRect, HWND parent = nullptr, RECT windowRect = {})
{
    if (!parent) windowRect = monitorRect;
    int width = windowRect.right - windowRect.left;
    int height = windowRect.bottom - windowRect.top;
    HWND hwnd = CreateWindowEx(parent ? 0 : (WS_EX_TOPMOST | WS_EX_TOOLWINDOW), L"ShoalScreenSaverWindow", L"Shoal",
        (parent ? WS_CHILD : WS_POPUP) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, windowRect.left, windowRect.top, width, height,
        parent, nullptr, instance, nullptr);
    if (!hwnd) return false;

    RenderWindowState* state = new RenderWindowState();
    state->hwnd = hwnd;
    state->dc = GetDC(hwnd);
    state->bounds = monitorRect;
    state->pixelSize = { width, height };
    state->refreshHz = 60.0;
    if (!parent || g_isWallpaper)
    {
        HMONITOR monitor = MonitorFromRect(&monitorRect, MONITOR_DEFAULTTONEAREST);
        state->refreshHz = GetRefreshRateForMonitor(monitor);
    }
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    g_renderWindows.push_back(state);

    // The first window sets up OpenGL (on a hidden window) and, if the interop extension is there, Direct3D/DXGI.
    bool ok = true;
    if (!g_glContext)
    {
        ok = CreateHiddenGLContext(instance);
        if (ok && !InitDxgiInterop())
        {
            // Fallback: present OpenGL directly; the context moves to the monitor windows' own DCs.
            ok = SetGLPixelFormat(state->dc) && wglMakeCurrent(state->dc, g_glContext);
            if (!ok) g_glError = "no OpenGL pixel format with depth and stencil";
        }
    }
    else if (!UsingDxgi())
    {
        ok = SetGLPixelFormat(state->dc);
        if (!ok) g_glError = "no OpenGL pixel format with depth and stencil";
    }
    if (ok && UsingDxgi())
    {
        wglMakeCurrent(g_glDc, g_glContext);
        ok = CreateDxgiWindow(state);
        if (!ok) g_glError = "couldn't set up the DXGI swap chain for OpenGL";
    }
    if (!ok)
    {
        DestroyWindow(hwnd);
        return false;
    }
    // V-Sync pacing. DXGI: each swap chain's frame latency waitable object (that monitor's refresh rate). Fallback: a
    // vertical blank waiter per monitor, else the first window's buffer swap waits for its blank.
    // The live wallpaper always draws on its own monitor's vertical blank: the compositor paces the whole desktop tree
    // at the primary monitor's rate, and a timer alone drifts against a monitor's real refresh (stutter).
    if ((UseVSync() && !parent && !UsingDxgi()) || g_isWallpaper)
        state->vblank = VBlankWaiter::Create(MonitorFromRect(&monitorRect, MONITOR_DEFAULTTONEAREST));
    state->vsyncWindow = UseVSync() && !UsingDxgi() && !state->vblank && g_renderWindows.size() == 1;
    Log("window %zu: %ldx%ld at %ld,%ld, %.3f Hz%s", g_renderWindows.size(), width, height, monitorRect.left, monitorRect.top,
        state->refreshHz, !UseVSync() ? "" : UsingDxgi() ? ", V-Sync through its swap chain" :
        state->vblank ? ", V-Sync on its own vertical blank" : state->vsyncWindow ? ", V-Sync through the buffer swap" : "");

    ShowWindow(hwnd, SW_SHOW);
    if (!parent) SetWindowPos(hwnd, HWND_TOPMOST, monitorRect.left, monitorRect.top, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    return true;
}

static ShoalSettings EngineSettings()
{
    ShoalSettings s;
    s.speed = g_settings.speedPixelsPerSecond;
    s.speedVariability = g_settings.speedVariability;
    s.fishCoverage = g_settings.fishCoverage;
    s.boidCount = g_boidCountOverridden ? g_settings.boidCount : 0;
    s.randomness = g_settings.randomness;
    s.sizeTenths = g_settings.sizeTenths;
    s.sizeVariation = g_settings.sizeVariation;
    s.sizeBell = g_settings.sizeBell;
    s.tailPercent = g_settings.tailPercent;
    s.tailShade = g_settings.tailShade;
    s.depthEffect = g_settings.depthEffect;
    s.rockCount = g_settings.rockCount;
    s.restrictToMonitor = g_settings.restrictToMonitor;
    s.colourSort = g_settings.colourSort;
    s.colourAvoid = g_settings.colourAvoid;
    s.colourCount = g_settings.colourCount;
    for (int i = 0; i < 10; ++i)
        s.colours[i] = (GetRValue(g_settings.colours[i]) << 16) | (GetGValue(g_settings.colours[i]) << 8) | GetBValue(g_settings.colours[i]);
    s.showFps = g_settings.showFps;
    s.speciesMask = g_settings.speciesMask ? g_settings.speciesMask : 0x3FF;
    s.speciesMode = g_settings.speciesMode;
    s.picture = g_settings.desktopBackground;
    return s;
}

// The whole virtual desktop, taken before the screensaver windows cover it; it becomes the engine's background picture
// and each monitor's window shows its own part of it.
struct DesktopCapture { int width = 0, height = 0; ShoalRect area; std::vector<uint8_t> rgba; };
static DesktopCapture g_desktopCapture;

static void CaptureDesktop()
{
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (memory && bitmap && bits)
    {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, w, h, screen, x, y, SRCCOPY))
        {
            GdiFlush();
            std::vector<uint8_t> rgba((size_t)w * h * 4);
            const uint8_t* bgra = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < (size_t)w * h; ++i)
            {
                rgba[i * 4] = bgra[i * 4 + 2];
                rgba[i * 4 + 1] = bgra[i * 4 + 1];
                rgba[i * 4 + 2] = bgra[i * 4];
                rgba[i * 4 + 3] = 255;
            }
            g_desktopCapture.width = w;
            g_desktopCapture.height = h;
            g_desktopCapture.area = { (float)x, (float)y, (float)(x + w), (float)(y + h) };
            g_desktopCapture.rgba = std::move(rgba);
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

// A window's pace event fired: draw now? The live wallpaper's frame-rate option draws on every n-th blank.
static bool DueOnBlank(RenderWindowState* state)
{
    if (!g_isWallpaper || g_presentInterval <= 1) return true;
    return ++state->blanks % g_presentInterval == 0;
}

// One simulation step with the real elapsed time, then the given windows draw.
static void RunFrame(const std::vector<RenderWindowState*>& ready)
{
    if (!g_engine || ready.empty() || !wglMakeCurrent(UsingDxgi() ? g_glDc : ready[0]->dc, g_glContext)) return;
    g_engine->step(g_engine->clockDelta());
    for (RenderWindowState* state : ready) DrawWindow(state);
}

// With V-Sync every window draws each loop and the first window's swap waits for its vertical blank. Without it,
// each window keeps its own deadline from its monitor's refresh rate and the Frame Rate setting; the loop sleeps on
// a high-resolution timer (or window messages) until the next one is due.
static int RunFrameLoop()
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) { timeBeginPeriod(1); timer = CreateWaitableTimer(nullptr, TRUE, nullptr); }
    const DWORD start = GetTickCount();
    DWORD lastReport = start;
    long frames = 0;
    std::vector<RenderWindowState*> ready, waitOwners;
    std::vector<HANDLE> waits;
    MSG message = {};
    for (;;)
    {
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                if (timer) CloseHandle(timer);
                double seconds = (GetTickCount() - start) / 1000.0;
                Log("%ld steps in %.1f s (%.1f per second)", frames, seconds, seconds > 0 ? frames / seconds : 0.0);

                return static_cast<int>(message.wParam);
            }
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        if (g_runSeconds > 0 && GetTickCount() - start >= (DWORD)g_runSeconds * 1000) { PostQuitMessage(0); continue; }
        if (g_log && GetTickCount() - lastReport >= 2000)
        {
            // Every 2 s: each window's frames, draw+swap time and vertical blanks seen (for the log).
            double interval = (GetTickCount() - lastReport) / 1000.0;
            lastReport = GetTickCount();
            for (size_t i = 0; i < g_renderWindows.size(); ++i)
            {
                RenderWindowState* w = g_renderWindows[i];
                Log("t=%5.1f window %zu: %.1f fps, draw+present avg %.2f ms max %.2f ms, late %d, doubled %d", (lastReport - start) / 1000.0,
                    i + 1, w->frames / interval, w->frames ? w->drawMsTotal / w->frames : 0.0, w->drawMsMax, w->late, w->doubled);
                w->frames = 0; w->drawMsTotal = 0; w->drawMsMax = 0; w->late = 0; w->doubled = 0;
            }
        }
        if (g_loopHook) g_loopHook();
        if (g_renderWindows.empty()) { WaitMessage(); continue; }

        ready.clear();
        waits.clear();
        waitOwners.clear();
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        LONGLONG earliest = LLONG_MAX;
        for (RenderWindowState* state : g_renderWindows)
        {
            if (state->paused) continue;
            HANDLE pace = UseVSync() || g_isWallpaper ? state->PaceEvent() : nullptr;
            if (pace)
            {
                if (WaitForSingleObject(pace, 0) == WAIT_OBJECT_0) { if (DueOnBlank(state)) ready.push_back(state); }
                else { waits.push_back(pace); waitOwners.push_back(state); }
                continue;
            }
            double fps = TargetFrameRate(state);
            if (fps <= 0.0) { ready.push_back(state); continue; }
            LONGLONG period = static_cast<LONGLONG>(g_qpcFrequency.QuadPart / fps);
            if (now.QuadPart >= state->nextFrameCounter)
            {
                state->nextFrameCounter += period;
                if (now.QuadPart - state->nextFrameCounter > period) state->nextFrameCounter = now.QuadPart + period;
                ready.push_back(state);
            }
            else
                earliest = (std::min)(earliest, state->nextFrameCounter);
        }
        if (!ready.empty())
        {
            RunFrame(ready);
            ++frames;
            continue;
        }
        if (earliest != LLONG_MAX)
        {
            double remainingMs = (earliest - now.QuadPart) * 1000.0 / g_qpcFrequency.QuadPart;
            if (remainingMs <= 1.0) { YieldProcessor(); continue; }
            LARGE_INTEGER due = {};
            due.QuadPart = -static_cast<LONGLONG>((remainingMs - 0.5) * 10000.0); // wake a little early, then spin
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            waits.push_back(timer);
            waitOwners.push_back(nullptr);
        }
        // Everything paused (live wallpaper): look again a few times a second.
        DWORD result = MsgWaitForMultipleObjects((DWORD)waits.size(), waits.empty() ? nullptr : waits.data(), FALSE, waits.empty() ? 250 : 100, QS_ALLINPUT);
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + waits.size() && waitOwners[result - WAIT_OBJECT_0])
        {
            // A monitor's vertical blank: draw it, and any other window that became due meanwhile.
            size_t signaled = result - WAIT_OBJECT_0;
            if (DueOnBlank(waitOwners[signaled])) ready.push_back(waitOwners[signaled]);
            for (size_t i = 0; i < waitOwners.size(); ++i)
                if (i != signaled && waitOwners[i] && WaitForSingleObject(waits[i], 0) == WAIT_OBJECT_0 && DueOnBlank(waitOwners[i]))
                    ready.push_back(waitOwners[i]);
            if (!ready.empty())
            {
                RunFrame(ready);
                ++frames;
            }
        }
    }
}

static void ReportGpuUnavailable()
{
    for (RenderWindowState* state : std::vector<RenderWindowState*>(g_renderWindows)) DestroyWindow(state->hwnd);
    MSG message;
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {} // closing the windows posts WM_QUIT
    Log("error: %s", g_glError.c_str());
    std::wstring text = L"Shoal needs a graphics card with OpenGL 4.3.\n\n" + std::wstring(g_glError.begin(), g_glError.end());
    MessageBox(nullptr, text.c_str(), L"Shoal", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

static int RunPreviewWindow(HINSTANCE instance, HWND parent)
{
    if (!parent || !IsWindow(parent) || !RegisterRendererWindowClass(instance)) return 1;
    RECT clientRect = {};
    GetClientRect(parent, &clientRect);
    if (clientRect.right <= clientRect.left || clientRect.bottom <= clientRect.top) return 1;

    // Simulate an area kPreviewScale times the preview's size and draw it scaled down, so the fish are big enough to
    // see in the small monitor image. No rocks or frame-rate counter here.
    const int kPreviewScale = 3;
    g_isPreview = true;
    g_settings.showFps = false;
    g_settings.rockCount = 0;
    RECT screenRect = { 0, 0, clientRect.right * kPreviewScale, clientRect.bottom * kPreviewScale };
    if (!CreateMonitorWindow(instance, screenRect, parent, clientRect)) return 1; // stays blank, no message box
    ShoalSettings s = EngineSettings();
    s.picture = false;
    g_engine->setSettings(s);
    g_engine->setMonitors({ { 0.0f, 0.0f, (float)screenRect.right, (float)screenRect.bottom } }, (float)ReferenceScreenHeight());
    return RunFrameLoop();
}

// "/species:1,3,9" -> bit mask (1-based numbers).
static void ParseSpeciesOverride(PWSTR commandLine, unsigned* mask)
{
    const wchar_t* value = FindOption(commandLine, L"species:");
    if (!value) return;
    unsigned m = 0;
    while (*value && *value != L' ')
    {
        wchar_t* end = nullptr;
        long n = wcstol(value, &end, 10);
        if (end == value) { ++value; continue; }
        if (n >= 1 && n <= kShoalSpeciesCount) m |= 1u << (n - 1);
        value = end;
    }
    if (m) *mask = m;
}


// ---------------------------------------------------------------------------------------------------------------- live wallpaper
// "Shoal.scr /wallpaper": the fish behind the desktop icons, on every monitor, like the Plasma wallpaper.
//
// The desktop layer: Explorer's Progman window draws the desktop with DirectComposition (WS_EX_NOREDIRECTIONBITMAP since
// Windows 11 24H2); its wallpaper WorkerW (created by the undocumented 0x052C message) is a child of Progman below the
// icon view (SHELLDLL_DefView). Before 24H2 it is a top-level WorkerW right after the one that holds the icons. Child
// windows of that WorkerW are shown behind the icons only if they present through DXGI (GDI drawing never appears),
// which the DXGI path above does. See tools/desktop_probe.cpp and README.md.
//
// Each monitor gets a child window (its view of the world) and its own wallpaper picture; the water is the monitor's
// work area (without the taskbar). Desktop icons are hard obstacles, merged into blocks; windows and the mouse pointer
// are soft ones. A monitor stops drawing while a window covers it, and everything stops while a full-screen program
// runs or the session is locked. A tray icon has the options; the fish settings are the screensaver's.

static const wchar_t* kWallpaperKey = L"Software\\Shoal\\Wallpaper";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"Shoal Wallpaper";
static const UINT kTrayMessage = WM_APP + 1;
static const UINT kTrayId = 1;
enum : UINT { kMenuSettings = 1, kMenuPause, kMenuPicture, kMenuIcons, kMenuWindows, kMenuMouse, kMenuPauseCovered, kMenuAutostart,
              kMenuQuit, kMenuRate1 = 11, kMenuRate2, kMenuRate3, kMenuRate4 };

struct WallpaperOptions
{
    bool picture = true;      // the desktop's wallpaper picture under the water (else the water colour)
    bool icons = true, windows = true, mouse = true;
    bool pauseCovered = true; // stop drawing a monitor while a window covers it
    int divisor = 1;          // 1 = every refresh, 2 = every other, ...
    int margin = 12;          // extra keep-out distance around icons, pixels
};
static WallpaperOptions g_wp;
static bool g_wpPaused = false; // from the tray menu
static HWND g_wpHost = nullptr; // message window: tray icon, Explorer restarts, display and session changes
static HWND g_wpLayer = nullptr;
static UINT g_taskbarCreated = 0;
static HANDLE g_settingsChanged = nullptr;
static HKEY g_settingsKey = nullptr;
static bool g_sessionLocked = false;
static int g_obstacleGen = 0;
static std::vector<ShoalObstacle> g_iconBlocks, g_windowBlocks;
static DWORD g_lastIcons = 0, g_lastWindows = 0;

static void LoadWallpaperOptions()
{
    HKEY key = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, kWallpaperKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    g_wp.picture = ReadSetting(key, L"ShowWallpaper", g_wp.picture, 0, 1) != 0;
    g_wp.icons = ReadSetting(key, L"AvoidIcons", g_wp.icons, 0, 1) != 0;
    g_wp.windows = ReadSetting(key, L"AvoidWindows", g_wp.windows, 0, 1) != 0;
    g_wp.mouse = ReadSetting(key, L"AvoidMouse", g_wp.mouse, 0, 1) != 0;
    g_wp.pauseCovered = ReadSetting(key, L"PauseWhenCovered", g_wp.pauseCovered, 0, 1) != 0;
    g_wp.divisor = ReadSetting(key, L"FrameRateDivisor", g_wp.divisor, 1, 4);
    g_wp.margin = ReadSetting(key, L"AvoidMargin", g_wp.margin, 0, 100);
    RegCloseKey(key);
}

static void SaveWallpaperOptions()
{
    HKEY key = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, kWallpaperKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    WriteSetting(key, L"ShowWallpaper", g_wp.picture);
    WriteSetting(key, L"AvoidIcons", g_wp.icons);
    WriteSetting(key, L"AvoidWindows", g_wp.windows);
    WriteSetting(key, L"AvoidMouse", g_wp.mouse);
    WriteSetting(key, L"PauseWhenCovered", g_wp.pauseCovered);
    WriteSetting(key, L"FrameRateDivisor", g_wp.divisor);
    WriteSetting(key, L"AvoidMargin", g_wp.margin);
    RegCloseKey(key);
}

static std::wstring ExePath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileName(nullptr, path, MAX_PATH);
    return path;
}

static bool AutostartEnabled()
{
    wchar_t value[MAX_PATH * 2] = {};
    DWORD size = sizeof(value) - sizeof(wchar_t);
    return RegGetValue(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

static void SetAutostart(bool on)
{
    HKEY key = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on)
    {
        std::wstring command = L"\"" + ExePath() + L"\" /wallpaper";
        RegSetValueEx(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    }
    else
        RegDeleteValue(key, kRunValue);
    RegCloseKey(key);
}

// ---- the desktop layer

static HWND FindWallpaperLayer()
{
    HWND progman = FindWindow(L"Progman", nullptr);
    if (!progman) return nullptr;
    DWORD_PTR result = 0;
    SendMessageTimeout(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
    SendMessageTimeout(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
    if (FindWindowEx(progman, nullptr, L"SHELLDLL_DefView", nullptr))
        return FindWindowEx(progman, nullptr, L"WorkerW", nullptr); // 24H2+
    HWND iconsHost = nullptr;
    EnumWindows([](HWND top, LPARAM out) -> BOOL {
        if (FindWindowEx(top, nullptr, L"SHELLDLL_DefView", nullptr)) { *reinterpret_cast<HWND*>(out) = top; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&iconsHost));
    return iconsHost ? FindWindowEx(nullptr, iconsHost, L"WorkerW", nullptr) : nullptr;
}

static HWND DesktopListView()
{
    HWND progman = FindWindow(L"Progman", nullptr);
    HWND defView = progman ? FindWindowEx(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (!defView)
    {
        EnumWindows([](HWND top, LPARAM out) -> BOOL {
            if (HWND v = FindWindowEx(top, nullptr, L"SHELLDLL_DefView", nullptr)) { *reinterpret_cast<HWND*>(out) = v; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&defView));
    }
    return defView ? FindWindowEx(defView, nullptr, L"SysListView32", nullptr) : nullptr;
}

// Moves the monitor windows into the (new) layer, e.g. after Explorer restarted.
static bool AttachToLayer()
{
    HWND layer = FindWallpaperLayer();
    if (!layer) return false;
    g_wpLayer = layer;
    for (RenderWindowState* state : g_renderWindows)
    {
        SetParent(state->hwnd, layer);
        POINT topLeft = { state->bounds.left, state->bounds.top };
        ScreenToClient(layer, &topLeft);
        SetWindowPos(state->hwnd, nullptr, topLeft.x, topLeft.y, state->pixelSize.cx, state->pixelSize.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    return true;
}

// ---- wallpaper pictures (one per monitor, or one spanning them)

// Decodes an image file with WIC to RGBA8, shrunk (never enlarged) so it still covers coverW x coverH.
static bool DecodePicture(const wchar_t* path, int coverW, int coverH, std::vector<uint8_t>& rgba, int& w, int& h)
{
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    bool ok = false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT iw = 0, ih = 0;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&iw, &ih)) && iw && ih)
    {
        double scale = (std::max)((double)coverW / iw, (double)coverH / ih);
        IWICBitmapSource* source = frame;
        UINT sw = iw, sh = ih;
        if (scale < 1.0 && SUCCEEDED(factory->CreateBitmapScaler(&scaler)))
        {
            sw = (std::max)(1u, (UINT)std::ceil(iw * scale));
            sh = (std::max)(1u, (UINT)std::ceil(ih * scale));
            if (SUCCEEDED(scaler->Initialize(frame, sw, sh, WICBitmapInterpolationModeHighQualityCubic))) source = scaler;
            else { sw = iw; sh = ih; }
        }
        if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        {
            rgba.resize((size_t)sw * sh * 4);
            ok = SUCCEEDED(converter->CopyPixels(nullptr, sw * 4, (UINT)rgba.size(), rgba.data()));
            w = (int)sw;
            h = (int)sh;
        }
    }
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return ok;
}

// The current wallpaper file of each monitor window (or empty), from IDesktopWallpaper; with a slideshow these follow
// the pictures on screen.
static std::vector<std::wstring> g_wallpaperPaths;
static DWORD g_lastWallpaperCheck = 0;
static std::vector<std::wstring> CurrentWallpaperPaths(DESKTOP_WALLPAPER_POSITION* position)
{
    std::vector<std::wstring> paths(g_renderWindows.size());
    IDesktopWallpaper* wallpaper = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper)))) return paths;
    if (position) wallpaper->GetPosition(position);
    UINT count = 0;
    wallpaper->GetMonitorDevicePathCount(&count);
    for (UINT m = 0; m < count; ++m)
    {
        LPWSTR id = nullptr;
        RECT r = {};
        if (FAILED(wallpaper->GetMonitorDevicePathAt(m, &id)) || !id) continue;
        if (SUCCEEDED(wallpaper->GetMonitorRECT(id, &r)))
            for (size_t i = 0; i < g_renderWindows.size(); ++i)
                if (r.left == g_renderWindows[i]->bounds.left && r.top == g_renderWindows[i]->bounds.top)
                {
                    LPWSTR file = nullptr;
                    if (SUCCEEDED(wallpaper->GetWallpaper(id, &file)) && file) { paths[i] = file; CoTaskMemFree(file); }
                }
        CoTaskMemFree(id);
    }
    wallpaper->Release();
    return paths;
}

// Gives each monitor window its wallpaper picture (engine slot = window index).
static void LoadWallpaperPictures()
{
    if (!g_engine) return;
    DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
    g_wallpaperPaths = CurrentWallpaperPaths(&position);
    g_lastWallpaperCheck = GetTickCount();
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    for (size_t i = 0; i < g_renderWindows.size(); ++i)
    {
        RenderWindowState* state = g_renderWindows[i];
        std::wstring path = g_wallpaperPaths[i];
        if (path.empty())
        {
            wchar_t current[MAX_PATH] = {};
            SystemParametersInfo(SPI_GETDESKWALLPAPER, MAX_PATH, current, 0);
            path = current;
        }
        const bool span = position == DWPOS_SPAN;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (path.empty() || !DecodePicture(path.c_str(), span ? vw : state->pixelSize.cx, span ? vh : state->pixelSize.cy, rgba, w, h))
        {
            Log("monitor %zu: no wallpaper picture (%ls)", i + 1, path.c_str());
            g_engine->setPicture(nullptr, 0, 0, nullptr, (int)i);
            continue;
        }
        ShoalRect area = { (float)vx, (float)vy, (float)(vx + vw), (float)(vy + vh) };
        g_engine->setPicture(rgba.data(), w, h, span ? &area : nullptr, (int)i);
        Log("monitor %zu: wallpaper %ls (%dx%d%s)", i + 1, path.c_str(), w, h, span ? ", spanned" : "");
    }
}

// ---- obstacles

static ShoalObstacle RectObstacle(float left, float top, float right, float bottom, bool soft)
{
    ShoalObstacle o = {};
    const float hx = (right - left) * 0.5f, hy = (bottom - top) * 0.5f;
    o.x = left + hx;
    o.y = top + hy;
    o.radius = std::sqrt(hx * hx + hy * hy) * (soft ? -1.0f : 1.0f);
    o.pointCount = 4;
    const float pts[8] = { -hx, -hy, hx, -hy, hx, hy, -hx, hy };
    std::copy(pts, pts + 8, o.points);
    return o;
}

// Desktop icon cells in screen coordinates, through the shell's folder view of the desktop (works across processes).
static std::vector<RECT> DesktopIconRects()
{
    std::vector<RECT> out;
    HWND listView = DesktopListView();
    if (!listView || !IsWindowVisible(listView)) return out; // "Show desktop icons" is off
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return out;
    VARIANT location, empty;
    VariantInit(&location);
    VariantInit(&empty);
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    IDispatch* dispatch = nullptr;
    IServiceProvider* services = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* view = nullptr;
    IFolderView* folder = nullptr;
    IEnumIDList* items = nullptr;
    if (SUCCEEDED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &dispatch)) && dispatch &&
        SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&services))) &&
        SUCCEEDED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folder))))
    {
        POINT spacing = {};
        folder->GetSpacing(&spacing);
        if (SUCCEEDED(folder->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items))))
        {
            PITEMID_CHILD item = nullptr;
            while (items->Next(1, &item, nullptr) == S_OK)
            {
                POINT pt = {};
                if (SUCCEEDED(folder->GetItemPosition(item, &pt)))
                {
                    RECT r = { pt.x, pt.y, pt.x + spacing.x, pt.y + spacing.y };
                    MapWindowPoints(listView, nullptr, reinterpret_cast<POINT*>(&r), 2);
                    out.push_back(r);
                }
                CoTaskMemFree(item);
            }
        }
    }
    if (items) items->Release();
    if (folder) folder->Release();
    if (view) view->Release();
    if (browser) browser->Release();
    if (services) services->Release();
    if (dispatch) dispatch->Release();
    windows->Release();
    return out;
}

// Icons as a few rectangular blocks: on a 6 px grid, each icon grows by the keep-out margin plus a fish length,
// then shrinks back by the fish length (a morphological closing), so gaps narrower than about two fish lengths are
// filled and fish can't get stuck between icons. Like the Plasma wallpaper's obstacle field.
static std::vector<ShoalObstacle> IconBlocks(const std::vector<RECT>& icons, float fishLength)
{
    std::vector<ShoalObstacle> out;
    if (icons.empty()) return out;
    const int cell = 6;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int gw = GetSystemMetrics(SM_CXVIRTUALSCREEN) / cell + 1, gh = GetSystemMetrics(SM_CYVIRTUALSCREEN) / cell + 1;
    const int close = (std::max)(1, (int)std::lround(fishLength / cell));
    const int grow = close + (int)std::lround(g_wp.margin / (double)cell);
    std::vector<uint8_t> grid((size_t)gw * gh, 0);
    for (const RECT& r : icons)
    {
        int x0 = (std::max)(0, (int)((r.left - vx) / cell) - grow), x1 = (std::min)(gw - 1, (int)((r.right - vx) / cell) + grow);
        int y0 = (std::max)(0, (int)((r.top - vy) / cell) - grow), y1 = (std::min)(gh - 1, (int)((r.bottom - vy) / cell) + grow);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) grid[(size_t)y * gw + x] = 1;
    }
    // Erode by `close` cells: a cell stays if every cell within `close` of it is set (2D prefix sums).
    std::vector<int> sum((size_t)(gw + 1) * (gh + 1), 0);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x)
            sum[(size_t)(y + 1) * (gw + 1) + x + 1] = grid[(size_t)y * gw + x] + sum[(size_t)y * (gw + 1) + x + 1] +
                                                      sum[(size_t)(y + 1) * (gw + 1) + x] - sum[(size_t)y * (gw + 1) + x];
    auto count = [&](int x0, int y0, int x1, int y1) {
        x0 = (std::max)(0, x0); y0 = (std::max)(0, y0); x1 = (std::min)(gw - 1, x1); y1 = (std::min)(gh - 1, y1);
        return sum[(size_t)(y1 + 1) * (gw + 1) + x1 + 1] - sum[(size_t)y0 * (gw + 1) + x1 + 1] - sum[(size_t)(y1 + 1) * (gw + 1) + x0] +
               sum[(size_t)y0 * (gw + 1) + x0];
    };
    std::vector<uint8_t> kept((size_t)gw * gh, 0);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x)
        {
            int x0 = (std::max)(0, x - close), x1 = (std::min)(gw - 1, x + close), y0 = (std::max)(0, y - close), y1 = (std::min)(gh - 1, y + close);
            kept[(size_t)y * gw + x] = grid[(size_t)y * gw + x] && count(x0, y0, x1, y1) == (x1 - x0 + 1) * (y1 - y0 + 1);
        }
    // Rectangles: runs per row, merged downwards while the run is the same.
    struct Open { int x0, x1, y0; bool alive; };
    std::vector<Open> open;
    auto emit = [&](const Open& o, int yEnd) {
        out.push_back(RectObstacle((float)(vx + o.x0 * cell), (float)(vy + o.y0 * cell), (float)(vx + (o.x1 + 1) * cell), (float)(vy + yEnd * cell), false));
    };
    for (int y = 0; y <= gh; ++y)
    {
        std::vector<std::pair<int, int>> runs;
        for (int x = 0; y < gh && x < gw;)
        {
            if (!kept[(size_t)y * gw + x]) { ++x; continue; }
            int x0 = x;
            while (x < gw && kept[(size_t)y * gw + x]) ++x;
            runs.push_back({ x0, x - 1 });
        }
        std::vector<Open> next;
        for (Open& o : open)
        {
            auto it = std::find(runs.begin(), runs.end(), std::make_pair(o.x0, o.x1));
            if (it != runs.end()) { next.push_back(o); runs.erase(it); }
            else emit(o, y);
        }
        for (auto& r : runs) next.push_back({ r.first, r.second, y, true });
        open.swap(next);
    }
    return out;
}

static bool IsCloaked(HWND w)
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked;
}

// Visible application windows (soft obstacles) and which monitors are covered by one.
static void ScanWindows(std::vector<ShoalObstacle>& obstacles, std::vector<bool>& covered)
{
    struct Ctx { std::vector<RECT> rects; } ctx;
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(p);
        if (!IsWindowVisible(w) || IsIconic(w) || IsCloaked(w)) return TRUE;
        wchar_t cls[64] = {};
        GetClassName(w, cls, 64);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
            !wcscmp(cls, L"ShoalScreenSaverWindow") || !wcscmp(cls, L"ShoalWallpaperHost"))
            return TRUE;
        if (GetWindowLongPtr(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW && !(GetWindowLongPtr(w, GWL_STYLE) & WS_CAPTION)) return TRUE;
        RECT r = {};
        if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && !GetWindowRect(w, &r)) return TRUE;
        if (r.right - r.left < 8 || r.bottom - r.top < 8) return TRUE;
        c->rects.push_back(r);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    covered.assign(g_renderWindows.size(), false);
    for (const RECT& r : ctx.rects)
    {
        bool big = false;
        for (size_t i = 0; i < g_renderWindows.size(); ++i)
        {
            const RECT& m = g_renderWindows[i]->bounds;
            RECT overlap = {};
            if (!IntersectRect(&overlap, &r, &m)) continue;
            double share = (double)(overlap.right - overlap.left) * (overlap.bottom - overlap.top) / ((double)(m.right - m.left) * (m.bottom - m.top));
            if (share >= 0.95) covered[i] = true;
            if (share >= 0.85) big = true; // fish swim under very large windows rather than around them
        }
        if (g_wp.windows && !big) obstacles.push_back(RectObstacle((float)r.left, (float)r.top, (float)r.right, (float)r.bottom, true));
    }
}

static void ApplyWallpaperSettings()
{
    if (!g_engine) return;
    ShoalSettings s = EngineSettings();
    s.picture = g_wp.picture;
    s.rockCount = 0; // the desktop has icons and windows to swim around instead
    g_engine->setSettings(s);
    g_presentInterval = (UINT)g_wp.divisor;
}

// Called by the frame loop every turn: settings changes, obstacles, pausing.
static void WallpaperTick()
{
    const DWORD now = GetTickCount();
    if (g_settingsChanged && WaitForSingleObject(g_settingsChanged, 0) == WAIT_OBJECT_0)
    {
        // The settings dialog (or the tray menu) changed the registry: reload, and watch again.
        LoadSettings();
        g_settings.boidLength = BoidLengthForSize(g_settings.sizeTenths);
        LoadWallpaperOptions();
        ApplyWallpaperSettings();
        RegNotifyChangeKeyValue(g_settingsKey, TRUE, REG_NOTIFY_CHANGE_LAST_SET, g_settingsChanged, TRUE);
        g_lastIcons = g_lastWindows = 0;
        Log("settings reloaded");
    }
    if (now - g_lastWallpaperCheck >= 30000)
    {
        // A slideshow moves on without always telling: compare the files every 30 s.
        g_lastWallpaperCheck = now;
        if (CurrentWallpaperPaths(nullptr) != g_wallpaperPaths) LoadWallpaperPictures();
    }
    bool changed = false;
    const float fishLength = g_engine ? g_engine->boidLength() : 22.0f;
    if (now - g_lastIcons >= 1000)
    {
        g_lastIcons = now;
        std::vector<RECT> icons = g_wp.icons ? DesktopIconRects() : std::vector<RECT>();
        std::vector<ShoalObstacle> blocks = IconBlocks(icons, fishLength);
        if (blocks.size() != g_iconBlocks.size() || memcmp(blocks.data(), g_iconBlocks.data(), blocks.size() * sizeof(ShoalObstacle)))
        {
            Log("desktop icons: %zu, as %zu obstacle blocks", icons.size(), blocks.size());
            g_iconBlocks.swap(blocks);
            changed = true;
        }
    }
    if (now - g_lastWindows >= 250)
    {
        g_lastWindows = now;
        std::vector<ShoalObstacle> windows;
        std::vector<bool> covered;
        ScanWindows(windows, covered);
        if (windows.size() != g_windowBlocks.size() || memcmp(windows.data(), g_windowBlocks.data(), windows.size() * sizeof(ShoalObstacle)))
        {
            if (windows.size() != g_windowBlocks.size()) Log("windows to swim around: %zu", windows.size());
            g_windowBlocks.swap(windows);
            changed = true;
        }
        QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
        SHQueryUserNotificationState(&state);
        const bool fullScreenApp = state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE || state == QUNS_BUSY;
        for (size_t i = 0; i < g_renderWindows.size(); ++i)
        {
            bool paused = g_wpPaused || g_sessionLocked || fullScreenApp || (g_wp.pauseCovered && i < covered.size() && covered[i]);
            if (paused != g_renderWindows[i]->paused)
            {
                g_renderWindows[i]->paused = paused;
                Log("monitor %zu %s", i + 1, paused ? "paused" : "resumed");
            }
        }
    }
    if (changed && g_engine)
    {
        std::vector<ShoalObstacle> all = g_iconBlocks;
        all.insert(all.end(), g_windowBlocks.begin(), g_windowBlocks.end());
        g_engine->setObstacles(all, ++g_obstacleGen);
    }
    if (g_engine)
    {
        std::vector<ShoalObstacle> extra;
        POINT cursor = {};
        if (g_wp.mouse && GetCursorPos(&cursor))
        {
            // The pointer: a small soft hexagon (fish steer around it, one it lands on darts away).
            ShoalObstacle hex = {};
            hex.x = (float)cursor.x;
            hex.y = (float)cursor.y;
            hex.radius = -24.0f;
            hex.pointCount = 6;
            for (int p = 0; p < 6; ++p)
            {
                hex.points[p * 2] = std::cos(p * 1.0471976f) * 24.0f;
                hex.points[p * 2 + 1] = std::sin(p * 1.0471976f) * 24.0f;
            }
            extra.push_back(hex);
        }
        g_engine->setDynamicObstacles(extra);
    }
}

// ---- tray icon

// The tray icon: the classic fish from the species table, drawn into a 32x32 ARGB icon.
static HICON MakeFishIcon()
{
    const int size = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colour || !bits) return LoadIcon(nullptr, IDI_APPLICATION);
    DWORD* pixels = static_cast<DWORD*>(bits);
    std::memset(pixels, 0, size * size * 4);
    const std::vector<ShoalFishShape>& shapes = ShoalSpeciesShapes();
    if (!shapes.empty())
    {
        ShoalFishOutline o = ShoalFishOutlineFor(shapes[0], 30.0f, 1.2f, 0.6f, 1.0f);
        for (ShoalPoint& p : o.body) { p.x += 31.0f; p.y += 16.0f; }
        for (auto& fin : o.fins) for (ShoalPoint& p : fin) { p.x += 31.0f; p.y += 16.0f; }
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                float cx = x + 0.5f, cy = y + 0.5f;
                if (InsidePolygon(o.body.data(), o.body.size(), cx, cy) || InsidePolygon(o.fins[0], 3, cx, cy) || InsidePolygon(o.fins[1], 3, cx, cy))
                {
                    double along = (31.0 - cx) / 30.0;
                    DWORD rgb = ShadeFishColour(RGB(255, 205, 40), -0.45, along);
                    pixels[y * size + x] = 0xFF000000 | rgb;
                }
            }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info = { TRUE, 0, 0, mask, colour };
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(colour);
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

static void AddTrayIcon()
{
    static HICON icon = MakeFishIcon();
    NOTIFYICONDATA data = { sizeof(data) };
    data.hWnd = g_wpHost;
    data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon;
    wcscpy_s(data.szTip, L"Shoal live wallpaper");
    Shell_NotifyIcon(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &data);
}

static void ShowTrayMenu()
{
    HMENU rate = CreatePopupMenu();
    const wchar_t* rates[] = { L"Every refresh", L"Half", L"A third", L"A quarter" };
    for (int i = 0; i < 4; ++i) AppendMenu(rate, MF_STRING | (g_wp.divisor == i + 1 ? MF_CHECKED : 0), kMenuRate1 + i, rates[i]);
    HMENU menu = CreatePopupMenu();
    AppendMenu(menu, MF_STRING, kMenuSettings, L"Fish settings...");
    AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(menu, MF_STRING | (g_wpPaused ? MF_CHECKED : 0), kMenuPause, L"Pause");
    AppendMenu(menu, MF_STRING | (g_wp.picture ? MF_CHECKED : 0), kMenuPicture, L"Show my wallpaper under the water");
    AppendMenu(menu, MF_STRING | (g_wp.icons ? MF_CHECKED : 0), kMenuIcons, L"Swim around desktop icons");
    AppendMenu(menu, MF_STRING | (g_wp.windows ? MF_CHECKED : 0), kMenuWindows, L"Swim around windows");
    AppendMenu(menu, MF_STRING | (g_wp.mouse ? MF_CHECKED : 0), kMenuMouse, L"Keep away from the mouse pointer");
    AppendMenu(menu, MF_STRING | (g_wp.pauseCovered ? MF_CHECKED : 0), kMenuPauseCovered, L"Pause a screen while a window fills it");
    AppendMenu(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rate), L"Frame rate");
    AppendMenu(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(menu, MF_STRING, kMenuQuit, L"Quit");
    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(g_wpHost);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wpHost, nullptr);
    DestroyMenu(menu); // also destroys the submenu
}

static void OpenFishSettings()
{
    if (!LaunchSelf(L"/c")) Log("couldn't open the fish settings (error %lu)", GetLastError());
    else Log("opened the fish settings");
}

static LRESULT CALLBACK WallpaperHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == g_taskbarCreated && g_taskbarCreated)
    {
        // Explorer restarted: a new desktop layer and taskbar.
        Log("Explorer restarted: re-attaching");
        AddTrayIcon();
        if (!AttachToLayer()) Log("no desktop layer after the restart");
        return 0;
    }
    switch (message)
    {
        case kTrayMessage:
            if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) ShowTrayMenu();
            else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) OpenFishSettings();
            return 0;
        case WM_COMMAND:
        {
            UINT id = LOWORD(wParam);
            Log("tray menu: command %u", id);
            if (id == kMenuSettings) OpenFishSettings();
            else if (id == kMenuPause) { g_wpPaused = !g_wpPaused; g_lastWindows = 0; }
            else if (id == kMenuAutostart) SetAutostart(!AutostartEnabled());
            else if (id == kMenuQuit) PostQuitMessage(0);
            else
            {
                if (id == kMenuPicture) g_wp.picture = !g_wp.picture;
                if (id == kMenuIcons) g_wp.icons = !g_wp.icons;
                if (id == kMenuWindows) g_wp.windows = !g_wp.windows;
                if (id == kMenuMouse) g_wp.mouse = !g_wp.mouse;
                if (id == kMenuPauseCovered) g_wp.pauseCovered = !g_wp.pauseCovered;
                if (id >= kMenuRate1 && id <= kMenuRate4) g_wp.divisor = (int)(id - kMenuRate1 + 1);
                SaveWallpaperOptions(); // the registry watch applies it
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            // Monitors were added, removed or rearranged: start over with the new layout.
            Log("display layout changed: restarting");
            LaunchSelf(L"/wallpaper:restart");
            PostQuitMessage(0);
            return 0;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETDESKWALLPAPER) LoadWallpaperPictures(); // a new wallpaper (or the next slideshow picture)
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK) g_sessionLocked = true;
            if (wParam == WTS_SESSION_UNLOCK) g_sessionLocked = false;
            g_lastWindows = 0;
            return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

static int RunWallpaper(HINSTANCE instance, PWSTR commandLine)
{
    const bool quit = FindOption(commandLine, L"wallpaper:quit") != nullptr;
    const bool restart = FindOption(commandLine, L"wallpaper:restart") != nullptr;
    if (HWND running = FindWindow(L"ShoalWallpaperHost", nullptr))
    {
        if (quit || restart) SendMessageTimeout(running, WM_CLOSE, 0, 0, SMTO_NORMAL, 5000, nullptr);
        if (!restart) return 0; // already running (or asked to quit)
        for (int i = 0; i < 50 && FindWindow(L"ShoalWallpaperHost", nullptr); ++i) Sleep(100);
    }
    else if (quit)
        return 0;
    HANDLE mutex = CreateMutex(nullptr, TRUE, L"ShoalLiveWallpaper");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
    OpenLog(L"wallpaper.log");

    g_isWallpaper = true;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadWallpaperOptions();

    g_wpLayer = FindWallpaperLayer();
    if (!g_wpLayer)
    {
        Log("error: no desktop layer (Progman/WorkerW) found");
        MessageBox(nullptr, L"Shoal couldn't find the desktop to draw the live wallpaper on.", L"Shoal", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!RegisterRendererWindowClass(instance)) return 1;

    // One child window per monitor, in the layer's client coordinates; the water is each monitor's work area.
    std::vector<ShoalRect> water;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFO info = { sizeof(info) };
        if (GetMonitorInfo(monitor, &info))
        {
            g_monitorBounds.push_back(info.rcMonitor);
            reinterpret_cast<std::vector<ShoalRect>*>(p)->push_back(
                { (float)info.rcWork.left, (float)info.rcWork.top, (float)info.rcWork.right, (float)info.rcWork.bottom });
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&water));
    for (const RECT& m : g_monitorBounds)
    {
        RECT local = m;
        MapWindowPoints(nullptr, g_wpLayer, reinterpret_cast<POINT*>(&local), 2);
        if (!CreateMonitorWindow(instance, m, g_wpLayer, local))
        {
            ReportGpuUnavailable();
            return 1;
        }
    }
    for (size_t i = 0; i < g_renderWindows.size(); ++i) g_renderWindows[i]->picture = (int)i;
    Log("live wallpaper: %zu monitor window(s) in layer %p", g_renderWindows.size(), (void*)g_wpLayer);
    ApplyWallpaperSettings();
    g_engine->setMonitors(water, (float)ReferenceScreenHeight());
    LoadWallpaperPictures();

    // Message window: tray icon, Explorer restarts, display/session changes, quit requests (/wallpaper:quit).
    WNDCLASS host = {};
    host.lpfnWndProc = WallpaperHostProc;
    host.hInstance = instance;
    host.lpszClassName = L"ShoalWallpaperHost";
    RegisterClass(&host);
    g_wpHost = CreateWindowEx(0, L"ShoalWallpaperHost", L"Shoal wallpaper", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g_taskbarCreated = RegisterWindowMessage(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_wpHost, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    WTSRegisterSessionNotification(g_wpHost, NOTIFY_FOR_THIS_SESSION);
    AddTrayIcon();

    // Watch the settings (the fish settings dialog and the tray menu write them).
    g_settingsChanged = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    RegCreateKeyEx(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_NOTIFY | KEY_READ, nullptr, &g_settingsKey, nullptr);
    if (g_settingsKey) RegNotifyChangeKeyValue(g_settingsKey, TRUE, REG_NOTIFY_CHANGE_LAST_SET, g_settingsChanged, TRUE);

    g_loopHook = WallpaperTick;
    int exitCode = RunFrameLoop();

    // Quit: remove the tray icon and the windows, and give the desktop its normal wallpaper back.
    NOTIFYICONDATA data = { sizeof(data) };
    data.hWnd = g_wpHost;
    data.uID = kTrayId;
    Shell_NotifyIcon(NIM_DELETE, &data);
    WTSUnRegisterSessionNotification(g_wpHost);
    for (RenderWindowState* state : std::vector<RenderWindowState*>(g_renderWindows)) DestroyWindow(state->hwnd);
    DestroyWindow(g_wpHost);
    wchar_t wallpaper[MAX_PATH] = {};
    SystemParametersInfo(SPI_GETDESKWALLPAPER, MAX_PATH, wallpaper, 0);
    SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, wallpaper, SPIF_SENDCHANGE);
    if (g_settingsKey) RegCloseKey(g_settingsKey);
    if (g_settingsChanged) CloseHandle(g_settingsChanged);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    CoUninitialize();
    Log("live wallpaper stopped");
    if (g_log) std::fclose(g_log);
    return exitCode;
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
    if (FindOption(pCmdLine, L"wallpaper"))
    {
        return RunWallpaper(hInstance, pCmdLine); // opens its log once it knows it is the one running
    }
    OpenLog();

    HWND previewParent = nullptr;
    if (IsPreviewCommand(pCmdLine, &previewParent))
    {
        return RunPreviewWindow(hInstance, previewParent);
    }

    // Real pixels on scaled displays (the settings dialog above keeps the system's scaling).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    ParseIntOverride(pCmdLine, L"seconds:", 0, 100000, &g_runSeconds);
    ParseIntOverride(pCmdLine, L"speciesmode:", 0, 1, &g_settings.speciesMode);
    ParseSpeciesOverride(pCmdLine, &g_settings.speciesMask);
    struct { const wchar_t* name; bool* value; } flags[] = {
        { L"restrict:", &g_settings.restrictToMonitor }, { L"showfps:", &g_settings.showFps }, { L"vsync:", &g_settings.vsync },
        { L"coloursort:", &g_settings.colourSort }, { L"colouravoid:", &g_settings.colourAvoid }, { L"sizebell:", &g_settings.sizeBell },
        { L"depth:", &g_settings.depthEffect }, { L"desktop:", &g_settings.desktopBackground } };
    for (const auto& flag : flags)
    {
        int value = *flag.value ? 1 : 0;
        ParseIntOverride(pCmdLine, flag.name, 0, 1, &value);
        *flag.value = value != 0;
    }
    ParseIntOverride(pCmdLine, L"sizevar:", 100, 300, &g_settings.sizeVariation);

    std::vector<ShoalRect> monitors;
    for (const RECT& r : g_monitorBounds) monitors.push_back({ (float)r.left, (float)r.top, (float)r.right, (float)r.bottom });
    if (monitors.empty())
    {
        return 1;
    }

    // Copy the desktop before any window covers it; the first window then creates the GL context and the engine.
    if (g_settings.desktopBackground) CaptureDesktop();
    for (size_t i = 0; i < g_monitorBounds.size(); ++i)
    {
        if (!CreateMonitorWindow(hInstance, g_monitorBounds[i]))
        {
            ReportGpuUnavailable();
            return 1;
        }
    }
    Log("%zu monitor window(s), V-Sync %s, %zu species shapes", g_renderWindows.size(), UseVSync() ? "on" : "off", ShoalSpeciesShapes().size());
    g_engine->setSettings(EngineSettings());
    g_engine->setMonitors(monitors, (float)ReferenceScreenHeight());
    if (g_desktopCapture.width > 0)
    {
        ShoalRect area = g_desktopCapture.area;
        g_engine->setPicture(g_desktopCapture.rgba.data(), g_desktopCapture.width, g_desktopCapture.height, &area);
        g_desktopCapture = {};
    }

    GetCursorPos(&g_initialCursorPosition);
    g_inputReadyTick = GetTickCount() + 1000;
    // Hide the cursor until the screensaver closes; WM_SETCURSOR also clears it over the windows.
    while (ShowCursor(FALSE) >= 0) {}
    SetCursor(nullptr);

    int exitCode = RunFrameLoop();
    while (ShowCursor(TRUE) < 0) {}
    if (g_log) std::fclose(g_log);
    return exitCode;
}
