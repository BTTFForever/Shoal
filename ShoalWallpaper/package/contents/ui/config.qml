import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls
import org.shoal.wallpaper as Shoal

Kirigami.FormLayout {
    id: root
    twinFormLayouts: parentLayout
    property alias formLayout: root

    // ---- settings bound to main.xml (cfg_ properties are read and written by Plasma's dialog)
    property alias cfg_Speed: speed.value
    property alias cfg_SpeedVariability: speedVariability.value
    property alias cfg_FishCoverage: coverage.value
    property alias cfg_Randomness: randomness.value
    property real cfg_Size: 2.4
    property alias cfg_SizeVariation: sizeVariation.value
    property alias cfg_SizeBell: sizeBell.checked
    property alias cfg_TailPercent: tail.value
    property alias cfg_TailShade: shade.value
    property alias cfg_DepthEffect: depth.checked
    property alias cfg_RockCount: rocks.value
    property alias cfg_ColourSort: colourSort.checked
    property alias cfg_ColourAvoid: colourAvoid.checked
    property string cfg_Colours
    property alias cfg_BackgroundColor: background.color
    property alias cfg_AvoidIcons: avoidIcons.checked
    property alias cfg_AvoidWidgets: avoidWidgets.checked
    property alias cfg_AvoidMouse: avoidMouse.checked
    property alias cfg_AvoidWindows: avoidWindows.checked
    property alias cfg_PauseWhenCovered: pause.checked
    property int cfg_FrameRateDivisor: 1
    property alias cfg_ShowFps: showFps.checked
    property int cfg_BackgroundMode: 1
    property string cfg_BackgroundImage
    property alias cfg_BackgroundDim: dim.value
    property alias cfg_AvoidMargin: margin.value
    property string cfg_Gpu
    property string cfg_Species
    property int cfg_SpeciesMode

    readonly property var gpuList: Shoal.ShoalConfig.gpus()

    onCfg_SizeChanged: size.value = Math.round(cfg_Size * 10)
    Component.onCompleted: size.value = Math.round(cfg_Size * 10)
    readonly property var colourList: cfg_Colours.length ? cfg_Colours.split(",") : []
    function setColours(list) { cfg_Colours = list.join(",") }
    readonly property var speciesNames: ["Classic", "Sardine", "Goldfish", "Koi", "Tuna", "Trout", "Pike", "Tetra", "Fantail", "Mackerel"]
    readonly property var speciesChosen: cfg_Species.split(",").map(n => parseInt(n)).filter(n => n >= 1 && n <= 10)
    function setSpecies(number, on) {
        let list = speciesChosen.filter(n => n !== number);
        if (on) list.push(number);
        if (list.length === 0) list = [number]; // keep at least one
        cfg_Species = list.sort((a, b) => a - b).join(",");
    }

    // A slider with its value shown next to it.
    component SliderRow: RowLayout {
        id: row
        property int value
        property int from: 0
        property int to: 100
        property var format: v => String(v)
        onValueChanged: slider.value = value
        QQC2.Slider {
            id: slider
            Layout.preferredWidth: Kirigami.Units.gridUnit * 14
            from: row.from
            to: row.to
            stepSize: 1
            value: row.value
            onMoved: row.value = Math.round(value)
        }
        QQC2.Label {
            Layout.minimumWidth: Kirigami.Units.gridUnit * 6
            text: row.format(row.value)
        }
    }

    QQC2.Button {
        Kirigami.FormData.label: i18n("Standalone Shoal:")
        icon.name: "document-import"
        text: i18n("Copy settings from settings.conf")
        QQC2.ToolTip.text: Shoal.ShoalConfig.standaloneSettingsPath()
        QQC2.ToolTip.visible: hovered
        onClicked: {
            const m = Shoal.ShoalConfig.readStandaloneSettings();
            for (const k in m)
                root["cfg_" + k] = m[k];
        }
    }

    // ---------------------------------------------------------------- fish
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18n("Fish") }
    SliderRow { id: coverage; Kirigami.FormData.label: i18n("Fish amount:"); from: 0; to: 100 }
    SliderRow {
        id: size
        Kirigami.FormData.label: i18n("Fish size:")
        from: 10; to: 100
        format: v => (v / 10).toFixed(1) + i18n("% of screen height")
        onValueChanged: root.cfg_Size = value / 10
    }
    SliderRow { id: sizeVariation; Kirigami.FormData.label: i18n("Size variability:"); from: 100; to: 300; format: v => v === 100 ? i18n("off") : "×" + (v / 100).toFixed(2) }
    QQC2.CheckBox { id: sizeBell; text: i18n("Most fish near the set size (bell curve)"); enabled: sizeVariation.value > 100 }
    SliderRow { id: tail; Kirigami.FormData.label: i18n("Tail size:"); from: 10; to: 100; format: v => v + "%" }
    SliderRow { id: shade; Kirigami.FormData.label: i18n("Tail shade:"); from: -100; to: 100; format: v => v === 0 ? i18n("none") : v < 0 ? i18n("%1% darker", -v) : i18n("%1% lighter", v) }
    QQC2.CheckBox { id: depth; text: i18n("3D depth and shadows") }
    GridLayout {
        Kirigami.FormData.label: i18n("Species:")
        columns: 5
        Repeater {
            model: root.speciesNames
            QQC2.CheckBox {
                required property int index
                required property string modelData
                text: (index + 1) + " " + modelData
                checked: root.speciesChosen.indexOf(index + 1) >= 0
                onToggled: root.setSpecies(index + 1, checked)
            }
        }
    }
    QQC2.ComboBox {
        Kirigami.FormData.label: i18n("Species mix:")
        model: [i18n("Random mix"), i18n("One species per colour")]
        currentIndex: root.cfg_SpeciesMode
        onActivated: root.cfg_SpeciesMode = currentIndex
    }

    // ---------------------------------------------------------------- swimming
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18n("Swimming") }
    SliderRow { id: speed; Kirigami.FormData.label: i18n("Speed:"); from: 1; to: 1000; format: v => i18n("%1 px/s", v) }
    SliderRow { id: speedVariability; Kirigami.FormData.label: i18n("Speed variability:"); from: 0; to: 10 }
    SliderRow { id: randomness; Kirigami.FormData.label: i18n("Randomness:"); from: 0; to: 100 }

    // ---------------------------------------------------------------- colours
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18n("Colours") }
    RowLayout {
        Kirigami.FormData.label: i18n("Fish colours:")
        Repeater {
            model: root.colourList
            delegate: KQuickControls.ColorButton {
                required property string modelData
                required property int index
                color: modelData
                showAlphaChannel: false
                dialogTitle: i18n("Fish colour")
                onAccepted: c => {
                    const l = root.colourList.slice();
                    l[index] = c.toString();
                    root.setColours(l);
                }
            }
        }
        QQC2.ToolButton {
            icon.name: "list-add"
            enabled: root.colourList.length < 10
            QQC2.ToolTip.text: i18n("Add a colour")
            QQC2.ToolTip.visible: hovered
            onClicked: root.setColours(root.colourList.concat(["#ffffff"]))
        }
        QQC2.ToolButton {
            icon.name: "list-remove"
            enabled: root.colourList.length > 1
            QQC2.ToolTip.text: i18n("Remove the last colour")
            QQC2.ToolTip.visible: hovered
            onClicked: root.setColours(root.colourList.slice(0, -1))
        }
    }
    QQC2.CheckBox { id: colourSort; text: i18n("Fish only school with their own colour"); enabled: root.colourList.length > 1 }
    QQC2.CheckBox { id: colourAvoid; text: i18n("…and steer away from other colours"); enabled: colourSort.checked && colourSort.enabled }
    // ---------------------------------------------------------------- background
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18n("Background") }
    QQC2.ComboBox {
        Kirigami.FormData.label: i18n("Under the fish:")
        model: [i18n("Plain water colour"), i18n("Picture")]
        currentIndex: root.cfg_BackgroundMode
        onActivated: root.cfg_BackgroundMode = currentIndex
    }
    KQuickControls.ColorButton {
        id: background
        Kirigami.FormData.label: i18n("Water colour:")
        visible: root.cfg_BackgroundMode === 0
        showAlphaChannel: false
        dialogTitle: i18n("Water colour")
    }
    RowLayout {
        Kirigami.FormData.label: i18n("Picture:")
        visible: root.cfg_BackgroundMode === 1
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 16
            elide: Text.ElideMiddle
            text: root.cfg_BackgroundImage ? decodeURIComponent(root.cfg_BackgroundImage.toString().replace(/^file:\/\//, ""))
                                           : i18n("Plasma's default wallpaper")
        }
        QQC2.Button {
            icon.name: "document-open"
            text: i18n("Choose…")
            onClicked: imageDialog.open()
        }
        QQC2.Button {
            icon.name: "edit-undo"
            text: i18n("Default")
            enabled: root.cfg_BackgroundImage.length > 0
            onClicked: root.cfg_BackgroundImage = ""
        }
    }
    SliderRow {
        id: dim
        Kirigami.FormData.label: i18n("Darken picture:")
        visible: root.cfg_BackgroundMode === 1
        from: 0; to: 90
        format: v => v === 0 ? i18n("off") : v + "%"
    }
    Dialogs.FileDialog {
        id: imageDialog
        title: i18n("Choose a picture")
        nameFilters: [i18n("Images (*.png *.jpg *.jpeg *.webp *.avif *.bmp)")]
        currentFolder: "file:///usr/share/wallpapers"
        onAccepted: root.cfg_BackgroundImage = selectedFile.toString()
    }

    // ---------------------------------------------------------------- desktop
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18n("Desktop") }
    QQC2.CheckBox { id: avoidIcons; Kirigami.FormData.label: i18n("Swim around:"); text: i18n("Desktop icons") }
    QQC2.CheckBox { id: avoidWidgets; text: i18n("Desktop widgets") }
    QQC2.CheckBox { id: avoidWindows; text: i18n("Application windows") }
    QQC2.CheckBox { id: avoidMouse; text: i18n("The mouse pointer") }
    SliderRow {
        id: margin
        Kirigami.FormData.label: i18n("Keep-out margin:")
        enabled: avoidIcons.checked || avoidWidgets.checked || avoidWindows.checked
        from: 0; to: 100
        format: v => i18n("%1 px", v)
    }
    SliderRow { id: rocks; Kirigami.FormData.label: i18n("Rocks:"); from: 0; to: 10 }

    // ---------------------------------------------------------------- performance
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18n("Performance") }
    QQC2.ComboBox {
        Kirigami.FormData.label: i18n("Frame rate:")
        model: [i18n("Refresh rate (1×)"), i18n("½ refresh rate"), i18n("⅓ refresh rate"), i18n("¼ refresh rate")]
        currentIndex: root.cfg_FrameRateDivisor - 1
        onActivated: root.cfg_FrameRateDivisor = currentIndex + 1
    }
    QQC2.ComboBox {
        Kirigami.FormData.label: i18n("GPU:")
        visible: root.gpuList.length > 0
        textRole: "label"
        valueRole: "id"
        model: root.gpuList.map(g => ({
            id: g.id,
            label: g.name + (g.dedicated ? i18n(" (dedicated)") : root.gpuList.length > 1 ? i18n(" (integrated)") : "")
        }))
        // An empty or unknown setting means the dedicated GPU (the first one listed).
        currentIndex: Math.max(0, root.gpuList.findIndex(g => g.id === root.cfg_Gpu))
        onActivated: root.cfg_Gpu = currentValue
        QQC2.ToolTip.text: i18n("On a GPU other than the one drawing the desktop, each frame is copied over to it.")
        QQC2.ToolTip.visible: hovered
    }
    QQC2.CheckBox { id: pause; text: i18n("Pause while a window is maximised or full screen") }
    QQC2.CheckBox { id: showFps; text: i18n("Show frame rate") }
}
