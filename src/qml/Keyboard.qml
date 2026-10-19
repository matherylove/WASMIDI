import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    clip: true

    KeyboardSurface {
        anchors.fill: parent
        controller: root.mainWindow
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Dk.light2
    }
}
