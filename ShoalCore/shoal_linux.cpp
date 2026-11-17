// Linux helpers, see shoal_linux.h. Ported from ShoalWallpaper's Qt code to the standard library.
#include "shoal_linux.h"

#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string Trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string ReadSys(const fs::path& p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return Trim(ss.str());
}

std::string Strip0x(std::string s) { return s.rfind("0x", 0) == 0 ? s.substr(2) : s; }

// PCI address of a DRM node (/dev/dri/card1 or /dev/dri/renderD128), from sysfs.
std::string PciOfDrmNode(const char* node)
{
    if (!node) return {};
    std::error_code ec;
    fs::path dev = fs::canonical(fs::path("/sys/class/drm") / fs::path(node).filename() / "device", ec);
    return ec ? std::string() : dev.filename().string();
}

// "NVIDIA GeForce RTX 2060 Mobile" from pci.ids ("TU106M [GeForce RTX 2060 Mobile]" under "NVIDIA Corporation").
std::string PciName(const std::string& vendor, const std::string& device)
{
    std::string vendorName, deviceName;
    for (const char* path : { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids" })
    {
        std::ifstream f(path);
        if (!f) continue;
        bool inVendor = false;
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] != '\t')
            {
                if (inVendor) break;
                inVendor = line.rfind(vendor + "  ", 0) == 0;
                if (inVendor) vendorName = line.substr(6);
                continue;
            }
            if (inVendor && line.size() > 7 && line[1] != '\t' && line.substr(1, 4) == device) { deviceName = line.substr(7); break; }
        }
        break;
    }
    size_t lb = deviceName.find('['), rb = deviceName.rfind(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) deviceName = deviceName.substr(lb + 1, rb - lb - 1);
    static const std::map<std::string, std::string> shortVendor = { { "10de", "NVIDIA" }, { "1002", "AMD" }, { "8086", "Intel" } };
    auto it = shortVendor.find(vendor);
    std::string v = it != shortVendor.end() ? it->second : vendorName;
    if (deviceName.empty()) deviceName = "GPU " + vendor + ":" + device;
    return v.empty() ? deviceName : v + " " + deviceName;
}

// Minimal INI reader: value of `key` in `[group]`.
std::string ReadIni(const fs::path& file, const std::string& group, const std::string& key)
{
    std::ifstream f(file);
    std::string line;
    bool in = false;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') { in = line == "[" + group + "]"; continue; }
        size_t eq = line.find('=');
        if (in && eq != std::string::npos && Trim(line.substr(0, eq)) == key) return Trim(line.substr(eq + 1));
    }
    return {};
}

std::string Env(const char* name, const std::string& fallback)
{
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

// XDG data directories, user first.
std::vector<fs::path> DataDirs()
{
    std::vector<fs::path> out;
    out.push_back(Env("XDG_DATA_HOME", Env("HOME", ".") + "/.local/share"));
    std::stringstream ss(Env("XDG_DATA_DIRS", "/usr/local/share:/usr/share"));
    std::string dir;
    while (std::getline(ss, dir, ':')) if (!dir.empty()) out.push_back(dir);
    return out;
}

fs::path Locate(const std::string& relative)
{
    std::error_code ec;
    for (const fs::path& d : DataDirs()) if (fs::exists(d / relative, ec)) return d / relative;
    return {};
}

// Dark colour scheme? Plasma's window background colour in kdeglobals ("r,g,b").
bool DarkScheme()
{
    std::string rgb = ReadIni(fs::path(Env("XDG_CONFIG_HOME", Env("HOME", ".") + "/.config")) / "kdeglobals", "Colors:Window", "BackgroundNormal");
    int r = 255, g = 255, b = 255;
    if (std::sscanf(rgb.c_str(), "%d,%d,%d", &r, &g, &b) != 3) return false;
    int hi = std::max({ r, g, b }), lo = std::min({ r, g, b });
    return (hi + lo) / 2 < 128; // HSL lightness, as QColor::lightness()
}

} // namespace

std::vector<ShoalGpu> ShoalListGpus()
{
    std::vector<ShoalGpu> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec))
    {
        const std::string card = entry.path().filename().string();
        if (card.rfind("card", 0) != 0 || card.find('-') != std::string::npos) continue; // connectors (card1-eDP-1)
        const fs::path dev = entry.path() / "device";
        std::error_code e2;
        const std::string pci = fs::canonical(dev, e2).filename().string();
        const std::string vendor = Strip0x(ReadSys(dev / "vendor")), device = Strip0x(ReadSys(dev / "device"));
        if (e2 || pci.empty() || vendor.empty() || ReadSys(dev / "class").rfind("0x03", 0) != 0) continue; // display controllers only
        if (std::any_of(out.begin(), out.end(), [&](const ShoalGpu& g) { return g.id == pci; })) continue;
        std::string driver = fs::canonical(dev / "driver", e2).filename().string();
        out.push_back({ pci, PciName(vendor, device), e2 ? std::string() : driver, ReadSys(dev / "boot_vga") == "1" });
    }
    std::sort(out.begin(), out.end(), [](const ShoalGpu& a, const ShoalGpu& b) { return a.bootVga != b.bootVga ? !a.bootVga : a.id < b.id; });
    return out;
}

std::string ShoalResolveGpu(const std::string& setting)
{
    const std::vector<ShoalGpu> gpus = ShoalListGpus();
    for (const ShoalGpu& g : gpus) if (g.id == setting) return g.id;
    for (const ShoalGpu& g : gpus) if (!g.bootVga) return g.id;
    return {};
}

std::string ShoalGpuName(const std::string& id)
{
    for (const ShoalGpu& g : ShoalListGpus()) if (g.id == id) return g.name;
    return id.empty() ? "unknown" : id;
}

std::string ShoalCurrentContextGpu()
{
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) return {};
    auto queryDisplay = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
    auto queryString = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLAttrib device = 0;
    if (!queryDisplay || !queryString || !queryDisplay(display, EGL_DEVICE_EXT, &device) || !device) return {};
    std::string pci = PciOfDrmNode(queryString((EGLDeviceEXT)device, EGL_DRM_DEVICE_FILE_EXT));
    return pci.empty() ? PciOfDrmNode(queryString((EGLDeviceEXT)device, EGL_DRM_RENDER_NODE_FILE_EXT)) : pci;
}

std::string ShoalUsePrimeGpu(const std::string& id)
{
    for (const ShoalGpu& g : ShoalListGpus())
    {
        if (g.id != id) continue;
        if (g.bootVga) return g.name + " (the primary GPU)";
        if (g.driver == "nvidia")
        {
            // NVIDIA's PRIME render offload, for both GLX (X11) and EGL (Wayland) through glvnd.
            setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
            setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
            std::error_code ec;
            for (const char* json : { "/usr/share/glvnd/egl_vendor.d/10_nvidia.json", "/etc/glvnd/egl_vendor.d/10_nvidia.json" })
                if (fs::exists(json, ec)) { setenv("__EGL_VENDOR_LIBRARY_FILENAMES", json, 1); break; }
            return g.name + " (NVIDIA PRIME render offload)";
        }
        // Mesa drivers pick the device from DRI_PRIME, e.g. pci-0000_03_00_0.
        std::string tag = "pci-" + g.id;
        std::replace(tag.begin(), tag.end(), ':', '_');
        std::replace(tag.begin(), tag.end(), '.', '_');
        setenv("DRI_PRIME", tag.c_str(), 1);
        return g.name + " (DRI_PRIME=" + tag + ")";
    }
    return "the default GPU";
}

std::string ShoalDefaultWallpaper(int width, int height)
{
    const fs::path config = Env("XDG_CONFIG_HOME", Env("HOME", ".") + "/.config");
    std::string lnf = ReadIni(config / "kdeglobals", "KDE", "LookAndFeelPackage");
    if (lnf.empty()) lnf = "org.kde.breeze.desktop";
    std::string image;
    fs::path defaults = Locate("plasma/look-and-feel/" + lnf + "/contents/defaults");
    if (!defaults.empty()) image = ReadIni(defaults, "Wallpaper", "Image");
    if (image.empty()) image = "Next";
    std::error_code ec;
    if (fs::path(image).is_absolute() && fs::is_regular_file(image, ec)) return image;
    fs::path dir = fs::path(image).is_absolute() ? fs::path(image) : Locate("wallpapers/" + image);
    if (dir.empty()) dir = Locate("wallpapers/Next");
    if (dir.empty()) return {};

    // Like the Image wallpaper: the dark variant for dark colour schemes, then the size closest to the screen's shape.
    fs::path images = dir / "contents/images";
    if (DarkScheme() && fs::is_directory(dir / "contents/images_dark", ec)) images = dir / "contents/images_dark";
    if (width <= 0 || height <= 0) { width = 1920; height = 1080; }
    const double want = double(width) / height;
    std::string best;
    double bestScore = 1e30;
    for (const auto& e : fs::directory_iterator(images, ec))
    {
        const std::string ext = e.path().extension().string();
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp" && ext != ".avif") continue;
        int w = 0, h = 0;
        if (std::sscanf(e.path().stem().string().c_str(), "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
        {
            if (best.empty()) best = e.path().string();
            continue;
        }
        // Aspect mismatch dominates; among equal shapes prefer the smallest image that still covers the screen.
        double score = std::fabs(std::log((double(w) / h) / want)) * 1e6 + (w >= width && h >= height ? w * double(h) * 1e-6 : 1e3);
        if (score < bestScore) { bestScore = score; best = e.path().string(); }
    }
    return best;
}
