import QtQuick
import QtQuick.Controls.Basic
import MLC

// Original PCL progress bar: 4px height, gradient fill
ProgressBar {
    id: control
    implicitWidth: 200
    implicitHeight: Theme.progressBarHeight

    property color fillColorStart: Theme.color4
    property color fillColorEnd: Theme.color3

    background: Rectangle {
        implicitWidth: control.implicitWidth
        implicitHeight: Theme.progressBarHeight
        radius: 0
        color: Qt.rgba(213/255, 230/255, 253/255, 0.6) // Color6 with 0.6 opacity
    }

    contentItem: Item {
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            gradient: Gradient {
                GradientStop { position: 0.0; color: control.fillColorStart }
                GradientStop { position: 1.0; color: control.fillColorEnd }
            }
            Behavior on width { NumberAnimation { duration: 300 } }
        }
    }
}
