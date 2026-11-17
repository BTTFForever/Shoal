// Probe for a Windows live-wallpaper host: finds the desktop layer behind the icons (Progman's WorkerW, created with
// the undocumented 0x052C message), logs the window structure, attaches a test window to it and animates it for a
// few seconds, then restores the normal wallpaper. Log: %LOCALAPPDATA%\Shoal\desktop_probe.log
//
// Layouts seen in the wild:
//   before Windows 11 24H2: top-level WorkerW (holds SHELLDLL_DefView, the icons) followed by a second top-level
//                           WorkerW that draws the wallpaper; a window parented to that second WorkerW sits behind
//                           the icons.
//   24H2 and later:         SHELLDLL_DefView and a WorkerW are both children of Progman; the WorkerW draws the
//                           wallpaper, below the icon view in Progman's child z-order.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cmath>
#include <cstdio>
#include <string>

static FILE* g_log = nullptr;
static void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    if (g_log) { std::vfprintf(g_log, format, args); std::fputc('\n', g_log); std::fflush(g_log); }
    va_end(args);
}

static std::string Describe(HWND w)
{
    if (!w) return "(null)";
    wchar_t cls[128] = {}, title[128] = {};
    GetClassName(w, cls, 128);
    GetWindowText(w, title, 128);
    RECT r = {};
    GetWindowRect(w, &r);
    char buf[512];
    std::snprintf(buf, sizeof buf, "%p class=%ls title=\"%ls\" rect=%ld,%ld %ldx%ld visible=%d", (void*)w, cls, title, r.left, r.top,
        r.right - r.left, r.bottom - r.top, IsWindowVisible(w) ? 1 : 0);
    return buf;
}

static void LogChildren(HWND parent, int depth)
{
    for (HWND c = FindWindowEx(parent, nullptr, nullptr, nullptr); c; c = FindWindowEx(parent, c, nullptr, nullptr))
    {
        Log("%*s- %s", depth * 2, "", Describe(c).c_str());
        if (depth < 2) LogChildren(c, depth + 1);
    }
}

// The window that should parent the wallpaper, or null.
static HWND FindWallpaperLayer(const char** layout)
{
    HWND progman = FindWindow(L"Progman", nullptr);
    if (!progman) return nullptr;
    // 24H2+: icons (SHELLDLL_DefView) are a child of Progman, and so is the wallpaper WorkerW.
    if (FindWindowEx(progman, nullptr, L"SHELLDLL_DefView", nullptr))
    {
        HWND worker = FindWindowEx(progman, nullptr, L"WorkerW", nullptr);
        *layout = "24H2+ (WorkerW child of Progman)";
        return worker;
    }
    // Older: the top-level WorkerW after the one that holds the icons.
    HWND iconsHost = nullptr;
    EnumWindows([](HWND top, LPARAM out) -> BOOL {
        if (FindWindowEx(top, nullptr, L"SHELLDLL_DefView", nullptr)) { *reinterpret_cast<HWND*>(out) = top; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&iconsHost));
    *layout = "classic (top-level WorkerW after the icons)";
    return iconsHost ? FindWindowEx(nullptr, iconsHost, L"WorkerW", nullptr) : nullptr;
}

static LRESULT CALLBACK ProbeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProc(hwnd, message, wParam, lParam);
}

// Usage: desktop_probe.exe [worker|progman|layered] [dxgi]
//   worker / progman / layered: how to attach the test window (default worker)
//   dxgi: present through a flip-model swap chain (as Shoal does) instead of drawing with GDI
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    std::wstring args = commandLine && *commandLine ? commandLine : L"worker";
    const bool dxgi = args.find(L"dxgi") != std::wstring::npos;
    std::wstring variant = args.find(L"progman") != std::wstring::npos ? L"progman" : args.find(L"layered") != std::wstring::npos ? L"layered" : L"worker";
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t path[MAX_PATH];
    GetEnvironmentVariable(L"LOCALAPPDATA", path, MAX_PATH);
    wcscat_s(path, L"\\Shoal");
    CreateDirectory(path, nullptr);
    wcscat_s(path, L"\\desktop_probe.log");
    _wfopen_s(&g_log, path, L"a");
    Log("==== variant %ls%ls", variant.c_str(), dxgi ? L" + dxgi" : L" + gdi");

    OSVERSIONINFOEXW version = { sizeof(version) };
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW*);
    if (auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandle(L"ntdll.dll"), "RtlGetVersion"))) rtl(&version);
    Log("Windows %lu.%lu build %lu", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);

    HWND progman = FindWindow(L"Progman", nullptr);
    Log("Progman: %s", Describe(progman).c_str());
    Log("before 0x052C, Progman children:");
    LogChildren(progman, 1);

    // Ask Explorer for the wallpaper layer. (0xD, 0x1) is what current versions answer to; (0, 0) for older ones.
    DWORD_PTR result = 0;
    SendMessageTimeout(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
    SendMessageTimeout(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);

    Log("after 0x052C, Progman children:");
    LogChildren(progman, 1);
    Log("top-level WorkerW windows:");
    for (HWND w = FindWindowEx(nullptr, nullptr, L"WorkerW", nullptr); w; w = FindWindowEx(nullptr, w, L"WorkerW", nullptr))
    {
        Log("  %s", Describe(w).c_str());
        LogChildren(w, 2);
    }

    const char* layout = "none";
    HWND layer = FindWallpaperLayer(&layout);
    Log("layout: %s", layout);
    Log("wallpaper layer: %s", Describe(layer).c_str());
    HWND defView = FindWindowEx(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    Log("ex styles: Progman 0x%08lx, WorkerW 0x%08lx, DefView 0x%08lx", (unsigned long)GetWindowLongPtr(progman, GWL_EXSTYLE),
        layer ? (unsigned long)GetWindowLongPtr(layer, GWL_EXSTYLE) : 0ul, defView ? (unsigned long)GetWindowLongPtr(defView, GWL_EXSTYLE) : 0ul);
    if (!layer) { Log("RESULT: no wallpaper layer found"); return 1; }

    // A child of the layer covering the whole virtual desktop, in the layer's client coordinates.
    WNDCLASS wc = {};
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ShoalDesktopProbe";
    RegisterClass(&wc);
    HWND parent = variant == L"worker" ? layer : progman;
    RECT parentRect = {};
    GetWindowRect(parent, &parentRect);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    DWORD exStyle = variant == L"layered" ? WS_EX_LAYERED : 0;
    HWND probe = CreateWindowEx(exStyle, L"ShoalDesktopProbe", L"Shoal probe", WS_CHILD | WS_VISIBLE, vx - parentRect.left, vy - parentRect.top,
        vw, vh, parent, nullptr, instance, nullptr);
    if (probe && exStyle & WS_EX_LAYERED) SetLayeredWindowAttributes(probe, 0, 255, LWA_ALPHA);
    // Child of Progman: just below the icon view (and so above the wallpaper WorkerW).
    if (probe && parent == progman && defView) SetWindowPos(probe, defView, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Log("probe window: %s (parent %s)", Describe(probe).c_str(), Describe(GetParent(probe)).c_str());
    if (!probe) { Log("RESULT: CreateWindowEx failed (%lu)", GetLastError()); return 1; }
    Log("Progman children with the probe:");
    LogChildren(progman, 1);

    // DXGI: a flip-model swap chain on the probe window, cleared to an animated colour each frame.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* chain = nullptr;
    if (dxgi)
    {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        IDXGIFactory2* factory = nullptr;
        if (SUCCEEDED(hr)) hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = vw; desc.Height = vh;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForHwnd(device, probe, &desc, nullptr, nullptr, &chain);
        Log("dxgi swap chain: 0x%08lx", hr);
        if (factory) factory->Release();
    }

    // Animate for 8 s: a moving gradient and a bouncing square (GDI), or an animated colour (DXGI).
    HDC dc = GetDC(probe);
    const DWORD start = GetTickCount();
    MSG msg;
    while (GetTickCount() - start < 8000)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        double t = (GetTickCount() - start) / 1000.0;
        if (chain)
        {
            ID3D11Texture2D* back = nullptr;
            ID3D11RenderTargetView* rtv = nullptr;
            chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
            device->CreateRenderTargetView(back, nullptr, &rtv);
            float colour[4] = { 0.1f, (float)(0.3 + 0.25 * std::sin(t * 3)), (float)(0.5 + 0.3 * std::cos(t * 2)), 1.0f };
            context->ClearRenderTargetView(rtv, colour);
            rtv->Release();
            back->Release();
            chain->Present(1, 0);
            continue;
        }
        for (int band = 0; band < 16; ++band)
        {
            RECT r = { band * vw / 16, 0, (band + 1) * vw / 16, vh };
            int g = (int)(60 + 40 * std::sin(t * 2 + band * 0.4));
            HBRUSH b = CreateSolidBrush(RGB(0, g, 90 + g / 2));
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        int x = (int)((std::sin(t * 1.3) * 0.5 + 0.5) * (vw - 200)), y = (int)((std::cos(t * 0.9) * 0.5 + 0.5) * (vh - 200));
        RECT sq = { x, y, x + 200, y + 200 };
        HBRUSH b = CreateSolidBrush(RGB(255, 205, 40));
        FillRect(dc, &sq, b);
        DeleteObject(b);
        Sleep(16);
    }
    ReleaseDC(probe, dc);
    if (chain) chain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(probe);

    // Give the desktop its normal wallpaper back (the layer otherwise keeps the last frame).
    wchar_t wallpaper[MAX_PATH] = {};
    SystemParametersInfo(SPI_GETDESKWALLPAPER, MAX_PATH, wallpaper, 0);
    SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, wallpaper, SPIF_SENDCHANGE);
    Log("restored wallpaper: %ls", wallpaper);
    Log("RESULT: variant %ls%ls ran 8 s on %s", variant.c_str(), dxgi ? L" + dxgi" : L"", layout);
    if (g_log) std::fclose(g_log);
    return 0;
}
