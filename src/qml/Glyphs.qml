pragma Singleton
import QtQuick

// Pixel-art glyphs as [x, y, w, h] rects (from the approved mockup).
QtObject {
    readonly property var logo: [
        [0, 0, 6, 1, "#c4b5fd"], [0, 1, 7, 1, "#c4b5fd"], [0, 2, 2, 5, "#c4b5fd"], [6, 2, 2, 5, "#c4b5fd"],
        [0, 7, 7, 1, "#c4b5fd"], [0, 8, 6, 1, "#c4b5fd"],
        [9, 2, 4, 1, "#a78bfa"], [9, 4, 5, 1, "#818cf8"], [9, 6, 3, 1, "#f0abfc"]
    ]
    readonly property var close: [
        [0, 0, 2, 1], [6, 0, 2, 1], [1, 1, 2, 1], [5, 1, 2, 1], [2, 2, 4, 1], [3, 3, 2, 1],
        [2, 4, 4, 1], [1, 5, 2, 1], [5, 5, 2, 1], [0, 6, 2, 1], [6, 6, 2, 1]
    ]
    readonly property var fullScreen: [[0, 0, 9, 2], [0, 2, 1, 7], [8, 2, 1, 7], [0, 8, 9, 1]]
    readonly property var play: [[0, 0, 1, 8], [1, 1, 2, 6], [3, 2, 2, 4], [5, 3, 2, 2]]
    readonly property var pause: [[0, 0, 2, 8], [5, 0, 2, 8]]
    readonly property var stop: [[0, 0, 7, 7]]
    readonly property var folder: [[0, 0, 4, 1], [0, 1, 9, 1], [0, 2, 1, 5], [8, 2, 1, 5], [0, 7, 9, 1]]
    readonly property var song: [[0, 0, 9, 1], [0, 8, 9, 1], [0, 1, 1, 7], [8, 1, 1, 7], [2, 4, 1, 3], [4, 2, 1, 5], [6, 3, 1, 4]]
    readonly property var palette: [[2, 0, 5, 1], [1, 1, 1, 1], [7, 1, 1, 1], [0, 2, 1, 5], [8, 2, 1, 4], [1, 7, 6, 1],
        [2, 2, 1, 1], [5, 2, 1, 1], [2, 5, 1, 1], [6, 6, 2, 1]]
    readonly property var speaker: [[0, 3, 2, 3], [2, 2, 1, 5], [3, 1, 1, 7], [4, 0, 1, 9], [6, 3, 1, 3], [7, 2, 1, 1], [7, 6, 1, 1], [8, 3, 1, 3]]
    readonly property var gear: [[4, 0, 1, 2], [4, 7, 1, 2], [0, 4, 2, 1], [7, 4, 2, 1], [3, 2, 3, 1], [3, 6, 3, 1],
        [2, 3, 1, 3], [6, 3, 1, 3], [1, 1, 1, 1], [7, 1, 1, 1], [1, 7, 1, 1], [7, 7, 1, 1]]
}
