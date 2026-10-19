import QtQuick
import QtQuick.Controls.Basic

// dekx.css button. `on` shows the pressed-in state; `lcd` is the VT323 toggle.
Button {
    id: control
    property bool on: checked
    property bool lcd: false
    property alias iconRects: icon.rects
    property color iconColor: Dk.text

    implicitHeight: lcd ? 22 : 23
    implicitWidth: Math.max(lcd ? 0 : 23, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: lcd ? 8 : 10
    rightPadding: lcd ? 8 : 10
    topPadding: 0
    bottomPadding: 0
    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    opacity: enabled ? 1.0 : 0.5

    font.family: lcd ? Dk.term : Dk.ui
    font.pixelSize: lcd ? 15 : 12
    font.letterSpacing: lcd ? 1 : 0

    background: Rectangle {
        color: control.on && control.lcd ? Dk.toggleOn : Dk.face
        Image {
            anchors.fill: parent
            visible: control.on && !control.lcd
            source: Dk.ditherFace
            fillMode: Image.Tile
            smooth: false
        }
        DkBevel {
            anchors.fill: parent
            kind: control.down || control.on ? 1 : 0
        }
    }

    contentItem: Item {
        implicitWidth: icon.rects.length ? icon.width : label.implicitWidth
        implicitHeight: label.implicitHeight
        Text {
            id: label
            anchors.centerIn: parent
            visible: !icon.rects.length
            text: control.text
            font: control.font
            color: control.on || control.hovered ? Dk.white : Dk.text
        }
        PixelIcon {
            id: icon
            anchors.centerIn: parent
            color: control.on || control.hovered ? Dk.white : control.iconColor
        }
    }
}
