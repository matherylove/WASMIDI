import QtQuick

// Status bar cell: thin-in well with a dim label and a lavender value.
Item {
    id: cell
    property string label: ""
    property string value: ""
    implicitWidth: row.implicitWidth + 12
    implicitHeight: 18

    DkBevel { anchors.fill: parent; kind: 2 }
    Row {
        id: row
        x: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        Text { text: cell.label; visible: text.length > 0; color: Dk.dim; font.family: Dk.ui; font.pixelSize: 11 }
        Text { text: cell.value; visible: text.length > 0; color: Dk.lav; font.family: Dk.ui; font.pixelSize: 11 }
    }
}
