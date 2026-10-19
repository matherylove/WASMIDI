import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    signal openRequested()
    clip: true

    property bool dragActive: false
    property bool portrait: false
    property int fps: 0

    // Clears to the "Roll" share of its background, so the page's neural field
    // shows through.
    PianoRollSurface {
        id: surface
        anchors.fill: parent
        controller: root.mainWindow
        portrait: root.portrait
    }

    Text {
        anchors.centerIn: parent
        visible: !root.mainWindow.hasMidi && !root.dragActive
        text: root.portrait ? "Tap to open a MIDI" : "Drop a MIDI here or click to open"
        color: Dk.dim
        font.family: Dk.term
        font.pixelSize: 22
    }

    Rectangle {
        anchors.fill: parent
        visible: root.dragActive
        color: "#cc1f175a"
        border.color: Dk.violet
        border.width: 2

        Text {
            anchors.centerIn: parent
            text: "Drop MIDI to load"
            color: Dk.white
            font.family: Dk.term
            font.pixelSize: 26
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: !root.mainWindow.hasMidi
        cursorShape: Qt.PointingHandCursor
        onClicked: root.openRequested()
    }

    DropArea {
        anchors.fill: parent

        onEntered: (drag) => {
            root.dragActive = true
            drag.acceptProposedAction()
        }

        onExited:
            root.dragActive = false

        onDropped: (drop) => {
            root.dragActive = false

            if (drop.urls &&
                drop.urls.length > 0) {
                root.mainWindow.loadMidiUrl(
                    drop.urls[0])
            } else {
                root.openRequested()
            }

            drop.acceptProposedAction()
        }
    }

    // Samples the completed C++/WebGL render rate; this timer does not drive
    // frames. Renderer::update() is the only animation source.
    Timer {
        interval: 500
        running: root.visible
        repeat: true
        onTriggered:
            root.fps =
                surface.renderFps
    }
}
