// Shoal live wallpaper. The world setup and GL code are ported from ShoalLinux/shoal.cpp (globals turned into per-instance
// state, since plasmashell runs one wallpaper per screen); the GLSL shaders are the ShoalLinux ones, unchanged.
#include "shoalitem.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QUrl>
#include <QFile>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QTextStream>
#include <QtMath>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace {

float Len(float x, float y) { return std::sqrt(x * x + y * y); }

ShoalObstacle RectObstacle(float x, float y, float w, float h)
{
    ShoalObstacle o = {};
    o.x = x + w * 0.5f;
    o.y = y + h * 0.5f;
    o.radius = Len(w, h) * 0.5f;
    o.pointCount = 4;
    const float hx = w * 0.5f, hy = h * 0.5f;
    const float pts[8] = { -hx, -hy, hx, -hy, hx, hy, -hx, hy };
    std::copy(pts, pts + 8, o.points);
    return o;
}

ShoalObstacle HexObstacle(float x, float y, float r)
{
    ShoalObstacle o = {};
    o.x = x; o.y = y; o.radius = r; o.pointCount = 6;
    for (int p = 0; p < 6; ++p)
    {
        float a = 2.0f * 3.14159265f * p / 6.0f;
        o.points[p * 2] = std::cos(a) * r;
        o.points[p * 2 + 1] = std::sin(a) * r;
    }
    return o;
}

// ------------------------------------------------------------------------------------------------ renderer
// Runs the engine in the scene-graph context (the desktop's GPU), or hands it to a ShoalOffload on another GPU.
class ShoalRenderer : public QQuickFramebufferObject::Renderer
{
public:
    ~ShoalRenderer() override
    {
        m_offload.reset(); // the scene-graph context is current here, as both need
        m_local.reset();
    }

    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        format.setInternalTextureFormat(GL_RGBA8);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject* item) override
    {
        auto* shoal = static_cast<ShoalItem*>(item);
        m_item = shoal;
        m_in.s = shoal->m_settings;

        const qreal dpr = item->window() ? item->window()->effectiveDevicePixelRatio() : 1.0;
        const QSize world(qRound(item->width() * dpr), qRound(item->height() * dpr));
        m_in.world = world;
        QRectF water = shoal->m_water.isEmpty() ? QRectF(0, 0, item->width(), item->height()) : shoal->m_water;
        water = QRectF(water.x() * dpr, water.y() * dpr, water.width() * dpr, water.height() * dpr).intersected(QRectF(QPointF(), QSizeF(world)));
        if (water.width() < 64 || water.height() < 64) water = QRectF(QPointF(), QSizeF(world));
        m_in.water = water;

        // Desktop obstacles arrive in item coordinates; the simulation runs in device pixels like the standalone port.
        if (shoal->m_obstacleGen != m_obstacleGen || m_staticDpr != dpr)
        {
            m_obstacleGen = shoal->m_obstacleGen;
            m_staticDpr = dpr;
            m_in.statics.clear();
            for (ShoalObstacle ob : shoal->m_obstacles)
            {
                ob.x *= dpr; ob.y *= dpr; ob.radius *= dpr;
                for (float& p : ob.points) p *= dpr;
                m_in.statics.push_back(ob);
            }
            ++m_in.staticsGen;
        }
        m_in.extra.clear();
        if (m_in.s.avoidMouse && shoal->m_haveMouse)
        {
            // Soft (negative radius): fish steer around the pointer but are never shoved; one it lands on darts away.
            ShoalObstacle hex = HexObstacle(shoal->m_mouse.x() * dpr, shoal->m_mouse.y() * dpr, 18.0f * dpr);
            hex.radius = -hex.radius;
            m_in.extra.push_back(hex);
        }
    }

    void render() override
    {
        auto* gl = QOpenGLContext::currentContext() ? QOpenGLContext::currentContext()->functions() : nullptr;
        QOpenGLFramebufferObject* fbo = framebufferObject();
        if (!gl || !fbo) return;
        if (!m_checked)
        {
            m_checked = true;
            QSurfaceFormat f = QOpenGLContext::currentContext()->format();
            if (QOpenGLContext::currentContext()->isOpenGLES() || f.version() < qMakePair(4, 3))
                m_error = QStringLiteral("Shoal needs desktop OpenGL 4.3 (got %1.%2)").arg(f.majorVersion()).arg(f.minorVersion());
            m_desktopGpu = Q(ShoalCurrentContextGpu());
            ShoalInstallLogger();
        }

        // The setting is resolved (sysfs scan) only when it changes, not every frame.
        if (m_error.isEmpty() && (!m_gpuChosen || m_in.s.gpu != m_gpuSetting))
        {
            m_gpuChosen = true;
            m_gpuSetting = m_in.s.gpu;
            const QString gpu = Q(ShoalResolveGpu(m_gpuSetting.toStdString()));
            m_gpu = gpu;
            m_offload.reset();
            m_local.reset();
            const QString desktopName = GpuName(m_desktopGpu);
            if (!gpu.isEmpty() && gpu != m_desktopGpu)
            {
                QString why;
                m_offload = ShoalOffload::create(gpu, &why);
                if (m_offload)
                    qInfo("Shoal: rendering on %s (%s); frames are copied to the desktop's GPU (%s)", qPrintable(m_offload->renderer()),
                          qPrintable(gpu), qPrintable(desktopName));
                else
                    qWarning("Shoal: can't render on GPU %s (%s), using the desktop's GPU (%s) instead", qPrintable(gpu), qPrintable(why),
                             qPrintable(desktopName));
                m_presented = false;
            }
            if (!m_offload)
                qInfo("Shoal: rendering on the desktop's GPU, %s (%s)", qPrintable(desktopName), qPrintable(m_desktopGpu));
        }

        if (m_offload)
        {
            if (fbo != m_lastFbo) { m_lastFbo = fbo; m_presented = false; }
            if (m_offload->present(fbo->texture(), fbo->size())) m_presented = true;
            else if (!m_presented)
            {
                // Nothing from the other GPU yet: show the plain water rather than an uninitialised texture.
                fbo->bind();
                gl->glViewport(0, 0, fbo->width(), fbo->height());
                gl->glDisable(GL_SCISSOR_TEST);
                gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                if (m_in.s.transparent) gl->glClearColor(0, 0, 0, 0);
                else
                {
                    const QColor bg(QRgb(m_in.s.background));
                    gl->glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.0f);
                }
                gl->glClear(GL_COLOR_BUFFER_BIT);
            }
            m_in.world = fbo->size();
            m_offload->submit(m_in);
        }
        else
        {
            if (!m_local && m_error.isEmpty())
            {
                m_local = std::make_unique<ShoalEngine>();
                std::string error;
                if (!m_local->init(&error)) { m_error = Q(error); m_local.reset(); }
            }
            fbo->bind();
            if (m_local)
            {
                m_in.world = fbo->size();
                ShoalRunFrame(*m_local, m_in);
            }
            else
            {
                gl->glClearColor(0, 0, 0, 1);
                gl->glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        if (!m_error.isEmpty() && !m_errorSent)
        {
            m_errorSent = true;
            qWarning("Shoal: %s", qPrintable(m_error));
            QPointer<ShoalItem> item = m_item;
            QString msg = m_error;
            QMetaObject::invokeMethod(item, [item, msg] { if (item) item->reportError(msg); }, Qt::QueuedConnection);
        }
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    static QString GpuName(const QString& id)
    {
        return Q(ShoalGpuName(id.toStdString()));
    }

    QPointer<ShoalItem> m_item;
    ShoalFrameInput m_in;
    int m_obstacleGen = -1;
    qreal m_staticDpr = 0;
    bool m_checked = false, m_gpuChosen = false, m_presented = false, m_errorSent = false;
    QString m_error, m_desktopGpu, m_gpu, m_gpuSetting;
    std::unique_ptr<ShoalEngine> m_local;
    std::unique_ptr<ShoalOffload> m_offload;
    QOpenGLFramebufferObject* m_lastFbo = nullptr;
};

int ToInt(const QVariant& v, int lo, int hi, int def)
{
    bool ok = false;
    int i = v.toInt(&ok);
    return ok ? std::clamp(i, lo, hi) : def;
}
bool ToBool(const QVariant& v, bool def) { return v.isValid() ? v.toBool() : def; }

} // namespace

// ------------------------------------------------------------------------------------------------ item
ShoalItem::ShoalItem(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(true); // the shaders draw y-down like the D3D original
    m_scanTimer.setInterval(400);
    connect(&m_scanTimer, &QTimer::timeout, this, &ShoalItem::scanDesktop);
    m_frameTimer.setSingleShot(true);
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_frameTimer, &QTimer::timeout, this, [this] { if (m_running) { m_frameClock.restart(); update(); } });
    m_scanTimer.start();
}

ShoalItem::~ShoalItem()
{
    if (m_window) m_window->removeEventFilter(this);
}

QQuickFramebufferObject::Renderer* ShoalItem::createRenderer() const { return new ShoalRenderer; }

void ShoalItem::reportError(const QString& e)
{
    if (m_error == e) return;
    m_error = e;
    emit errorChanged();
}

void ShoalItem::setSettings(const QVariantMap& m)
{
    m_settingsMap = m;
    WallpaperSettings s;
    s.speed = ToInt(m.value("Speed"), 1, 1000, s.speed);
    s.speedVariability = ToInt(m.value("SpeedVariability"), 0, 10, s.speedVariability);
    s.fishCoverage = ToInt(m.value("FishCoverage"), 0, 100, s.fishCoverage);
    s.randomness = ToInt(m.value("Randomness"), 0, 100, s.randomness);
    if (m.contains("Size")) s.sizeTenths = std::clamp((int)std::lround(m.value("Size").toDouble() * 10.0), 10, 100);
    s.sizeVariation = ToInt(m.value("SizeVariation"), 100, 300, s.sizeVariation);
    s.sizeBell = ToBool(m.value("SizeBell"), s.sizeBell);
    s.tailPercent = ToInt(m.value("TailPercent"), 10, 100, s.tailPercent);
    s.tailShade = ToInt(m.value("TailShade"), -100, 100, s.tailShade);
    s.depthEffect = ToBool(m.value("DepthEffect"), s.depthEffect);
    s.rockCount = ToInt(m.value("RockCount"), 0, 10, s.rockCount);
    s.colourSort = ToBool(m.value("ColourSort"), s.colourSort);
    s.colourAvoid = ToBool(m.value("ColourAvoid"), s.colourAvoid);
    s.showFps = ToBool(m.value("ShowFps"), s.showFps);
    s.frameDivisor = ToInt(m.value("FrameRateDivisor"), 1, 4, s.frameDivisor);
    s.avoidIcons = ToBool(m.value("AvoidIcons"), s.avoidIcons);
    s.avoidWidgets = ToBool(m.value("AvoidWidgets"), s.avoidWidgets);
    s.avoidMouse = ToBool(m.value("AvoidMouse"), s.avoidMouse);
    s.avoidWindows = ToBool(m.value("AvoidWindows"), s.avoidWindows);
    s.avoidMargin = ToInt(m.value("AvoidMargin"), 0, 100, s.avoidMargin);
    s.gpu = m.value("Gpu").toString();
    if (m.contains("Species"))
    {
        uint32_t mask = 0;
        for (const QString& n : m.value("Species").toString().split(',', Qt::SkipEmptyParts))
        {
            const int i = n.trimmed().toInt();
            if (i >= 1 && i <= kShoalSpeciesCount) mask |= 1u << (i - 1);
        }
        if (mask) s.speciesMask = mask;
    }
    s.speciesMode = ToInt(m.value("SpeciesMode"), 0, 1, 0);
    s.transparent = ToInt(m.value("BackgroundMode"), 0, 1, 0) == 1;
    QColor bg = m.value("BackgroundColor").value<QColor>();
    if (bg.isValid()) s.background = bg.rgb() & 0xFFFFFF;
    const QStringList colours = m.value("Colours").toString().split(',', Qt::SkipEmptyParts);
    if (!colours.isEmpty())
    {
        s.colourCount = 0;
        for (const QString& c : colours)
        {
            QColor col(c.trimmed());
            if (col.isValid() && s.colourCount < 10) s.colours[s.colourCount++] = col.rgb() & 0xFFFFFF;
        }
        if (s.colourCount == 0) { s.colourCount = 1; s.colours[0] = 0xdce6f0; }
    }
    m_settings = s;
    if (qEnvironmentVariableIsSet("SHOAL_DEBUG"))
        qInfo("Shoal: settings applied: %lld keys, FrameRateDivisor=%s speed=%d", (long long)m.size(),
              qPrintable(m.value("FrameRateDivisor").toString()), s.speed);
    if (!s.avoidIcons && !s.avoidWidgets && !s.avoidWindows && !m_obstacles.empty()) { m_obstacles.clear(); ++m_obstacleGen; }
    scanDesktop();
    emit settingsChanged();
    update();
}

void ShoalItem::setWindowRects(const QVariantList& rects)
{
    m_windowRectsVar = rects;
    m_windowRects.clear();
    for (const QVariant& v : rects)
    {
        QRectF r = v.toRectF();
        if (r.isValid()) m_windowRects.push_back(r);
    }
    emit windowRectsChanged();
    scanDesktop(); // react straight away rather than at the next 0.4 s scan
}

void ShoalItem::setRunning(bool r)
{
    if (m_running == r) return;
    m_running = r;
    qInfo("Shoal: %s", r ? "resumed" : "paused (window covers the desktop)");
    if (r) { m_scanTimer.start(); update(); }
    else { m_scanTimer.stop(); m_frameTimer.stop(); }
    emit runningChanged();
}

void ShoalItem::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickFramebufferObject::itemChange(change, data);
    if (change == ItemSceneChange) attachWindow(data.window);
}

void ShoalItem::attachWindow(QQuickWindow* w)
{
    if (m_window == w) return;
    if (m_window) { m_window->removeEventFilter(this); disconnect(m_swapConnection); }
    m_window = w;
    m_haveMouse = false;
    if (!w) return;
    w->installEventFilter(this);
    // Frame loop: every time the window has been presented, ask for the next frame (or wait for the next n-th vblank).
    m_swapConnection = connect(w, &QQuickWindow::frameSwapped, this, &ShoalItem::onFrameSwapped, Qt::QueuedConnection);
    update();
}

void ShoalItem::onFrameSwapped()
{
    if (!m_running || !isVisible()) return;
    if (m_settings.frameDivisor <= 1) { update(); return; }
    if (m_frameTimer.isActive()) return;
    double hz = m_window && m_window->screen() ? m_window->screen()->refreshRate() : 60.0;
    if (hz < 20.0) hz = 60.0;
    // Pace from our own last frame (the window may also redraw for other reasons). Aim a little early so the
    // request lands before the target vblank.
    const qint64 periodNs = qint64(m_settings.frameDivisor * 1e9 / hz);
    const qint64 sinceNs = m_frameClock.isValid() ? m_frameClock.nsecsElapsed() : periodNs;
    const qint64 waitMs = (periodNs - sinceNs) / 1000000 - 3;
    if (waitMs <= 0) { m_frameClock.restart(); update(); }
    else m_frameTimer.start(int(waitMs));
}

bool ShoalItem::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_window)
    {
        switch (e->type())
        {
        case QEvent::MouseMove:
        case QEvent::HoverMove:
        case QEvent::MouseButtonPress:
            m_mouse = mapFromScene(static_cast<QSinglePointEvent*>(e)->scenePosition());
            m_haveMouse = true;
            break;
        case QEvent::Leave:
            m_haveMouse = false;
            break;
        default:
            break;
        }
    }
    return QQuickFramebufferObject::eventFilter(o, e);
}

// Finds desktop icons (folder view delegates) and desktop widgets (applet containers) in this screen's desktop window and
// turns their rectangles into obstacles in this item's coordinates.
void ShoalItem::scanDesktop()
{
    // The desktop containment (an ancestor) knows the screen area not covered by panels.
    QRectF water;
    for (QQuickItem* p = parentItem(); p; p = p->parentItem())
    {
        QVariant v = p->property("availableScreenRect");
        if (v.isValid() && v.toRectF().isValid())
        {
            water = p->mapRectToItem(this, v.toRectF());
            break;
        }
    }
    const bool waterChanged = water != m_water;
    if (waterChanged)
    {
        m_water = water;
        qInfo("Shoal: water area %.0fx%.0f at %.0f,%.0f", water.width(), water.height(), water.x(), water.y());
    }

    std::vector<QRectF> rects;
    size_t icons = 0, widgets = 0;
    size_t windows = 0, windowCount = 0; // rects[0, windowCount) are windows
    if (m_window && (m_settings.avoidIcons || m_settings.avoidWidgets || m_settings.avoidWindows) && width() > 0 && height() > 0)
    {
        const qreal area = width() * height();
        auto addRect = [&](QRectF r, double maxShare = 0.6)
        {
            r = r.intersected(QRectF(0, 0, width(), height()));
            if (r.width() < 2 || r.height() < 2 || r.width() * r.height() > area * maxShare) return; // never wall off the whole screen
            rects.push_back(r);
        };
        // Windows may take up to 85% of the screen; bigger ones (nearly maximised) are left alone, the fish swim under them.
        if (m_settings.avoidWindows)
            for (const QRectF& r : m_windowRects) { addRect(r, 0.85); ++windows; }
        windowCount = rects.size();
        std::vector<QQuickItem*> stack{ m_window->contentItem() };
        while (!stack.empty())
        {
            QQuickItem* it = stack.back();
            stack.pop_back();
            if (!it || it == this || !it->isVisible() || it->opacity() < 0.01) continue;
            if (it->inherits("AppletContainer"))
            {
                if (m_settings.avoidWidgets) { addRect(it->mapRectToItem(this, it->boundingRect())); ++widgets; }
                continue;
            }
            if (qstrncmp(it->metaObject()->className(), "FolderItemDelegate", 18) == 0)
            {
                if (m_settings.avoidIcons && !it->property("blank").toBool())
                {
                    QRectF r;
                    for (const char* part : { "iconArea", "labelArea" })
                        if (auto* p = it->property(part).value<QQuickItem*>(); p && p->isVisible())
                            r |= p->mapRectToItem(this, p->boundingRect());
                    addRect(r.isEmpty() ? it->mapRectToItem(this, it->boundingRect()) : r);
                    ++icons;
                }
                continue;
            }
            const auto children = it->childItems();
            stack.insert(stack.end(), children.begin(), children.end());
        }
    }
    auto byPosition = [](const QRectF& a, const QRectF& b) { return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x(); };
    std::sort(rects.begin(), rects.begin() + windowCount, byPosition);
    std::sort(rects.begin() + windowCount, rects.end(), byPosition);
    bool same = rects.size() == m_rawRects.size() && windowCount == m_rawWindowCount && m_rawMargin == m_settings.avoidMargin
        && m_rawSize == m_settings.sizeTenths;
    for (size_t i = 0; same && i < rects.size(); ++i)
        same = std::abs(rects[i].x() - m_rawRects[i].x()) < 1 && std::abs(rects[i].y() - m_rawRects[i].y()) < 1
            && std::abs(rects[i].width() - m_rawRects[i].width()) < 1 && std::abs(rects[i].height() - m_rawRects[i].height()) < 1;
    if (!same || waterChanged)
    {
        m_rawRects = rects;
        m_rawWindowCount = windowCount;
        m_rawMargin = m_settings.avoidMargin;
        m_rawSize = m_settings.sizeTenths;
        // Icons and widgets are hard walls. Windows are soft: fish caught under one when it moves swim out of it.
        m_obstacles = buildObstacleField({ rects.begin() + windowCount, rects.end() });
        for (ShoalObstacle o : buildObstacleField({ rects.begin(), rects.begin() + windowCount }))
        {
            o.radius = -o.radius;
            m_obstacles.push_back(o);
        }
        ++m_obstacleGen;
    }
    if (icons != m_loggedIcons || widgets != m_loggedWidgets || windows != m_loggedWindows)
    {
        m_loggedIcons = icons; m_loggedWidgets = widgets; m_loggedWindows = windows;
        qInfo("Shoal: avoiding %zu desktop icons, %zu widgets and %zu windows (%zu obstacle blocks)", icons, widgets, windows, m_obstacles.size());
    }
}

// Turns the icon/widget rectangles into keep-out blocks: on a coarse grid, gaps narrower than about two fish lengths
// (between two obstacles, or between an obstacle and the water's edge) are filled in (a morphological closing), so
// fish can't get trapped in narrow pockets; then the margin is added and the grid is cut back into rectangles.
std::vector<ShoalObstacle> ShoalItem::buildObstacleField(const std::vector<QRectF>& rects) const
{
    std::vector<ShoalObstacle> out;
    if (rects.empty()) return out;
    const QRectF water = m_water.isEmpty() ? QRectF(0, 0, width(), height()) : m_water;
    const double cell = 6.0;
    const int gw = std::max(1, (int)std::ceil(water.width() / cell)), gh = std::max(1, (int)std::ceil(water.height() / cell));
    using Grid = std::vector<uint8_t>;
    Grid g(size_t(gw) * gh, 0);
    for (const QRectF& r : rects)
    {
        int x0 = std::max(0, (int)std::floor((r.left() - water.left()) / cell)), x1 = std::min(gw, (int)std::ceil((r.right() - water.left()) / cell));
        int y0 = std::max(0, (int)std::floor((r.top() - water.top()) / cell)), y1 = std::min(gh, (int)std::ceil((r.bottom() - water.top()) / cell));
        for (int y = y0; y < y1; ++y) std::fill(g.begin() + size_t(y) * gw + x0, g.begin() + size_t(y) * gw + std::max(x0, x1), 1);
    }
    // Square dilation, separable, with prefix sums. Cells outside the grid count as filled when outsideFilled.
    auto dilate = [&](const Grid& in, int r, bool outsideFilled)
    {
        if (r <= 0) return in;
        Grid tmp(in.size()), res(in.size());
        std::vector<int> pre(size_t(std::max(gw, gh)) + 1);
        for (int y = 0; y < gh; ++y)
        {
            for (int x = 0; x < gw; ++x) pre[x + 1] = pre[x] + in[size_t(y) * gw + x];
            for (int x = 0; x < gw; ++x)
            {
                int a = x - r, b = x + r;
                bool hit = pre[std::min(gw, b + 1)] - pre[std::max(0, a)] > 0 || (outsideFilled && (a < 0 || b >= gw));
                tmp[size_t(y) * gw + x] = hit;
            }
        }
        for (int x = 0; x < gw; ++x)
        {
            for (int y = 0; y < gh; ++y) pre[y + 1] = pre[y] + tmp[size_t(y) * gw + x];
            for (int y = 0; y < gh; ++y)
            {
                int a = y - r, b = y + r;
                bool hit = pre[std::min(gh, b + 1)] - pre[std::max(0, a)] > 0 || (outsideFilled && (a < 0 || b >= gh));
                res[size_t(y) * gw + x] = hit;
            }
        }
        return res;
    };
    auto invert = [](Grid a) { for (auto& v : a) v = !v; return a; };
    const double fishLength = std::max(4.0, height() * m_settings.sizeTenths / 1000.0);
    const int closeR = (int)std::ceil(std::max(20.0, fishLength) / cell);
    Grid closed = invert(dilate(invert(dilate(g, closeR, true)), closeR, false));
    for (size_t i = 0; i < g.size(); ++i) closed[i] |= g[i];
    Grid field = dilate(closed, (int)std::lround(m_settings.avoidMargin / cell), false);

    // Cut into rectangles: runs per row, extended downwards while the next row has the identical run.
    struct Open { int x0, x1, y0; };
    std::vector<Open> open, next;
    auto closeBlock = [&](const Open& o, int yEnd)
    {
        if (out.size() < 256)
            out.push_back(RectObstacle(water.left() + o.x0 * cell, water.top() + o.y0 * cell, (o.x1 - o.x0) * cell, (yEnd - o.y0) * cell));
    };
    for (int y = 0; y <= gh; ++y)
    {
        next.clear();
        if (y < gh)
            for (int x = 0; x < gw;)
            {
                if (!field[size_t(y) * gw + x]) { ++x; continue; }
                int x0 = x;
                while (x < gw && field[size_t(y) * gw + x]) ++x;
                auto it = std::find_if(open.begin(), open.end(), [&](const Open& o) { return o.x0 == x0 && o.x1 == x; });
                if (it != open.end()) { next.push_back(*it); open.erase(it); }
                else next.push_back({ x0, x, y });
            }
        for (const Open& o : open) closeBlock(o, y);
        open.swap(next);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ settings helpers
QVariantList ShoalConfig::gpus() const
{
    QVariantList out;
    for (const ShoalGpu& g : ShoalListGpus())
        out << QVariantMap{ { "id", Q(g.id) }, { "name", Q(g.name) }, { "dedicated", !g.bootVga } };
    return out;
}

QString ShoalConfig::standaloneSettingsPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/shoal/settings.conf");
}

QVariantMap ShoalConfig::readStandaloneSettings() const
{
    QVariantMap out;
    QFile f(standaloneSettingsPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QStringList keys = { "Speed", "SpeedVariability", "FishCoverage", "Randomness", "Size", "SizeVariation", "SizeBell",
        "TailPercent", "TailShade", "DepthEffect", "RockCount", "ColourSort", "ColourAvoid", "ShowFps", "SpeciesMode" };
    QStringList colours(10);
    int colourCount = 0;
    QTextStream in(&f);
    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.startsWith('#')) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        QString name = line.left(eq).trimmed(), value = line.mid(eq + 1);
        int comment = value.indexOf(QLatin1String("   #"));
        if (comment >= 0) value = value.left(comment);
        value = value.trimmed();
        for (const QString& k : keys)
            if (k.compare(name, Qt::CaseInsensitive) == 0)
                out[k] = k == "Size" ? QVariant(value.toDouble()) : (k.contains("Sort") || k.contains("Avoid") || k == "SizeBell" || k == "DepthEffect" || k == "ShowFps")
                    ? QVariant(value.toInt() != 0) : QVariant(value.toInt());
        if (name.compare("Species", Qt::CaseInsensitive) == 0) out["Species"] = value;
        if (name.compare("ColourCount", Qt::CaseInsensitive) == 0) colourCount = std::clamp(value.toInt(), 1, 10);
        if (name.size() == 7 && name.startsWith("Colour", Qt::CaseInsensitive) && name[6].isDigit())
            colours[name[6].digitValue()] = value.startsWith('#') ? value : '#' + value;
    }
    QStringList used;
    for (int i = 0; i < (colourCount ? colourCount : 10); ++i)
        if (!colours[i].isEmpty()) used << colours[i];
    if (!used.isEmpty()) out["Colours"] = used.join(',');
    return out;
}

namespace {
// Value of key in [group] of a KDE-style ini file (no KConfig dependency).
QString ReadIni(const QString& path, const QString& group, const QString& key)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    bool inGroup = false;
    QTextStream in(&f);
    while (!in.atEnd())
    {
        const QString line = in.readLine().trimmed();
        if (line.startsWith('[')) { inGroup = line == '[' + group + ']'; continue; }
        if (inGroup && line.startsWith(key + '=')) return line.mid(key.size() + 1).trimmed();
    }
    return {};
}
} // namespace

QString ShoalConfig::defaultWallpaper(int width, int height) const
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QString lnf = ReadIni(config + "/kdeglobals", "KDE", "LookAndFeelPackage");
    if (lnf.isEmpty()) lnf = "org.kde.breeze.desktop";
    QString image = ReadIni(QStandardPaths::locate(QStandardPaths::GenericDataLocation, "plasma/look-and-feel/" + lnf + "/contents/defaults"),
                            "Wallpaper", "Image");
    if (image.isEmpty()) image = "Next";
    if (QFileInfo(image).isAbsolute() && QFileInfo(image).isFile()) return QUrl::fromLocalFile(image).toString();
    QString dir = QFileInfo(image).isAbsolute() ? image
                                                : QStandardPaths::locate(QStandardPaths::GenericDataLocation, "wallpapers/" + image, QStandardPaths::LocateDirectory);
    if (dir.isEmpty()) dir = QStandardPaths::locate(QStandardPaths::GenericDataLocation, "wallpapers/Next", QStandardPaths::LocateDirectory);
    if (dir.isEmpty()) return {};

    // Like the Image wallpaper: the dark variant for dark colour schemes, then the size closest to the screen's shape.
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    QDir images(dir + (dark && QDir(dir + "/contents/images_dark").exists() ? "/contents/images_dark" : "/contents/images"));
    const QStringList files = images.entryList({ "*.png", "*.jpg", "*.jpeg", "*.webp", "*.avif" }, QDir::Files);
    if (width <= 0 || height <= 0) { width = 1920; height = 1080; }
    const double want = double(width) / height;
    QString best;
    double bestScore = 1e30;
    for (const QString& f : files)
    {
        const QStringList wh = QFileInfo(f).completeBaseName().split('x');
        if (wh.size() != 2) { if (best.isEmpty()) best = f; continue; }
        const int w = wh[0].toInt(), h = wh[1].toInt();
        if (w <= 0 || h <= 0) continue;
        // Aspect mismatch dominates; among equal shapes prefer the smallest image that still covers the screen.
        double score = std::fabs(std::log((double(w) / h) / want)) * 1e6 + (w >= width && h >= height ? w * double(h) * 1e-6 : 1e3);
        if (score < bestScore) { bestScore = score; best = f; }
    }
    return best.isEmpty() ? QString() : QUrl::fromLocalFile(images.filePath(best)).toString();
}
