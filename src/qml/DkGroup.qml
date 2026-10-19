import QtQuick
import QtQuick.Layouts

// dekx.css group box: etched frame with the title on its top edge.
Item {
    id: group
    property string title: ""
    default property alias content: column.data
    property alias spacing: column.spacing

    implicitWidth: column.implicitWidth + 18
    implicitHeight: column.implicitHeight + 22

    Rectangle { anchors.fill: parent; color: "transparent"; border.color: Dk.dark2 }
    Rectangle { x: 1; y: 1; width: parent.width - 2; height: 1; color: Dk.light2 }
    Rectangle { x: 1; y: 1; width: 1; height: parent.height - 2; color: Dk.light2 }
    Rectangle { x: 1; y: parent.height; width: parent.width; height: 1; color: Dk.light2 }
    Rectangle { x: parent.width; y: 1; width: 1; height: parent.height; color: Dk.light2 }

    Rectangle {
        x: 7
        y: -8
        width: legend.implicitWidth + 6
        height: 15
        color: Dk.face
        Text {
            id: legend
            x: 3
            anchors.verticalCenter: parent.verticalCenter
            text: group.title
            color: Dk.text
            font.family: Dk.ui
            font.pixelSize: 12
        }
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.leftMargin: 9
        anchors.rightMargin: 9
        anchors.topMargin: 13
        anchors.bottomMargin: 9
        spacing: 6
    }
}
