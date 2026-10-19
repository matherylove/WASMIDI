import QtQuick
import QtQuick.Layouts

// Active window caption: 20 px, horizontal gradient, bold title, caption buttons.
Rectangle {
    id: caption
    property string title: ""
    property color startColor: Dk.captionA
    property color endColor: Dk.accent
    default property alias buttons: buttonRow.data
    property alias iconRects: icon.rects
    implicitHeight: 20
    gradient: Gradient {
        orientation: Gradient.Horizontal
        GradientStop { position: 0.0; color: caption.startColor }
        GradientStop { position: 1.0; color: caption.endColor }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 3
        anchors.rightMargin: 2
        spacing: 4
        PixelIcon { id: icon; pixel: 1; visible: rects.length > 0; Layout.preferredWidth: 16; Layout.alignment: Qt.AlignVCenter }
        Text {
            Layout.fillWidth: true
            text: caption.title
            color: "#ffffff"
            font.family: Dk.ui
            font.pixelSize: 12
            font.bold: true
            elide: Text.ElideRight
        }
        Row { id: buttonRow; spacing: 2 }
    }
}
