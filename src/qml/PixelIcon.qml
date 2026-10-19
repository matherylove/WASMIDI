import QtQuick

// Pixel-art icon: rects is a list of [x, y, w, h] in icon pixels.
Item {
    id: icon
    property var rects: []
    property real pixel: 2
    property color color: Dk.text
    readonly property int gridWidth: extent(0)
    readonly property int gridHeight: extent(1)
    width: gridWidth * pixel
    height: gridHeight * pixel

    function extent(axis) {
        var m = 0
        for (var i = 0; i < rects.length; ++i)
            m = Math.max(m, rects[i][axis] + rects[i][axis + 2])
        return m
    }

    Repeater {
        model: icon.rects
        Rectangle {
            required property var modelData
            x: modelData[0] * icon.pixel
            y: modelData[1] * icon.pixel
            width: modelData[2] * icon.pixel
            height: modelData[3] * icon.pixel
            color: modelData.length > 4 ? modelData[4] : icon.color
        }
    }
}
