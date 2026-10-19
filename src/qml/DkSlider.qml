import QtQuick
import QtQuick.Controls.Basic

// Range input as the browser draws it with accent-color violet.
Slider {
    id: control
    property color accent: Dk.violet
    implicitHeight: 16
    implicitWidth: 100
    padding: 0
    focusPolicy: Qt.NoFocus
    opacity: enabled ? 1.0 : 0.5

    background: Item {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: control.availableWidth
        height: 4
        Rectangle {
            anchors.fill: parent
            color: Dk.field
            DkBevel { anchors.fill: parent; kind: 2 }
        }
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            color: control.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 13
        height: 13
        radius: 6.5
        color: control.pressed ? Qt.lighter(control.accent, 1.2) : control.accent
        border.color: Qt.darker(control.accent, 1.6)
        border.width: 1
    }
}
