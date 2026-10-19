import QtQuick
import Wasmidi

// Vertical keyboard at the left of the roll, one row per key like the roll.
Item {
    id: root
    required property var mainWindow
    clip: true

    KeyboardSurface {
        anchors.fill: parent
        controller: root.mainWindow
    }
}
