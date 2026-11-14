import QtQuick
import MLC

// 复刻原版 MyCheckBox（Controls/MyCheckBox.xaml + .xaml.vb）
// 勾选框 + 文字：18px 圆角方框，勾号 OutBack 缩放动画
Item {
    id: box

    // ---- 公开 API ----
    property alias text: labText.text
    property alias font: labText.font
    property bool checked: false
    property bool hovered: false
    property bool pressed: false

    signal clicked
    signal changed(bool byUser)   // 勾选状态改变（原版 Change 事件，byUser=是否用户操作）

    implicitWidth: Math.max(20, 26 + labText.implicitWidth)
    implicitHeight: Math.max(20, labText.implicitHeight)

    // ---- 颜色逻辑（对应原版 SyncUI / 指向动画） ----
    readonly property color _borderColor: {
        if (!box.enabled) return Theme.gray5;
        if (box.hovered) return Theme.color3;
        return box.checked ? Theme.color2 : Theme.color1;
    }
    readonly property color _textColor: {
        if (!box.enabled) return Theme.gray5;
        return box.hovered ? Theme.color3 : Theme.color1;
    }
    // 按下时底色为 ColorBrushBg1（color7 加 190/255 透明度）
    readonly property color _bgColor: box.pressed
                                      ? Qt.alpha(Theme.color7, 190 / 255) : Theme.halfWhite

    // ---- 勾选框（原版 ShapeBorder：18×18 圆角 3，Margin=1） ----
    Rectangle {
        id: shapeBorder
        anchors { left: parent.left; leftMargin: 1; verticalCenter: parent.verticalCenter }
        width: 18
        height: 18
        radius: 3
        border.width: 1
        border.color: box._borderColor
        color: box._bgColor
        scale: box.pressed ? 0.92 : 1.0
        Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
        Behavior on border.color { ColorAnimation { duration: 100 } }
        Behavior on color { ColorAnimation { duration: 100 } }
    }

    // ---- 勾号（原版 ShapeCheck：12×12，Margin=4，缩放动画） ----
    Canvas {
        id: shapeCheck
        anchors { left: parent.left; leftMargin: 4; verticalCenter: parent.verticalCenter }
        width: 12
        height: 12
        scale: box.checked ? 1 : 0
        Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }

        property color checkColor: box._borderColor
        onCheckColorChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            ctx.fillStyle = checkColor;
            ctx.beginPath();
            // 原版几何 M0,6 L1.5,4.5 4.5,7.5 10.5,1.5 12,3 4.5,10.5 z
            ctx.moveTo(0, 6);
            ctx.lineTo(1.5, 4.5);
            ctx.lineTo(4.5, 7.5);
            ctx.lineTo(10.5, 1.5);
            ctx.lineTo(12, 3);
            ctx.lineTo(4.5, 10.5);
            ctx.closePath();
            ctx.fill();
        }
    }

    // ---- 文本（原版 LabText：Margin=26,0,0,0） ----
    Text {
        id: labText
        anchors { left: parent.left; leftMargin: 26; verticalCenter: parent.verticalCenter }
        color: box._textColor
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
        Behavior on color { ColorAnimation { duration: 100 } }
    }

    // ---- 点击处理 ----
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: box.enabled
        hoverEnabled: true
        onEntered: box.hovered = true
        onExited: {
            box.hovered = false;
            box.pressed = false;
        }
        onPressed: box.pressed = true
        onReleased: box.pressed = false
        onCanceled: box.pressed = false
        onClicked: {
            // 标记本次切换来自用户点击，供 changed(byUser) 判定
            box._fromUser = true;
            box.checked = !box.checked;
            box._fromUser = false;
            box.clicked();
        }
    }

    property bool _fromUser: false

    // 用 Connections 监听勾选变化，避免占用实例的 onCheckedChanged 处理器
    Connections {
        target: box
        function onCheckedChanged() { box.changed(box._fromUser); }
    }
}
