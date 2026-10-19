import QtQuick
import Wasmidi

// Keyboard beside the roll, one row (portrait: one column) per key like the roll.
Item {
    id: root
    required property var mainWindow
    property bool portrait: false
    clip: true

    KeyboardSurface {
        anchors.fill: parent
        controller: root.mainWindow
        portrait: root.portrait
    }
}
