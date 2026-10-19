import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import Wasmidi

ApplicationWindow {
    id: window
    visible: true
    width: 1440
    height: 900
    minimumWidth: 840
    minimumHeight: 600
    color: Dk.desk
    title: mainWindow.fileName.length
        ? "WebAssembly MIDI Player — " + mainWindow.fileName
        : "WebAssembly MIDI Player"

    property int colorIndex: 0

    PlayerController {
        id: mainWindow
        onLoadFailed: (message) => {
            errorDialog.text = message
            errorDialog.open()
        }
    }

    MessageDialog {
        id: errorDialog
        title: "WebAssembly MIDI Player"
    }

    ColorDialog {
        id: colorDialog
        title: "Color " + (window.colorIndex + 1)
        // Every preview change goes straight to both GPU renderers.
        onSelectedColorChanged: {
            if (visible)
                mainWindow.setChannelColor(window.colorIndex, selectedColor)
        }
        onAccepted: mainWindow.setChannelColor(window.colorIndex, selectedColor)
    }

    NeuralBackground {
        anchors.fill: parent
    }

    // The player window: face over the neural field, as see-through as "Page" asks.
    Rectangle {
        id: frame
        anchors.fill: parent
        color: Qt.rgba(Dk.face.r, Dk.face.g, Dk.face.b, 1.0 - mainWindow.neuralPage * 0.5)

        DkBevel { anchors.fill: parent; kind: 0 }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 0

            DkCaption {
                Layout.fillWidth: true
                title: window.title
                iconRects: Glyphs.logo
                DkCapButton {
                    glyph: Glyphs.fullScreen
                    ToolTip.visible: hovered
                    ToolTip.text: "Full screen"
                    onClicked: mainWindow.toggleFullScreen()
                }
            }

            // Header: LCD and transport.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                Layout.bottomMargin: 6
                Layout.leftMargin: 2
                Layout.rightMargin: 2
                spacing: 6

                Rectangle {
                    id: lcd
                    Layout.fillWidth: true
                    Layout.minimumWidth: 360
                    implicitHeight: lcdRow.implicitHeight + 12
                    color: Dk.lcd
                    DkBevel { anchors.fill: parent; kind: 1 }

                    RowLayout {
                        id: lcdRow
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        anchors.topMargin: 6
                        anchors.bottomMargin: 6
                        spacing: 10

                        ColumnLayout {
                            Layout.minimumWidth: 140
                            Layout.alignment: Qt.AlignTop
                            spacing: 2
                            Text {
                                text: Dk.clock(mainWindow.currentTime)
                                color: Dk.white
                                font.family: Dk.term
                                font.pixelSize: 44
                                font.letterSpacing: 1
                                lineHeight: 0.9
                            }
                            Text {
                                text: "/ " + Dk.clock(mainWindow.duration)
                                color: Dk.faint
                                font.family: Dk.term
                                font.pixelSize: 16
                            }
                            Row {
                                Layout.topMargin: 2
                                spacing: 6
                                readonly property string state: mainWindow.midiLoading ? "LOAD"
                                    : !mainWindow.hasMidi ? "NO MIDI"
                                    : mainWindow.isPlaying ? "PLAY"
                                    : mainWindow.currentTime > 0 ? "PAUSE" : "STOP"
                                PixelIcon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    pixel: 1
                                    rects: parent.state === "PLAY"
                                        ? [[0, 0, 2, 9], [2, 1, 2, 7], [4, 2, 2, 5], [6, 3, 2, 3], [8, 4, 1, 1]]
                                        : parent.state === "PAUSE" ? [[0, 0, 3, 9], [6, 0, 3, 9]]
                                        : [[0, 0, 9, 9]]
                                    color: parent.state === "PLAY" ? Dk.good
                                        : parent.state === "PAUSE" ? Dk.warn
                                        : parent.state === "STOP" ? Dk.pink : Dk.faint
                                }
                                Text {
                                    text: parent.state
                                    color: Dk.dim
                                    font.family: Dk.term
                                    font.pixelSize: 15
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: 4

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                color: Dk.readout
                                clip: true
                                DkBevel { anchors.fill: parent; kind: 2 }
                                Text {
                                    anchors.fill: parent
                                    anchors.leftMargin: 6
                                    anchors.rightMargin: 6
                                    verticalAlignment: Text.AlignVCenter
                                    text: mainWindow.hasMidi
                                        ? mainWindow.fileName.toUpperCase() + " · "
                                          + Dk.integer(mainWindow.trackCount) + " TRACKS · "
                                          + Dk.integer(mainWindow.ppq) + " PPQ"
                                        : "WEBASSEMBLY MIDI PLAYER · DROP A MIDI OR PRESS MIDI…"
                                    color: Dk.white
                                    font.family: Dk.term
                                    font.pixelSize: 20
                                    elide: Text.ElideRight
                                }
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 5
                                columnSpacing: 3
                                DkReadout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1.4
                                    label: "Notes"
                                    value: Dk.integer(mainWindow.noteCount)
                                }
                                DkReadout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    label: "NPS"
                                    value: Dk.integer(mainWindow.nps)
                                    valueColor: Qt.lighter(Dk.accent, 1.6)
                                }
                                DkReadout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    label: "Polyphony"
                                    value: Dk.integer(mainWindow.activeVoices)
                                }
                                DkReadout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 0.7
                                    label: "BPM"
                                    value: mainWindow.hasMidi ? Dk.number(mainWindow.bpm, 1) : "—"
                                }
                                DkReadout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 0.7
                                    label: "FPS"
                                    value: Dk.integer(roll.fps)
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: false
                    Layout.preferredWidth: Math.min(430, window.width * 0.3)
                    Layout.maximumWidth: 430
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 6

                    DkSlider {
                        Layout.fillWidth: true
                        accent: Dk.accentLight
                        from: 0
                        to: Math.max(0.001, mainWindow.duration)
                        value: mainWindow.currentTime
                        enabled: mainWindow.hasMidi
                        onMoved: mainWindow.seek(value)
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        DkButton {
                            implicitWidth: 34
                            implicitHeight: 28
                            iconRects: Glyphs.play
                            on: mainWindow.isPlaying
                            enabled: mainWindow.hasMidi
                            onClicked: mainWindow.play()
                        }
                        DkButton {
                            implicitWidth: 34
                            implicitHeight: 28
                            iconRects: Glyphs.pause
                            on: mainWindow.hasMidi && !mainWindow.isPlaying && mainWindow.currentTime > 0
                            enabled: mainWindow.hasMidi
                            onClicked: mainWindow.pause()
                        }
                        DkButton {
                            implicitWidth: 34
                            implicitHeight: 28
                            iconRects: Glyphs.stop
                            enabled: mainWindow.hasMidi
                            onClicked: mainWindow.stop()
                        }
                        DkButton {
                            implicitHeight: 28
                            text: "MIDI…"
                            onClicked: mainWindow.openMidiPicker()
                        }
                        DkButton {
                            implicitHeight: 28
                            implicitWidth: 28
                            visible: mainWindow.hasMidi
                            iconRects: Glyphs.close
                            ToolTip.visible: hovered
                            ToolTip.text: "Close the MIDI"
                            onClicked: mainWindow.clearFile()
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: "VOL"
                            color: Dk.faint
                            font.family: Dk.term
                            font.pixelSize: 15
                        }
                        DkSlider {
                            Layout.preferredWidth: 110
                            from: 0
                            to: 100
                            value: mainWindow.volume
                            onMoved: mainWindow.volume = Math.round(value)
                        }
                    }
                }
            }

            // Roll and dock.
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 2
                Layout.rightMargin: 2
                spacing: 6

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    DkBevel { anchors.fill: parent; kind: 1 }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 2
                        spacing: 0

                        Keyboard {
                            Layout.preferredWidth: 62
                            Layout.fillHeight: true
                            mainWindow: mainWindow
                        }

                        PianoRoll {
                            id: roll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            mainWindow: mainWindow
                            onOpenRequested: mainWindow.openMidiPicker()
                        }
                    }
                }

                Dock {
                    Layout.preferredWidth: 304
                    Layout.fillHeight: true
                    mainWindow: mainWindow
                    onAdvancedRequested: advancedDialog.open()
                    onColorsRequested: colorsDialog.open()
                }
            }

            // Status bar.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 2
                DkCell {
                    value: ""
                    label: mainWindow.midiLoading ? "Loading " + mainWindow.midiLoadingProgress + "%"
                        : !mainWindow.hasMidi ? "Ready"
                        : mainWindow.isPlaying ? "Playing" : "Paused"
                }
                DkCell { label: "FPS"; value: Dk.integer(roll.fps) }
                DkCell {
                    label: "WasmiSynth"
                    value: !mainWindow.synthReady ? "starting"
                        : Dk.integer(mainWindow.synthMaxVoices) + " voices · "
                          + (mainWindow.synthWorkerCount > 0 ? Dk.integer(mainWindow.synthWorkerCount) : "—")
                          + " workers · " + Dk.number(mainWindow.synthRenderLoad, 0) + "%"
                }
                Item {
                    Layout.fillWidth: true
                    implicitHeight: 18
                    DkBevel { anchors.fill: parent; kind: 2 }
                }
                DkCell {
                    visible: mainWindow.hasMidi
                    label: Dk.integer(mainWindow.trackCount) + " tracks · " + Dk.integer(mainWindow.ppq) + " PPQ"
                }
            }
        }
    }

    AdvancedDialog {
        id: advancedDialog
        mainWindow: mainWindow
    }

    DkDialog {
        id: colorsDialog
        title: "Colors"
        width: 360

        ColumnLayout {
            width: colorsDialog.availableWidth - 8
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: mainWindow.perTrackColors
                    ? "Tracks take these colors in order and loop back to color 1 after 16."
                    : "One color per MIDI channel."
                color: Dk.dim
                font.family: Dk.ui
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 8
                columnSpacing: 3
                rowSpacing: 3
                Repeater {
                    model: 16
                    Rectangle {
                        required property int index
                        Layout.fillWidth: true
                        implicitHeight: 30
                        color: mainWindow.channelColorList[index]
                        DkBevel { anchors.fill: parent; kind: 1 }
                        Text {
                            anchors.centerIn: parent
                            text: index + 1
                            color: "#000000"
                            opacity: 0.6
                            font.family: Dk.term
                            font.pixelSize: 16
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                window.colorIndex = parent.index
                                colorDialog.selectedColor = mainWindow.channelColorList[parent.index]
                                colorDialog.open()
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                DkButton { text: "Close"; implicitWidth: 75; onClicked: colorsDialog.close() }
            }
        }
    }

    // MIDI loading window.
    Item {
        anchors.fill: parent
        visible: mainWindow.midiLoading
        z: 1000

        Rectangle { anchors.fill: parent; color: "#8007071a" }
        MouseArea { anchors.fill: parent; hoverEnabled: true }

        Rectangle {
            width: Math.min(460, parent.width - 48)
            height: loadColumn.implicitHeight + 8
            anchors.centerIn: parent
            color: Dk.face
            DkBevel { anchors.fill: parent; kind: 0 }

            ColumnLayout {
                id: loadColumn
                anchors.fill: parent
                anchors.margins: 4
                spacing: 8

                DkCaption {
                    Layout.fillWidth: true
                    title: "Loading MIDI"
                    iconRects: Glyphs.logo
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    Layout.rightMargin: 6
                    Text {
                        Layout.fillWidth: true
                        text: mainWindow.midiLoadingStage.length ? mainWindow.midiLoadingStage : "Preparing"
                        color: Dk.text
                        font.family: Dk.ui
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Text {
                        text: mainWindow.midiLoadingProgress + "%"
                        color: Dk.white
                        font.family: Dk.term
                        font.pixelSize: 20
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    Layout.rightMargin: 6
                    Layout.bottomMargin: 8
                    implicitHeight: 18
                    color: Dk.field
                    DkBevel { anchors.fill: parent; kind: 1 }
                    // Progress as discrete blocks, like the dekx.cc progress bars.
                    Row {
                        x: 3
                        y: 3
                        spacing: 2
                        Repeater {
                            model: Math.floor((parent.parent.width - 6) / 10
                                              * Math.max(0, Math.min(1, mainWindow.midiLoadingProgress / 100.0)))
                            Rectangle { width: 8; height: 12; color: Dk.violet }
                        }
                    }
                }
            }
        }
    }
}
