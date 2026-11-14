import QtQuick
import QtQuick.Controls.Basic
import MLC

// Exact replica of original MyComboBox (Controls/MyComboBox.vb)
ComboBox {
    id: control

    implicitWidth: 180
    implicitHeight: 28

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    // Dynamic border color (same logic as MLCTextBox)
    property color borderColor: {
        if (!enabled) return Theme.gray5
        if (down || popup.visible) return Theme.color3
        if (control.hovered) return Theme.color4
        return Theme.colorBg0
    }
    property color bgColor: {
        if (!enabled) return Theme.gray6
        if (down || popup.visible || control.hovered) return Theme.color7
        return Theme.halfWhite
    }

    contentItem: Text {
        text: control.displayText
        font: control.font
        color: control.enabled ? Theme.color1 : Theme.gray4
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        leftPadding: 8.5
        rightPadding: 21.5
    }

    // Chevron arrow indicator
    indicator: Canvas {
        x: control.width - width - 10
        y: (control.availableHeight - height) / 2
        width: 15
        height: 10

        rotation: control.popup.visible ? 180 : 0
        Behavior on rotation { RotationAnimation { duration: 200; easing.type: Easing.OutCubic } }

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.strokeStyle = control.borderColor
            ctx.lineWidth = 1.5
            ctx.beginPath()
            ctx.moveTo(1, 1)
            ctx.lineTo(7.5, 7.5)
            ctx.lineTo(14, 1)
            ctx.stroke()
        }
    }

    background: Rectangle {
        radius: Theme.inputRadius
        border.width: 1
        border.color: control.borderColor
        color: control.bgColor

        Behavior on border.color { ColorAnimation { duration: control.down || control.popup.visible ? 10 : 100 } }
        Behavior on color { ColorAnimation { duration: 100 } }
    }

    popup: Popup {
        y: control.height - 1.5
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight, 320)
        padding: 0
        topPadding: 1
        bottomPadding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(contentHeight, 320)
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            radius: Theme.buttonRadius
            border.width: 1
            border.color: control.borderColor
            color: Theme.pureWhite
        }
    }

    delegate: ItemDelegate {
        // qmllint disable unqualified
        width: control.width
        height: 30
        contentItem: Text {
                                            // qmllint disable unqualified
            text: modelData
            font: control.font
            color: Theme.color1
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8.5
        }
        background: Rectangle {
            color: control.hovered ? Theme.color7 : "transparent"
        }
    }
}
