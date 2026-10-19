import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Note colors: the 16-color loop by track or channel, its editor, config.xml
// import/export and "adapt". Cancel puts back what was there when it opened.
DkDialog {
    id: dialog
    required property var mainWindow
    readonly property var mw: mainWindow
    property int selected: 0
    property var saved: ({})

    title: "Note colors"
    width: Math.min(712, parent ? parent.width - 16 : 712)

    readonly property color current: mw.channelColorList[selected]

    function setComponent(index, value) {
        var c = dialog.current
        var rgb = [c.r, c.g, c.b]
        rgb[index] = Math.max(0, Math.min(255, Math.round(value))) / 255
        mw.setChannelColor(selected, Qt.rgba(rgb[0], rgb[1], rgb[2], 1))
    }

    function hex(c) {
        function two(v) { var s = Math.round(v * 255).toString(16); return s.length < 2 ? "0" + s : s }
        return "#" + two(c.r) + two(c.g) + two(c.b)
    }

    onAboutToShow: saved = {
        colors: mw.channelColorList.slice(),
        perTrack: mw.perTrackColors,
        adapt: mw.adaptColors
    }

    function cancel() {
        mw.setChannelColors(saved.colors)
        mw.perTrackColors = saved.perTrack
        mw.adaptColors = saved.adapt
        close()
    }

    ColumnLayout {
        width: dialog.availableWidth - 8
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 14

            ColumnLayout {
                Layout.preferredWidth: 380
                Layout.alignment: Qt.AlignTop
                spacing: 16

                RowLayout {
                    spacing: 10
                    Text { text: "Color by"; color: Dk.dim; font.family: Dk.ui; font.pixelSize: 12 }
                    Row {
                        spacing: 3
                        DkButton { lcd: true; text: "TRACK"; on: dialog.mw.perTrackColors; onClicked: dialog.mw.perTrackColors = true }
                        DkButton { lcd: true; text: "CHANNEL"; on: !dialog.mw.perTrackColors; onClicked: dialog.mw.perTrackColors = false }
                    }
                }

                DkGroup {
                    Layout.fillWidth: true
                    title: "Palette · 16 colors in a loop"
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 8
                        columnSpacing: 4
                        rowSpacing: 4
                        Repeater {
                            model: 16
                            Item {
                                required property int index
                                Layout.fillWidth: true
                                implicitHeight: 38
                                z: dialog.selected === index ? 1 : 0
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    visible: dialog.selected === parent.index
                                    color: "transparent"
                                    border.color: Dk.white
                                    border.width: 2
                                }
                                NoteSwatch {
                                    anchors.fill: parent
                                    noteColor: dialog.mw.channelColorList[parent.index]
                                }
                                Text {
                                    x: 4
                                    y: 2
                                    text: parent.index + 1
                                    color: "#000000"
                                    opacity: 0.65
                                    font.family: Dk.term
                                    font.pixelSize: 15
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: dialog.selected = parent.index
                                }
                            }
                        }
                    }
                }

                DkGroup {
                    Layout.fillWidth: true
                    title: "Color " + (dialog.selected + 1)
                    NoteSwatch {
                        Layout.fillWidth: true
                        Layout.bottomMargin: 4
                        implicitHeight: 16
                        noteColor: dialog.current
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 3
                        columnSpacing: 8
                        rowSpacing: 6
                        Repeater {
                            model: [["R", 0], ["G", 1], ["B", 2]]
                            delegate: Text {
                                required property var modelData
                                Layout.row: modelData[1]
                                Layout.column: 0
                                text: modelData[0]
                                color: Dk.dim
                                font.family: Dk.ui
                                font.pixelSize: 12
                            }
                        }
                        Repeater {
                            model: 3
                            delegate: DkSlider {
                                required property int index
                                Layout.row: index
                                Layout.column: 1
                                Layout.fillWidth: true
                                from: 0
                                to: 255
                                stepSize: 1
                                value: Math.round([dialog.current.r, dialog.current.g, dialog.current.b][index] * 255)
                                onMoved: dialog.setComponent(index, value)
                            }
                        }
                        Repeater {
                            model: 3
                            delegate: Text {
                                required property int index
                                Layout.row: index
                                Layout.column: 2
                                Layout.preferredWidth: 30
                                horizontalAlignment: Text.AlignRight
                                text: Math.round([dialog.current.r, dialog.current.g, dialog.current.b][index] * 255)
                                color: Dk.text
                                font.family: Dk.term
                                font.pixelSize: 15
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        spacing: 8
                        Text { text: "Hex"; color: Dk.dim; font.family: Dk.ui; font.pixelSize: 12 }
                        DkField {
                            Layout.preferredWidth: 84
                            validator: RegularExpressionValidator { regularExpression: /#?[0-9a-fA-F]{0,6}/ }
                            text: dialog.hex(dialog.current)
                            onEditingFinished: {
                                var v = text.charAt(0) === "#" ? text : "#" + text
                                if (/^#[0-9a-fA-F]{6}$/.test(v))
                                    dialog.mw.setChannelColor(dialog.selected, v)
                                text = dialog.hex(dialog.current)
                            }
                        }
                        Item { Layout.fillWidth: true }
                        DkButton { text: "Reset palette"; onClicked: dialog.mw.resetChannelColors() }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    DkButton { Layout.fillWidth: true; text: "Import config.xml"; onClicked: dialog.mw.importColorConfig() }
                    DkButton { Layout.fillWidth: true; text: "Export config.xml"; onClicked: dialog.mw.exportColorConfig() }
                }
                Text {
                    Layout.fillWidth: true
                    text: dialog.mw.perTrackColors
                        ? "Track N takes color ((N − 1) mod 16) + 1, so track 17 starts the loop again."
                        : "Each MIDI channel takes the palette color with its number."
                    color: Dk.dim
                    font.family: Dk.ui
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 318
                    color: Dk.well
                    DkBevel { anchors.fill: parent; kind: 1 }
                    ListView {
                        id: rows
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: dialog.mw.perTrackColors ? Math.max(17, dialog.mw.trackCount) : 16
                        ScrollBar.vertical: ScrollBar { policy: rows.contentHeight > rows.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
                        delegate: Item {
                            required property int index
                            readonly property int colorNumber: index % 16 + 1
                            width: rows.width
                            height: 18
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                x: 6
                                spacing: 8
                                NoteSwatch {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 22
                                    height: 9
                                    noteColor: dialog.mw.channelColorList[index % 16]
                                }
                                Text {
                                    width: 74
                                    text: (dialog.mw.perTrackColors ? "Track " : "Ch ") + (index + 1)
                                    color: Dk.white
                                    font.family: Dk.term
                                    font.pixelSize: 16
                                }
                                Text {
                                    width: rows.width - 200
                                    text: dialog.mw.perTrackColors
                                        ? (index === 16 ? "loops back to color 1" : "")
                                        : (index === 9 ? "Drums" : "")
                                    color: Dk.dim
                                    font.family: Dk.ui
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: "color " + colorNumber
                                    color: Dk.faint
                                    font.family: Dk.term
                                    font.pixelSize: 14
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: dialog.selected = index % 16
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            DkCheckBox {
                text: "Adapt the player to the colors playing most"
                checked: dialog.mw.adaptColors
                onToggled: dialog.mw.adaptColors = checked
            }
            Item { Layout.fillWidth: true }
            DkButton { implicitWidth: 76; text: "OK"; font.bold: true; onClicked: dialog.close() }
            DkButton { implicitWidth: 76; text: "Cancel"; onClicked: dialog.cancel() }
        }
    }
}
