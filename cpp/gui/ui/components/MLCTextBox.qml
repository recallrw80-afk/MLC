import QtQuick
import QtQuick.Controls.Basic
import MLC

// MyTextBox — directly inherits TextField, background provides visual style
TextField {
    id: field

    property alias hintText: hintTextItem.text
    property bool hasError: false
    property string errorText: ""

    // enabled / text / placeholderText / readOnly / validator / echoMode
    // — all inherited from TextField, no aliases needed

    implicitWidth: 200
    implicitHeight: 28

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
    color: enabled ? Theme.color1 : Theme.gray4
    verticalAlignment: TextInput.AlignVCenter
    selectionColor: Theme.color3
    selectedTextColor: Theme.pureWhite

    leftPadding: 7
    rightPadding: 7

    // Dynamic border color
    property color borderColor: {
        if (!field.enabled) return Theme.gray5
        if (hasError) return Theme.redLight
        if (field.activeFocus) return Theme.color3
        if (field.hovered) return Theme.color4
        return Theme.colorBg0
    }
    property color bgColor: {
        if (!field.enabled) return Theme.gray6
        if (hasError) return Theme.redBack
        if (field.activeFocus || field.hovered) return Theme.color7
        return Theme.halfWhite
    }

    // Background
    background: Rectangle {
        radius: Theme.inputRadius
        border.width: 1
        border.color: field.borderColor
        color: field.bgColor

        Behavior on border.color { ColorAnimation { duration: field.hasError || field.activeFocus ? 10 : 100 } }
        Behavior on color { ColorAnimation { duration: 100 } }

        // Hint text (placeholder overlay)
        Text {
            id: hintTextItem
            anchors.fill: parent
            anchors.leftMargin: 7
            verticalAlignment: Text.AlignVCenter
            text: ""
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize
            color: Theme.gray4
            visible: !field.text && !field.activeFocus && text !== ""
        }
    }
}
