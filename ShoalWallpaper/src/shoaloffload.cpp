// Wallpaper glue around the shared engine, and the offload thread. See shoaloffload.h.
#include "shoaloffload.h"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

void ShoalInstallLogger()
{
    static std::once_flag once;
    std::call_once(once, [] { ShoalEngine::setLogger([](const std::string& s) { qInfo("Shoal: %s", s.c_str()); }); });
}

void ShoalRunFrame(ShoalEngine& engine, const ShoalFrameInput& in)
{
    engine.setSettings(in.s);
    // The simulation's one "monitor" is the water: the screen minus panels, so fish turn at the panel edge.
    // Fish sizes are relative to the whole wallpaper's height, so a new screen size rebuilds the shoal.
    const ShoalRect water = { (float)in.water.left(), (float)in.water.top(), (float)in.water.right(), (float)in.water.bottom() };
    engine.setMonitors({ water }, (float)in.world.height());
    engine.setObstacles(in.statics, in.staticsGen);
    engine.setDynamicObstacles(in.extra);
    engine.step(engine.clockDelta());
    ShoalView view;
    view.bounds = { 0, 0, (float)in.world.width(), (float)in.world.height() };
    view.pixelWidth = in.world.width();
    view.pixelHeight = in.world.height();
    view.fpsAtBottom = true; // above a bottom panel, clear of the desktop's top-right corner
    engine.draw(view);
    static const bool debug = qEnvironmentVariableIsSet("SHOAL_DEBUG");
    static thread_local int lastFps = -1;
    if (debug && engine.fps() != lastFps) { lastFps = engine.fps(); qInfo("Shoal: %d fps", lastFps); }
}

namespace {
// PCI address of a DRM node (/dev/dri/card1 or /dev/dri/renderD128).
QString PciOfDrmNode(const char* node)
{
    if (!node) return {};
    char path[256];
    std::snprintf(path, sizeof path, "/sys/class/drm/%s/device", std::strrchr(node, '/') ? std::strrchr(node, '/') + 1 : node);
    char real[512];
    if (!realpath(path, real)) return {};
    const char* slash = std::strrchr(real, '/');
    return QString::fromLatin1(slash ? slash + 1 : real);
}
} // namespace

// ------------------------------------------------------------------------------------------------ offload
std::unique_ptr<ShoalOffload> ShoalOffload::create(const QString& pciAddress, QString* error)
{
    std::unique_ptr<ShoalOffload> o(new ShoalOffload);
    o->m_thread = std::thread(&ShoalOffload::run, o.get(), pciAddress);
    std::unique_lock<std::mutex> lock(o->m_mutex);
    o->m_cv.wait(lock, [&] { return o->m_initDone; });
    if (o->m_initOk) return o;
    if (error) *error = o->m_error;
    lock.unlock();
    o->m_thread.join();
    o->m_stop = true; // the destructor then has nothing left to stop
    return nullptr;
}

ShoalOffload::~ShoalOffload()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    freeSlots();
}

void ShoalOffload::freeSlots()
{
    for (Slot& s : m_slots)
    {
        if (s.fence) { glClientWaitSync((GLsync)s.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000); glDeleteSync((GLsync)s.fence); }
        if (s.pbo) glDeleteBuffers(1, &s.pbo);
        s = Slot();
    }
    m_slotSize = QSize();
}

int ShoalOffload::fps() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fps;
}

bool ShoalOffload::present(unsigned texture, QSize size)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_jobDone)
        {
            m_jobDone = false;
            for (Slot& s : m_slots) if (s.state == Ready) s.state = Free; // superseded
            m_slots[m_jobSlot].state = Ready;
        }
    }
    for (Slot& s : m_slots)
        if (s.state == Uploading && glClientWaitSync((GLsync)s.fence, 0, 0) != GL_TIMEOUT_EXPIRED)
        {
            glDeleteSync((GLsync)s.fence);
            s.fence = nullptr;
            s.state = Free;
        }
    if (size != m_slotSize) return false;
    for (Slot& s : m_slots)
    {
        if (s.state != Ready) continue;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s.pbo);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        s.state = Uploading;
        return true;
    }
    return false;
}

void ShoalOffload::submit(const ShoalFrameInput& in)
{
    if (in.world.isEmpty()) return;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_jobPending) return; // still drawing the previous frame: skip this one
    if (in.world != m_slotSize)
    {
        // New size: wait out the worker (not busy, see above) and recreate the buffers.
        if (m_jobDone) { m_jobDone = false; }
        lock.unlock();
        freeSlots();
        const GLsizeiptr bytes = GLsizeiptr(in.world.width()) * in.world.height() * 4;
        for (Slot& s : m_slots)
        {
            glGenBuffers(1, &s.pbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s.pbo);
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            glBufferStorage(GL_PIXEL_UNPACK_BUFFER, bytes, nullptr, flags);
            s.ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, bytes, flags);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        m_slotSize = in.world;
        lock.lock();
        if (std::any_of(std::begin(m_slots), std::end(m_slots), [](const Slot& s) { return !s.ptr; }))
        {
            qWarning("Shoal: couldn't map the frame buffers on the desktop GPU");
            return;
        }
    }
    for (int i = 0; i < 3; ++i)
    {
        if (m_slots[i].state != Free) continue;
        m_slots[i].state = Writing;
        m_jobSlot = i;
        m_jobPtr = m_slots[i].ptr;
        m_job = in;
        m_jobPending = true;
        lock.unlock();
        m_cv.notify_all();
        return;
    }
}

void ShoalOffload::run(QString pci)
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    auto finishInit = [&](bool ok, const QString& error)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_initDone = true;
        m_initOk = ok;
        m_error = error;
        m_cv.notify_all();
        return ok;
    };
    auto setup = [&]() -> bool
    {
        auto queryDevices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        auto queryString = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
        if (!queryDevices || !queryString) return finishInit(false, QStringLiteral("EGL can't list GPUs (no EGL_EXT_device_enumeration)"));
        EGLDeviceEXT devices[16];
        EGLint count = 0;
        if (!queryDevices(16, devices, &count)) count = 0;
        for (EGLint i = 0; i < count && context == EGL_NO_CONTEXT; ++i)
        {
            if (PciOfDrmNode(queryString(devices[i], EGL_DRM_DEVICE_FILE_EXT)) != pci) continue;
            EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
            // Never eglTerminate: the display is shared by every wallpaper (screen) that renders on this GPU.
            if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) continue;
            const char* ext = eglQueryString(d, EGL_EXTENSIONS);
            if (!ext || !strstr(ext, "EGL_KHR_surfaceless_context") || !eglBindAPI(EGL_OPENGL_API)) continue;
            const EGLint attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
            EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
            if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c))
            {
                if (c != EGL_NO_CONTEXT) eglDestroyContext(d, c);
                continue;
            }
            display = d;
            context = c;
        }
        if (context == EGL_NO_CONTEXT) return finishInit(false, QStringLiteral("no usable OpenGL 4.3 context on GPU %1").arg(pci));
        m_renderer = QString::fromLatin1((const char*)glGetString(GL_RENDERER));
        return true;
    };

    if (setup())
    {
        ShoalEngine engine;
        std::string error;
        GLuint fbo = 0, colour = 0, depth = 0, pack = 0;
        QSize fboSize;
        size_t packBytes = 0;
        void* packPtr = nullptr;
        if (finishInit(engine.init(&error), Q(error)))
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;)
            {
                m_cv.wait(lock, [&] { return m_stop || m_jobPending; });
                if (m_stop) break;
                ShoalFrameInput in = std::move(m_job);
                void* target = m_jobPtr;
                lock.unlock();

                if (in.world != fboSize)
                {
                    if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &colour); glDeleteRenderbuffers(1, &depth); }
                    fboSize = in.world;
                    glGenRenderbuffers(1, &colour);
                    glBindRenderbuffer(GL_RENDERBUFFER, colour);
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fboSize.width(), fboSize.height());
                    glGenRenderbuffers(1, &depth);
                    glBindRenderbuffer(GL_RENDERBUFFER, depth);
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fboSize.width(), fboSize.height());
                    glGenFramebuffers(1, &fbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colour);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                ShoalRunFrame(engine, in);
                const size_t bytes = size_t(fboSize.width()) * fboSize.height() * 4;
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                {
                    // DMA into our own system-memory buffer, sleep until it's done (NVIDIA's own waits spin), then one
                    // CPU copy into the desktop GPU's buffer. Reading straight into that buffer with glReadPixels
                    // costs about twice the CPU time.
                    if (packBytes != bytes)
                    {
                        if (pack) glDeleteBuffers(1, &pack);
                        glGenBuffers(1, &pack);
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
                        const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                        glBufferStorage(GL_PIXEL_PACK_BUFFER, bytes, nullptr, flags | GL_CLIENT_STORAGE_BIT);
                        packPtr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, flags);
                        packBytes = bytes;
                    }
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
                    glReadPixels(0, 0, fboSize.width(), fboSize.height(), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    GLsync done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    glFlush();
                    while (glClientWaitSync(done, 0, 0) == GL_TIMEOUT_EXPIRED) std::this_thread::sleep_for(std::chrono::microseconds(300));
                    glDeleteSync(done);
                    if (packPtr) memcpy(target, packPtr, bytes);
                }

                lock.lock();
                m_jobPending = false;
                m_jobDone = true;
                m_fps = engine.fps();
            }
            lock.unlock();
        }
        if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &colour); glDeleteRenderbuffers(1, &depth); }
        if (pack) glDeleteBuffers(1, &pack);
        // engine's GL objects go with the context
    }
    if (context != EGL_NO_CONTEXT)
    {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
    }
    eglReleaseThread();
}
