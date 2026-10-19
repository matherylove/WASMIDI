import QtQuick
import QtQuick.Controls.Basic

// Checkbox as the browser draws it with accent-color violet.
CheckBox {
    id: control
    padding: 0
    spacing: 6
    focusPolicy: Qt.NoFocus
    opacity: enabled ? 1.0 : 0.5

    indicator: Rectangle {
        x: control.leftPadding
        y: (control.height - height) / 2
        width: 13
        height: 13
        color: control.checked ? Dk.violet : Dk.field
        DkBevel { anchors.fill: parent; kind: control.checked ? 0 : 1 }
        PixelIcon {
            anchors.centerIn: parent
            visible: control.checked
            pixel: 1
            color: Dk.desk
            rects: [[0, 2, 1, 2], [1, 3, 1, 2], [2, 4, 1, 2], [3, 3, 1, 2], [4, 2, 1, 2], [5, 1, 1, 2], [6, 0, 1, 2]]
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        color: Dk.text
        font.family: Dk.ui
        font.pixelSize: 12
        verticalAlignment: Text.AlignVCenter
    }
}
