import QtQuick

// LCD readout: small label over a VT323 value.
Rectangle {
    id: readout
    property string label: ""
    property string value: ""
    property color valueColor: Dk.white
    color: Dk.readout
    implicitHeight: 38
    clip: true

    DkBevel { anchors.fill: parent; kind: 2 }
    Column {
        x: 6
        y: 2
        width: parent.width - 10
        Text { text: readout.label; color: Dk.faint; font.family: Dk.ui; font.pixelSize: 11 }
        Text {
            width: parent.width
            text: readout.value
            color: readout.valueColor
            font.family: Dk.term
            font.pixelSize: 20
            elide: Text.ElideRight
        }
    }
}
