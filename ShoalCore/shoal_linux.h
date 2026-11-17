// Linux helpers shared by ShoalLinux and ShoalWallpaper: GPUs (sysfs), PRIME render offload for a standalone
// window, and Plasma's default wallpaper picture. Standard library only (plus EGL for ShoalCurrentContextGpu).
#pragma once

#include <string>
#include <vector>

struct ShoalGpu
{
    std::string id;     // PCI address, e.g. 0000:01:00.0
    std::string name;   // e.g. NVIDIA GeForce RTX 2060 Mobile
    std::string driver; // kernel driver: nvidia, amdgpu, i915, xe, nouveau, ...
    bool bootVga;       // the firmware's primary display GPU (an integrated GPU on most laptops)
};

// Display GPUs, dedicated ones first.
std::vector<ShoalGpu> ShoalListGpus();
// The GPU a setting means: itself if present, else (empty or unknown) the first non-primary GPU, else empty.
std::string ShoalResolveGpu(const std::string& setting);
// Name of a GPU for messages ("unknown" if not found).
std::string ShoalGpuName(const std::string& id);
// PCI address of the GPU behind the current thread's EGL context (empty if unknown or not EGL).
std::string ShoalCurrentContextGpu();

// For a standalone window (ShoalLinux): sets the PRIME render-offload environment so that the next OpenGL context
// is created on GPU `id` (a PCI address). Must run before the GL library is loaded (before SDL_Init). Nothing is set
// for the primary GPU. Returns a description for the log.
std::string ShoalUsePrimeGpu(const std::string& id);

// Plasma's default wallpaper for the current Look-and-Feel (the picture the Image wallpaper shows when none is
// chosen): the dark variant for dark colour schemes, then the file whose size best fits width x height.
// Empty if Plasma's wallpapers aren't installed.
std::string ShoalDefaultWallpaper(int width, int height);
