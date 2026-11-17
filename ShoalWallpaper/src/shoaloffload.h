// Wallpaper glue around the shared engine (../../ShoalCore): the wallpaper's settings and per-frame input, one
// engine frame, and ShoalOffload, which runs the engine on another GPU.
#pragma once

#include "shoal_engine.h"
#include "shoal_linux.h"

#include <QColor>
#include <QRectF>
#include <QVariantList>
#include <QSize>
#include <QString>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The engine's settings plus the wallpaper's own.
struct WallpaperSettings : ShoalSettings
{
    WallpaperSettings() { rockCount = 0; }
    int frameDivisor = 1; // 1 = every vblank, 2 = every other one, ...
    bool avoidIcons = true, avoidWidgets = true, avoidMouse = true, avoidWindows = true;
    int avoidMargin = 12;     // extra keep-out distance around icons and widgets, logical px
    QString gpu;              // PCI address of the GPU to render on; empty = the dedicated GPU if there is one
};

// Everything one frame needs, in device pixels. Built on the scene-graph thread in synchronize().
struct ShoalFrameInput
{
    WallpaperSettings s;
    QSize world;                          // framebuffer size
    QRectF water;                         // simulation area (screen minus panels)
    std::vector<ShoalObstacle> statics;   // icons, widgets, windows (negative radius = soft)
    int staticsGen = 0;                   // changes whenever statics changes
    std::vector<ShoalObstacle> extra;     // per-frame obstacles (the mouse pointer)
};

// Feeds the input to the engine, steps it and draws the whole wallpaper into the bound framebuffer.
void ShoalRunFrame(ShoalEngine& engine, const ShoalFrameInput& in);
// Routes the engine's log lines to the journal (once per process).
void ShoalInstallLogger();

inline QString Q(const std::string& s) { return QString::fromStdString(s); }

// ------------------------------------------------------------------------------------------------ offload
// Renders on another GPU in a worker thread with its own surfaceless EGL context, and hands each finished frame to
// the scene-graph context: the other GPU copies the frame into system memory (DMA), the worker copies it into a
// persistently mapped pixel-unpack buffer of the desktop GPU, and the desktop GPU copies that into the wallpaper's
// texture. One frame of latency; the scene-graph thread never waits for the other GPU.
//
// The NVIDIA driver can't render into, or blit into, dma-bufs from another device (it draws into a private copy), so
// a zero-copy hand-over isn't possible in OpenGL; the CPU copy is about 1 ms per 1080p frame on the worker thread.
class ShoalOffload
{
public:
    // Starts the worker and waits for its context. Null (and *error) if the GPU can't be used.
    static std::unique_ptr<ShoalOffload> create(const QString& pciAddress, QString* error);
    ~ShoalOffload(); // scene-graph thread, its context current
    QString renderer() const { return m_renderer; }

    // Scene-graph thread, its context current. Copies the newest finished frame into `texture` (RGBA8, size
    // `size`); false if there was none.
    bool present(unsigned texture, QSize size);
    // Scene-graph thread. Starts the next frame unless the worker is still busy with the previous one.
    void submit(const ShoalFrameInput& in);
    int fps() const;

private:
    ShoalOffload() = default;
    void run(QString pci);
    void freeSlots();

    enum SlotState { Free, Writing, Ready, Uploading };
    struct Slot { unsigned pbo = 0; void* ptr = nullptr; void* fence = nullptr; SlotState state = Free; };
    Slot m_slots[3];
    QSize m_slotSize;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false, m_initDone = false, m_initOk = false;
    bool m_jobPending = false, m_jobDone = false; // a job is with the worker / the worker finished it
    int m_jobSlot = -1;
    void* m_jobPtr = nullptr;
    ShoalFrameInput m_job;
    QString m_renderer, m_error;
    int m_fps = 0;
};
