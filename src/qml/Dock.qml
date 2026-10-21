import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Wasmidi

// The settings column next to the roll. Everything fits: only the graph grows.
Item {
    id: dock
    required property var mainWindow
    readonly property var mw: mainWindow
    signal advancedRequested()
    signal colorsRequested()
    // Phone sheet: only one tab's groups ("song", "colors", "synth", "view"); empty shows all.
    property string section: ""

    function shows(name) { return section === "" || section === name }
    // -1 while the graph is hidden: it has nothing to draw.
    function takeGraphFrames() { return graph.visible ? graph.takeFrames() : -1 }

    component Label: Text {
        color: Dk.dim
        font.family: Dk.ui
        font.pixelSize: 12
    }

    component Unit: Text {
        color: Dk.dim
        font.family: Dk.ui
        font.pixelSize: 12
    }

    implicitHeight: dockColumn.implicitHeight + 9

    ColumnLayout {
        id: dockColumn
        anchors.fill: parent
        anchors.topMargin: 9
        anchors.rightMargin: 1
        spacing: 14

        DkGroup {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "Graphs"
            visible: dock.shows("song")

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 40
                color: Dk.lcd
                DkBevel { anchors.fill: parent; kind: 1 }
                LiveGraph {
                    id: graph
                    anchors.fill: parent
                    anchors.margins: 2
                    clip: true
                    time: dock.mw.currentTime
                    nps: dock.mw.nps
                    polyphony: dock.mw.activeVoices
                    sampling: dock.mw.hasMidi && dock.mw.isPlaying
                    span: Math.max(0.1, dock.mw.noteSpeed)
                    accent: Dk.accent
                    lineColor: Dk.accentLight
                    polyColor: Dk.pink
                    gridColor: Dk.faceHi
                    property var revision: dock.mw.documentRevision
                    onRevisionChanged: clear()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: "<font color='" + Dk.accentLight + "'>■</font> NPS " + Dk.compact(dock.mw.nps) + " / " + Dk.compact(graph.npsCeiling)
                    textFormat: Text.StyledText
                    color: Dk.dim
                    font.family: Dk.term
                    font.pixelSize: 15
                }
                Text {
                    Layout.fillWidth: true
                    text: "<font color='" + Dk.pink + "'>■</font> POLY " + Dk.compact(dock.mw.activeVoices) + " / " + Dk.compact(graph.polyCeiling)
                    textFormat: Text.StyledText
                    color: Dk.dim
                    font.family: Dk.term
                    font.pixelSize: 15
                }
                Text {
                    text: Dk.number(graph.span, graph.span < 10 ? 1 : 0) + " S"
                    color: Dk.dim
                    font.family: Dk.term
                    font.pixelSize: 15
                }
            }
        }

        DkGroup {
            Layout.fillWidth: true
            title: "Note speed"
            visible: dock.shows("view")
            RowLayout {
                Layout.fillWidth: true
                spacing: 7
                DkSlider {
                    Layout.fillWidth: true
                    from: 0.1
                    to: 10
                    stepSize: 0.05
                    value: Math.min(10, dock.mw.noteSpeed)
                    onMoved: dock.mw.noteSpeed = value
                }
                DkField {
                    id: speedField
                    Layout.preferredWidth: 60
                    decimals: 2
                    minValue: 0.1
                    maxValue: 60
                    horizontalAlignment: Text.AlignRight
                    text: Number(dock.mw.noteSpeed).toFixed(2)
                    ToolTip.visible: hovered
                    ToolTip.text: "Seconds of song across the roll (0.1 to 60)"
                    onEditingFinished: {
                        var n = parseFloat(text)
                        if (isFinite(n))
                            dock.mw.noteSpeed = Math.max(0.1, Math.min(60, n))
                        text = Number(dock.mw.noteSpeed).toFixed(2)
                    }
                }
                Unit { text: "s" }
            }
        }

        DkGroup {
            Layout.fillWidth: true
            title: "Colors"
            visible: dock.shows("colors")
            RowLayout {
                Layout.fillWidth: true
                spacing: 3
                DkButton {
                    lcd: true
                    text: "TRACK"
                    on: dock.mw.perTrackColors
                    onClicked: dock.mw.perTrackColors = true
                }
                DkButton {
                    lcd: true
                    text: "CHANNEL"
                    on: !dock.mw.perTrackColors
                    onClicked: dock.mw.perTrackColors = false
                }
                Item { Layout.fillWidth: true }
                DkButton {
                    text: "Edit…"
                    onClicked: dock.colorsRequested()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 20
                color: Dk.field
                DkBevel { anchors.fill: parent; kind: 1 }
                Row {
                    x: 2
                    y: 2
                    spacing: 2
                    readonly property real swatch: (parent.width - 4 - 15 * spacing) / 16
                    Repeater {
                        model: 16
                        NoteSwatch {
                            required property int index
                            width: parent.swatch
                            height: 16
                            noteColor: dock.mw.channelColorList[index]
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dock.colorsRequested()
                }
            }
        }

        DkGroup {
            Layout.fillWidth: true
            title: "Neural background"
            visible: dock.shows("colors")
            RowLayout {
                Layout.fillWidth: true
                spacing: 7
                Label { text: "Roll"; Layout.preferredWidth: 44 }
                DkSlider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    stepSize: 0.05
                    value: dock.mw.neuralRoll
                    onMoved: dock.mw.neuralRoll = value
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 7
                Label { text: "Page"; Layout.preferredWidth: 44 }
                DkSlider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    stepSize: 0.05
                    value: dock.mw.neuralPage
                    onMoved: dock.mw.neuralPage = value
                }
            }
        }

        DkGroup {
            Layout.fillWidth: true
            title: "WasmiSynth"
            visible: dock.shows("synth")

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 23
                    color: Dk.field
                    DkBevel { anchors.fill: parent; kind: 1 }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        text: dock.mw.soundfontLoaded
                            ? dock.mw.soundfontName
                            : (dock.mw.synthReady ? "No SoundFont" : "Starting…")
                        color: dock.mw.soundfontLoaded ? Dk.white : Dk.faint
                        font.family: Dk.ui
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                }
                DkButton {
                    text: "SF2…"
                    enabled: !dock.mw.isPlaying
                    ToolTip.visible: hovered
                    ToolTip.text: dock.mw.soundfontLoaded ? "Add an SF2 as another layer" : "Load an SF2"
                    onClicked: dock.mw.openSoundfontPicker()
                }
                DkButton {
                    text: "SFZ…"
                    enabled: !dock.mw.isPlaying
                    ToolTip.visible: hovered
                    ToolTip.text: "Load a folder with an .sfz and its samples"
                    onClicked: dock.mw.openSfzFolderPicker()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 7
                Label { text: "Voices"; Layout.preferredWidth: 44 }
                DkField {
                    Layout.preferredWidth: 72
                    enabled: !dock.mw.isPlaying
                    minValue: 1
                    maxValue: 5000000
                    text: String(dock.mw.synthMaxVoices)
                    onEditingFinished: {
                        var n = parseInt(text)
                        if (isFinite(n))
                            dock.mw.synthMaxVoices = n
                        text = String(dock.mw.synthMaxVoices)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Voice pool shared by the workers. Only voices that sound cost CPU."
                }
                Item { Layout.fillWidth: true }
                DkButton {
                    lcd: true
                    text: "CPU"
                    on: !dock.mw.synthGpu
                    onClicked: dock.mw.synthGpu = false
                }
                DkButton {
                    lcd: true
                    text: "GPU"
                    on: dock.mw.synthGpu
                    onClicked: dock.mw.synthGpu = true
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 3
                DkButton {
                    lcd: true
                    text: "STACK GAIN"
                    on: dock.mw.synthOverlapGain
                    onClicked: dock.mw.synthOverlapGain = !dock.mw.synthOverlapGain
                }
                DkButton {
                    lcd: true
                    text: "SOFT CLIP"
                    on: dock.mw.synthSoftClip
                    enabled: !dock.mw.isPlaying
                    onClicked: dock.mw.synthSoftClip = !dock.mw.synthSoftClip
                }
                Item { Layout.fillWidth: true }
                DkButton {
                    text: "Advanced…"
                    onClicked: dock.advancedRequested()
                }
            }

            // Render load of the last block against its realtime budget.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 6
                color: Dk.readout
                DkBevel { anchors.fill: parent; kind: 2 }
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, dock.mw.synthRenderLoad / 100.0))
                    height: parent.height
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Dk.good }
                        GradientStop { position: 0.7; color: Dk.warn }
                        GradientStop { position: 1.0; color: Dk.bad }
                    }
                }
            }
        }

        DkGroup {
            Layout.fillWidth: true
            title: "Roll cache"
            visible: dock.shows("view")
            RowLayout {
                Layout.fillWidth: true
                spacing: 3
                DkButton {
                    lcd: true
                    text: "LOOK-AHEAD"
                    on: dock.mw.rollMode !== 2
                    onClicked: dock.mw.rollMode = 1
                    ToolTip.visible: hovered
                    ToolTip.text: "Notes decoded ahead of the roll up to the memory limit. Applies to the next MIDI."
                }
                DkButton {
                    lcd: true
                    text: "FULL SONG"
                    on: dock.mw.rollMode === 2
                    onClicked: dock.mw.rollMode = 2
                    ToolTip.visible: hovered
                    ToolTip.text: "The whole song is decoded while the MIDI loads. Applies to the next MIDI."
                }
                Item { Layout.fillWidth: true }
                DkField {
                    Layout.preferredWidth: 58
                    visible: dock.mw.rollMode !== 2
                    minValue: 64
                    maxValue: 16384
                    horizontalAlignment: Text.AlignRight
                    text: String(dock.mw.rollLookAheadMB)
                    onEditingFinished: {
                        var n = parseInt(text)
                        if (isFinite(n))
                            dock.mw.rollLookAheadMB = n
                        text = String(dock.mw.rollLookAheadMB)
                    }
                }
                Unit { text: "MB"; visible: dock.mw.rollMode !== 2 }
            }
        }

        Item { Layout.preferredHeight: 2 }
    }
}
