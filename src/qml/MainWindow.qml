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
    minimumWidth: 320
    minimumHeight: 480
    color: Dk.desk
    title: mainWindow.fileName.length
        ? "WebAssembly MIDI Player — " + mainWindow.fileName
        : "WebAssembly MIDI Player"

    // The accent follows the notes' dominant color, gently.
    property color uiAccent: mainWindow.accentColor
    Behavior on uiAccent { ColorAnimation { duration: 600 } }
    Binding { target: Dk; property: "accent"; value: window.uiAccent }

    // Phone: a portrait window. Notes fall onto a keyboard laid
    // across the screen, and the panel opens one tab at a time (HANDOFF sec. 96).
    readonly property bool phone: width < height && width < 760
    // Compact: a short or narrow landscape window (a phone on its side) keeps the
    // horizontal roll with the phone header, panel and transport (HANDOFF sec. 97).
    readonly property bool compact: !phone && (height < 540 || width < 860)
    readonly property bool mobileUi: phone || compact
    readonly property bool falling: phone
    property string phoneTab: ""
    onMobileUiChanged: phoneTab = ""
    readonly property var phoneTabs: [
        { key: "song", label: "Song", glyph: Glyphs.song },
        { key: "colors", label: "Colors", glyph: Glyphs.palette },
        { key: "synth", label: "Synth", glyph: Glyphs.speaker },
        { key: "view", label: "View", glyph: Glyphs.gear }
    ]

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
                visible: !window.mobileUi
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
                                columns: 6
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
                                    valueColor: Dk.mix(Dk.accent, 0.25)
                                }
                                DkReadout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    label: "Polyphony"
                                    value: Dk.integer(mainWindow.activeVoices)
                                }
                                DkReadout {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 0.9
                                    label: "CC/s"
                                    value: Dk.integer(mainWindow.ccPerSecond)
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

            // Phone header: clock and the three live counters.
            RowLayout {
                visible: window.mobileUi
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                spacing: 3

                Rectangle {
                    implicitWidth: phoneClock.implicitWidth + 16
                    implicitHeight: 42
                    color: Dk.lcd
                    DkBevel { anchors.fill: parent; kind: 1 }
                    Column {
                        x: 8
                        y: 3
                        Text {
                            id: phoneClock
                            text: Dk.clock(mainWindow.currentTime)
                            color: Dk.white
                            font.family: Dk.term
                            font.pixelSize: 28
                            lineHeight: 0.85
                        }
                        Text {
                            text: "/ " + Dk.clock(mainWindow.duration)
                            color: Dk.faint
                            font.family: Dk.term
                            font.pixelSize: 13
                        }
                    }
                }
                DkReadout {
                    Layout.fillWidth: true
                    implicitHeight: 42
                    label: "NPS"
                    value: Dk.integer(mainWindow.nps)
                    valueColor: Dk.mix(Dk.accent, 0.25)
                }
                DkReadout {
                    Layout.fillWidth: true
                    implicitHeight: 42
                    label: "Poly"
                    value: Dk.integer(mainWindow.activeVoices)
                }
                DkReadout {
                    Layout.fillWidth: true
                    implicitHeight: 42
                    label: "CC/s"
                    value: Dk.integer(mainWindow.ccPerSecond)
                }
                DkReadout {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 48
                    implicitHeight: 42
                    label: "FPS"
                    value: Dk.integer(roll.fps)
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

                    // Desktop: keyboard at the left. Phone: keyboard along the bottom.
                    Item {
                        id: rollArea
                        anchors.fill: parent
                        anchors.margins: 2
                        readonly property int keySize: window.phone ? 46 : window.falling ? 36 : 62

                        Keyboard {
                            x: 0
                            y: window.falling ? parent.height - height : 0
                            width: window.falling ? parent.width : rollArea.keySize
                            height: window.falling ? rollArea.keySize : parent.height
                            portrait: window.falling
                            mainWindow: mainWindow
                        }

                        PianoRoll {
                            id: roll
                            x: window.falling ? 0 : rollArea.keySize
                            y: 0
                            width: parent.width - (window.falling ? 0 : rollArea.keySize)
                            height: parent.height - (window.falling ? rollArea.keySize : 0)
                            portrait: window.falling
                            mainWindow: mainWindow
                            onOpenRequested: mainWindow.openMidiPicker()
                        }

                        // Phone panel: the tab's groups over the lower part of the roll.
                        Rectangle {
                            visible: window.mobileUi && window.phoneTab !== ""
                            x: 0
                            width: parent.width
                            height: Math.min(roll.height, Math.max(phoneDock.implicitHeight + 16,
                                window.phoneTab === "song" ? roll.height * 0.45 : 0))
                            y: roll.height - height
                            color: Dk.face
                            DkBevel { anchors.fill: parent; kind: 0 }
                            MouseArea { anchors.fill: parent }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 4

                                RowLayout {
                                    visible: window.phoneTab === "synth"
                                    Layout.fillWidth: true
                                    Layout.topMargin: 6
                                    spacing: 7
                                    Text {
                                        text: "VOL"
                                        color: Dk.faint
                                        font.family: Dk.term
                                        font.pixelSize: 15
                                    }
                                    DkSlider {
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 100
                                        value: mainWindow.volume
                                        onMoved: mainWindow.volume = Math.round(value)
                                    }
                                }

                                Flickable {
                                    id: phoneFlick
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    contentWidth: width
                                    contentHeight: phoneDock.height
                                    boundsBehavior: Flickable.StopAtBounds
                                    Dock {
                                        id: phoneDock
                                        width: phoneFlick.width
                                        height: Math.max(phoneFlick.height, implicitHeight)
                                        section: window.phoneTab
                                        mainWindow: mainWindow
                                        onAdvancedRequested: advancedDialog.open()
                                        onColorsRequested: colorsDialog.open()
                                    }
                                }
                            }
                        }
                    }
                }

                Dock {
                    id: dock
                    visible: !window.mobileUi
                    Layout.preferredWidth: 304
                    Layout.fillHeight: true
                    mainWindow: mainWindow
                    onAdvancedRequested: advancedDialog.open()
                    onColorsRequested: colorsDialog.open()
                }
            }

            // Phone transport: position, buttons and the panel tabs.
            ColumnLayout {
                visible: window.mobileUi
                Layout.fillWidth: true
                Layout.topMargin: window.compact ? 4 : 6
                spacing: 6

                DkSlider {
                    visible: window.phone
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    accent: Dk.accentLight
                    from: 0
                    to: Math.max(0.001, mainWindow.duration)
                    value: mainWindow.currentTime
                    enabled: mainWindow.hasMidi
                    onMoved: mainWindow.seek(value)
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    spacing: 6
                    DkButton {
                        implicitWidth: 48
                        implicitHeight: 44
                        iconRects: Glyphs.folder
                        onClicked: mainWindow.openMidiPicker()
                    }
                    Item { visible: window.phone; Layout.fillWidth: true }
                    DkButton {
                        implicitWidth: 48
                        implicitHeight: 44
                        iconRects: Glyphs.stop
                        enabled: mainWindow.hasMidi
                        onClicked: mainWindow.stop()
                    }
                    DkButton {
                        implicitWidth: 64
                        implicitHeight: 44
                        iconRects: Glyphs.play
                        on: mainWindow.isPlaying
                        enabled: mainWindow.hasMidi
                        onClicked: mainWindow.play()
                    }
                    DkButton {
                        implicitWidth: 48
                        implicitHeight: 44
                        iconRects: Glyphs.pause
                        on: mainWindow.hasMidi && !mainWindow.isPlaying && mainWindow.currentTime > 0
                        enabled: mainWindow.hasMidi
                        onClicked: mainWindow.pause()
                    }
                    Item { visible: window.phone; Layout.fillWidth: true }
                    DkSlider {
                        visible: window.compact
                        Layout.fillWidth: true
                        accent: Dk.accentLight
                        from: 0
                        to: Math.max(0.001, mainWindow.duration)
                        value: mainWindow.currentTime
                        enabled: mainWindow.hasMidi
                        onMoved: mainWindow.seek(value)
                    }
                    DkButton {
                        implicitWidth: 64
                        implicitHeight: 44
                        lcd: true
                        font.pixelSize: 18
                        text: Dk.number(mainWindow.noteSpeed, mainWindow.noteSpeed < 10 ? 1 : 0) + " s"
                        on: window.phoneTab === "view"
                        onClicked: window.phoneTab = window.phoneTab === "view" ? "" : "view"
                    }
                    Repeater {
                        model: window.compact ? window.phoneTabs : []
                        DkButton {
                            required property var modelData
                            implicitWidth: 44
                            implicitHeight: 44
                            iconRects: modelData.glyph
                            on: window.phoneTab === modelData.key
                            onClicked: window.phoneTab = on ? "" : modelData.key
                        }
                    }
                }

                RowLayout {
                    visible: window.phone
                    Layout.fillWidth: true
                    spacing: 3
                    Repeater {
                        model: window.phoneTabs
                        DkButton {
                            id: tabButton
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 48
                            on: window.phoneTab === modelData.key
                            onClicked: window.phoneTab = on ? "" : modelData.key
                            contentItem: Item {
                              implicitHeight: tabColumn.implicitHeight
                              Column {
                                id: tabColumn
                                anchors.centerIn: parent
                                spacing: 4
                                PixelIcon {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    rects: tabButton.modelData.glyph
                                    color: tabButton.on ? Dk.white : Dk.dim
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: tabButton.modelData.label
                                    color: tabButton.on ? Dk.white : Dk.dim
                                    font.family: Dk.ui
                                    font.pixelSize: 11
                                }
                              }
                            }
                        }
                    }
                }
            }

            // Status bar.
            RowLayout {
                visible: !window.mobileUi
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

    ColorsDialog {
        id: colorsDialog
        mainWindow: mainWindow
    }

    FrameAnimation {
        running: mainWindow.isPlaying
        onTriggered: mainWindow.advanceFrame()
    }

    // Notices at the top right of the roll.
    Toasts {
        parent: rollArea
        x: roll.x + roll.width - width - 6
        y: roll.y + 6
        z: 50
        mainWindow: mainWindow
        roll: roll
        dock: window.mobileUi ? phoneDock : dock
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
