import QtQuick
import QtQuick.Controls.Basic

// dekx.css sunken text field. Numeric when `decimals` >= 0.
TextField {
    id: control
    property real minValue: 0
    property real maxValue: 2147483647
    property int decimals: 0

    implicitHeight: 21
    implicitWidth: 60
    leftPadding: 5
    rightPadding: 5
    topPadding: 0
    bottomPadding: 0
    color: Dk.white
    selectionColor: "#3a2d8c"
    selectedTextColor: Dk.white
    font.family: Dk.ui
    font.pixelSize: 12
    verticalAlignment: Text.AlignVCenter
    selectByMouse: true
    opacity: enabled ? 1.0 : 0.5
    validator: control.decimals > 0
        ? doubleValidator
        : intValidator

    property IntValidator intValidator: IntValidator {
        bottom: Math.ceil(control.minValue)
        top: Math.floor(control.maxValue)
    }
    property DoubleValidator doubleValidator: DoubleValidator {
        bottom: control.minValue
        top: control.maxValue
        decimals: control.decimals
        notation: DoubleValidator.StandardNotation
        locale: "C"
    }

    background: Rectangle {
        color: Dk.field
        DkBevel { anchors.fill: parent; kind: 1 }
    }
}
