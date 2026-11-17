// Shoal as a KDE Plasma live wallpaper: a Qt Quick item that runs the ShoalLinux GLSL simulation and renderer
// inside plasmashell's own OpenGL scene graph.
#pragma once

#include "shoaloffload.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QPointF>
#include <QQuickFramebufferObject>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <vector>

class ShoalItem : public QQuickFramebufferObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(ShoalView)
    // Settings keyed like main.xml (built in main.qml from WallpaperItem.configuration).
    Q_PROPERTY(QVariantMap settings READ settings WRITE setSettings NOTIFY settingsChanged)
    // False pauses the simulation and stops requesting frames (window maximised/full screen on this screen).
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // Application windows on this screen (item coordinates), from the task manager model in main.qml.
    Q_PROPERTY(QVariantList windowRects READ windowRects WRITE setWindowRects NOTIFY windowRectsChanged)

public:
    explicit ShoalItem(QQuickItem* parent = nullptr);
    ~ShoalItem() override;
    Renderer* createRenderer() const override;

    QVariantMap settings() const { return m_settingsMap; }
    void setSettings(const QVariantMap& m);
    bool running() const { return m_running; }
    void setRunning(bool r);
    QString error() const { return m_error; }
    QVariantList windowRects() const { return m_windowRectsVar; }
    void setWindowRects(const QVariantList& rects);

    // Read by the renderer in synchronize() (GUI thread blocked).
    WallpaperSettings m_settings;
    std::vector<ShoalObstacle> m_obstacles; // icons/widgets, then windows (negative radius = soft); gaps closed, margin added (item coordinates)
    int m_obstacleGen = 0;                  // bumped whenever m_obstacles changes
    QRectF m_water; // panel-free part of the screen, item coordinates (empty = whole item)
    bool m_haveMouse = false;
    QPointF m_mouse;
    void reportError(const QString& e);

signals:
    void settingsChanged();
    void runningChanged();
    void errorChanged();
    void windowRectsChanged();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    void scanDesktop();
    std::vector<ShoalObstacle> buildObstacleField(const std::vector<QRectF>& rects) const;
    void onFrameSwapped();
    void attachWindow(QQuickWindow* w);

    QVariantMap m_settingsMap;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_swapConnection;
    QTimer m_scanTimer, m_frameTimer;
    QElapsedTimer m_frameClock;
    bool m_running = true;
    QString m_error;
    std::vector<QRectF> m_rawRects, m_windowRects;
    size_t m_rawWindowCount = 0;
    QVariantList m_windowRectsVar;
    int m_rawMargin = -1, m_rawSize = -1;
    size_t m_loggedIcons = ~size_t(0), m_loggedWidgets = ~size_t(0), m_loggedWindows = ~size_t(0);
};

// Helpers for the settings page.
class ShoalConfig : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    using QObject::QObject;
    // Values of ~/.config/shoal/settings.conf (the standalone port's settings), keyed like main.xml.
    Q_INVOKABLE QVariantMap readStandaloneSettings() const;
    Q_INVOKABLE QString standaloneSettingsPath() const;
    // Plasma's default wallpaper for the current Look-and-Feel theme (what the Image wallpaper shows when no picture
    // is chosen), the variant best matching width x height, as a file URL.
    Q_INVOKABLE QString defaultWallpaper(int width, int height) const;
    // The GPUs, dedicated ones first: [{ id: PCI address, name, dedicated }]. ""/unknown ids mean the first dedicated one.
    Q_INVOKABLE QVariantList gpus() const;
};
