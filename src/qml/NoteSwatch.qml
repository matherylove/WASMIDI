import QtQuick

// A note color drawn like the roll's notes: gradient, darker edge and the
// 2 px texture.
Rectangle {
    id: swatch
    property color noteColor: "#a78bfa"
    property bool edge: true

    gradient: Gradient {
        GradientStop { position: 0.0; color: Dk.mix(swatch.noteColor, 0.45) }
        GradientStop { position: 0.45; color: swatch.noteColor }
        GradientStop { position: 1.0; color: Dk.mix(swatch.noteColor, -0.45) }
    }
    border.color: Dk.mix(swatch.noteColor, -0.7)
    border.width: edge ? 1 : 0

    Image {
        anchors.fill: parent
        anchors.margins: swatch.border.width
        source: Dk.ditherLight
        fillMode: Image.Tile
        smooth: false
    }
}
