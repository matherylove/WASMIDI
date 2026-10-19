import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A dekx.cc child window shown over the player.
Popup {
    id: dialog
    property string title: ""
    default property alias content: body.data
    property alias bodyItem: body

    parent: Overlay.overlay
    anchors.centerIn: parent
    padding: 4
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape

    Overlay.modal: Rectangle { color: "#8007071a" }

    background: Rectangle {
        color: Dk.face
        DkBevel { anchors.fill: parent; kind: 0 }
    }

    contentItem: ColumnLayout {
        spacing: 6
        DkCaption {
            Layout.fillWidth: true
            title: dialog.title
            DkCapButton {
                glyph: Glyphs.close
                onClicked: dialog.close()
            }
        }
        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 4
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }
}
