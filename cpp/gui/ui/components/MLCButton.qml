import QtQuick
import QtQuick.Controls.Basic
import MLC

// Exact replica of original MyButton (Controls/MyButton.xaml + .xaml.vb)
// Structure: PanBack(semiTransparent) > PanFore(border+halfWhite bg) > LabText
Item {
    id: wrapper

    // ---- Public API ----
    property alias text: labText.text
    property alias font: labText.font
    // enabled — inherited from Item; propagate to child MouseArea
    onEnabledChanged: mouseArea.enabled = enabled
    Component.onCompleted: mouseArea.enabled = enabled
    property alias contentItem: contentLoader.sourceComponent
    property int colorType: 0           // 0=Normal, 1=Highlight, 2=Red, 3=Disabled
    property int padding: 10
    property bool down: false
    property bool pressed: false
    property real radius: Theme.buttonRadius
    property bool hasBorder: true        // false → borderless (background only)

    // state: -1=auto (mouse-driven), 0=Normal, 1=Hover, 2=Pressed, 3=Disabled
    // Set >= 0 to lock state externally; in auto mode the MouseArea updates it.
    property int state: -1
    readonly property int effectiveState: state >= 0 ? state : internalState
    property int internalState: 0

    signal clicked
    signal pressAndHold

    implicitWidth: {
        let contentW = contentLoader.item ? contentLoader.item.implicitWidth : 0;
        let base = Math.max(labText.implicitWidth, contentW) + wrapper.padding * 2;
        return Math.max(60, base);
    }
    implicitHeight: 28

    // ---- Color logic (exact match original code-behind) ----
    property color borderColor: {
        if (effectiveState === 3 || wrapper.colorType === 3 || !enabled)
            return Theme.gray4;
        if (wrapper.colorType === 1) {
            // Filled (Highlight): border = fill color
            if (hovered)
                return Theme.color3;
            return Theme.color2;
        }
        if (hovered) {
            if (wrapper.colorType === 2)
                return Theme.redLight;
            return Theme.color3;
        }
        if (wrapper.colorType === 2)
            return Theme.redDark;
        return Theme.color2;
    }
    property color backgroundColor: {
        if (effectiveState === 3 || wrapper.colorType === 3 || !enabled)
            return Theme.gray6;
        if (wrapper.colorType === 1) {
            // Filled style — solid blue, white text
            if (hovered)
                return Theme.color3;
            return Theme.color2;
        }
        if (hovered) {
            if (wrapper.colorType === 2)
                return Theme.redBack;
            return Theme.color7;
        }
        return Theme.halfWhite;
    }
    readonly property color labelColor: colorType === 1 ? Theme.pureWhite : wrapper.borderColor
    property bool hovered: false

    // ---- Press scale animation (exact match original ScaleTransform) ----
    property real btnScale: effectiveState === 2 ? 0.95 : 1.0

    Behavior on btnScale {
        NumberAnimation {
            duration: 300
            easing.type: Easing.OutBack
        }
    }

    transform: Scale {
        origin.x: wrapper.width / 2
        origin.y: wrapper.height / 2
        xScale: wrapper.btnScale
        yScale: wrapper.btnScale
    }

    // ---- PanBack (outer border, semiTransparent background) ----
    Rectangle {
        id: panBack
        anchors.fill: parent
        color: Theme.semiTransparent
        radius: wrapper.radius

        // ---- PanFore (inner border, colored border + halfWhite background) ----
        Rectangle {
            id: panFore
            anchors.fill: parent
            radius: wrapper.radius
            border.width: wrapper.hasBorder ? 1 : 0
            border.color: wrapper.borderColor
            color: wrapper.backgroundColor

            Behavior on border.color {
                ColorAnimation {
                    duration: 100
                }
            }
            Behavior on color {
                ColorAnimation {
                    duration: 100
                }
            }

            // ---- Content area (inset by padding) ----
            Item {
                id: contentArea
                anchors.fill: parent
                anchors.margins: wrapper.padding

                // ---- LabText (text color = border color, matching original) ----
                Text {
                    id: labText
                    anchors.centerIn: parent
                    visible: text !== "" && contentLoader.item === null
                    color: wrapper.labelColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSize
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                // ---- Custom content loader (replaces labText when set) ----
                Loader {
                    id: contentLoader
                    anchors.centerIn: parent
                }
            }
        }
    }

    // ---- Mouse handling ----
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: wrapper.enabled   // follow Item.enabled instead of shadowing it
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        onEntered: {
            wrapper.hovered = true;
            wrapper.internalState = 1;
        }
        onExited: {
            wrapper.hovered = false;
            wrapper.down = false;
            wrapper.pressed = false;
            wrapper.internalState = 0;
        }
        onPressed: {
            wrapper.down = true;
            wrapper.pressed = true;
            wrapper.internalState = 2;
        }
        onReleased: {
            wrapper.down = false;
            wrapper.pressed = false;
            wrapper.internalState = wrapper.hovered ? 1 : 0;
        }
        onCanceled: {
            wrapper.down = false;
            wrapper.pressed = false;
            wrapper.internalState = 0;
        }
        onClicked: wrapper.clicked()
        onPressAndHold: wrapper.pressAndHold()
    }
}
