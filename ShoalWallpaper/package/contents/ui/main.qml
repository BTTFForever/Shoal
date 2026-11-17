import QtQuick
import QtQuick.Window
import org.kde.plasma.plasmoid
import org.kde.taskmanager as TaskManager
import org.shoal.wallpaper as Shoal

WallpaperItem {
    id: root

    readonly property var settingKeys: ["Speed", "SpeedVariability", "FishCoverage", "Randomness", "Size", "SizeVariation",
        "SizeBell", "TailPercent", "TailShade", "DepthEffect", "RockCount", "ColourSort", "ColourAvoid", "Colours",
        "BackgroundColor", "AvoidIcons", "AvoidWidgets", "AvoidMouse", "AvoidWindows", "AvoidMargin", "FrameRateDivisor", "ShowFps", "BackgroundMode", "Gpu",
        "Species", "SpeciesMode"]

    // True while a maximised, full-screen or screen-filling window is on this screen (current desktop and activity).
    property bool covered: false
    // Other (normal) windows on this screen, in wallpaper coordinates, for the fish to swim around.
    property var windowRects: []

    accentColor: root.configuration.BackgroundColor

    TaskManager.VirtualDesktopInfo { id: virtualDesktopInfo }
    TaskManager.ActivityInfo { id: activityInfo }
    TaskManager.TasksModel {
        id: tasksModel
        groupMode: TaskManager.TasksModel.GroupDisabled
        screenGeometry: Qt.rect(root.Screen.virtualX, root.Screen.virtualY, root.Screen.width, root.Screen.height)
        filterByScreen: true
        virtualDesktop: virtualDesktopInfo.currentDesktop
        filterByVirtualDesktop: true
        activity: activityInfo.currentActivity
        filterByActivity: true
        filterMinimized: true
        filterHidden: true
    }
    Connections {
        target: tasksModel
        function onDataChanged() { coverTimer.restart() }
        function onRowsInserted() { coverTimer.restart() }
        function onRowsRemoved() { coverTimer.restart() }
        function onModelReset() { coverTimer.restart() }
        function onCountChanged() { coverTimer.restart() }
        function onScreenGeometryChanged() { coverTimer.restart() }
    }
    Timer {
        id: coverTimer
        interval: 100
        onTriggered: root.updateCovered()
    }
    Component.onCompleted: coverTimer.restart()

    function updateCovered() {
        const s = tasksModel.screenGeometry;
        const roles = TaskManager.AbstractTasksModel;
        let c = false;
        const rects = [];
        for (let i = 0; i < tasksModel.count; ++i) {
            const idx = tasksModel.index(i, 0);
            if (tasksModel.data(idx, roles.IsMinimized) || tasksModel.data(idx, roles.IsHidden))
                continue;
            if (tasksModel.data(idx, roles.IsFullScreen) || tasksModel.data(idx, roles.IsMaximized)) {
                c = true;
                continue;
            }
            // A plain window dragged/resized to (almost) fill the screen counts too.
            const g = tasksModel.data(idx, roles.Geometry);
            if (g && g.width > 0 && g.height > 0)
                rects.push(Qt.rect(g.x - s.x, g.y - s.y, g.width, g.height));
            if (g && s.width > 0 && s.height > 0) {
                const w = Math.min(g.x + g.width, s.x + s.width) - Math.max(g.x, s.x);
                const h = Math.min(g.y + g.height, s.y + s.height) - Math.max(g.y, s.y);
                if (w > 0 && h > 0 && w * h >= 0.95 * s.width * s.height)
                    c = true;
            }
        }
        covered = c;
        windowRects = rects;
    }

    // Picture mode: the wallpaper picture under see-through water (the fish's shadows fall on it).
    readonly property bool pictureMode: root.configuration.BackgroundMode === 1
    Image {
        id: picture
        anchors.fill: parent
        visible: root.pictureMode
        source: !root.pictureMode ? ""
            : root.configuration.BackgroundImage ? root.configuration.BackgroundImage
            : Shoal.ShoalConfig.defaultWallpaper(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        fillMode: Image.PreserveAspectCrop
        // Decode at screen size, not at the file's (the default is 5120x2880).
        sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        asynchronous: true
        cache: false
        smooth: true
    }
    Rectangle {
        anchors.fill: parent
        visible: root.pictureMode && root.configuration.BackgroundDim > 0
        color: "black"
        opacity: root.configuration.BackgroundDim / 100
    }

    Shoal.ShoalView {
        id: view
        anchors.fill: parent
        running: !(root.configuration.PauseWhenCovered && root.covered)
        windowRects: root.windowRects
        settings: {
            const m = {};
            for (const k of root.settingKeys)
                m[k] = root.configuration[k];
            return m;
        }
    }

    Text {
        anchors.centerIn: parent
        visible: view.error.length > 0
        color: "white"
        text: "Shoal: " + view.error
    }
}
