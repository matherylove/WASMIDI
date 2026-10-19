import QtQuick

// dekx.css bevels drawn as 1 px edges: 0 raised, 1 sunken, 2 thin-in.
Item {
    id: bevel
    property int kind: 0

    readonly property color outerTL: kind === 0 ? Dk.light : Dk.dark2
    readonly property color outerBR: kind === 0 ? Dk.dark : (kind === 1 ? Dk.light : Dk.light2)
    readonly property color innerTL: kind === 0 ? Dk.light2 : Dk.dark
    readonly property color innerBR: kind === 0 ? Dk.dark2 : Dk.light2
    readonly property bool twoDeep: kind !== 2

    Rectangle { x: 0; y: 0; width: parent.width - 1; height: 1; color: bevel.outerTL }
    Rectangle { x: 0; y: 1; width: 1; height: parent.height - 2; color: bevel.outerTL }
    Rectangle { x: 0; y: parent.height - 1; width: parent.width; height: 1; color: bevel.outerBR }
    Rectangle { x: parent.width - 1; y: 0; width: 1; height: parent.height - 1; color: bevel.outerBR }

    Rectangle { visible: bevel.twoDeep; x: 1; y: 1; width: parent.width - 3; height: 1; color: bevel.innerTL }
    Rectangle { visible: bevel.twoDeep; x: 1; y: 2; width: 1; height: parent.height - 4; color: bevel.innerTL }
    Rectangle { visible: bevel.twoDeep; x: 1; y: parent.height - 2; width: parent.width - 2; height: 1; color: bevel.innerBR }
    Rectangle { visible: bevel.twoDeep; x: parent.width - 2; y: 1; width: 1; height: parent.height - 3; color: bevel.innerBR }
}
