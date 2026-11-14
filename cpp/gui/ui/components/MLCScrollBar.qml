import QtQuick
import QtQuick.Controls.Basic
import MLC

// Original PCL scrollbar: 8px wide, thumb radius 3, semi-transparent track
ScrollBar {
    id: control

    implicitWidth: Theme.scrollbarWidth
    policy: ScrollBar.AsNeeded

    contentItem: Rectangle {
        implicitWidth: Theme.scrollbarWidth
        radius: Theme.scrollbarThumbRadius
        color: control.pressed ? Theme.color4 : Theme.colorBg0
        opacity: control.hovered || control.pressed ? 0.8 : 0.4

        Behavior on color { ColorAnimation { duration: 100 } }
        Behavior on opacity { NumberAnimation { duration: 100 } }
    }

    background: Rectangle {
        implicitWidth: Theme.scrollbarWidth
        radius: 2
        color: Theme.semiTransparent
    }
}
