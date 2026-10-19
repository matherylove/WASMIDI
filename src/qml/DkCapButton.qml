import QtQuick
import QtQuick.Controls.Basic

// 16x14 caption button with a pixel glyph.
Button {
    id: control
    property var glyph: []
    implicitWidth: 16
    implicitHeight: 14
    padding: 0
    focusPolicy: Qt.NoFocus
    background: Rectangle {
        color: Dk.face
        DkBevel { anchors.fill: parent; kind: control.down ? 1 : 0 }
    }
    contentItem: Item {
        PixelIcon {
            anchors.centerIn: parent
            pixel: 1
            rects: control.glyph
            color: control.hovered ? Dk.white : Dk.text
        }
    }
}
