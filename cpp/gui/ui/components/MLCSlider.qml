import QtQuick
import QtQuick.Controls.Basic
import MLC

// 复刻原版 MySlider（Controls/MySlider.xaml + .xaml.vb）
// 拖动滑块：1px 未完成轨道 + 2px 完成轨道 + 10px 圆点，支持拖动与方向键
Item {
    id: slider

    // ---- 公开 API ----
    property int minValue: 0        // 原版固定为 0，这里做最小扩展
    property int maxValue: 100
    property int value: 0
    property int valueByKey: 1      // 方向键步长（原版 ValueByKey）
    property var getHintText: null  // 提示文本函数 (value) => string，为 null 时不显示气泡
    property bool hovered: false
    property bool dragging: false

    signal changed   // 值被拖动或按键改变（属性自带的 valueChanged 同样可用）

    implicitWidth: 162
    implicitHeight: 16

    // ---- 值与位置换算（原版：有效宽度 = 总宽 - 圆点宽 10） ----
    readonly property real _usableWidth: Math.max(0, width - 10)
    // 非 readonly：Behavior 需要可写属性；实际值由绑定维护
    property real _pos: maxValue > minValue
                        ? (value - minValue) / (maxValue - minValue) * _usableWidth : 0

    // 轨道与圆点位置平滑过渡（原版 AaWidth/AaX 动画），拖动时立即跟随；
    // 动画作用于位置而非值，保证 value 赋值与钳制即时生效
    Behavior on _pos {
        NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
        enabled: !slider.dragging
    }

    function _clampValue() {
        let clamped = Math.min(maxValue, Math.max(minValue, value));
        if (clamped !== value)
            value = clamped;
    }

    function _setValueFromX(x) {
        let ratio = Math.min(1, Math.max(0, (x - 5) / _usableWidth));
        let newValue = Math.round(minValue + ratio * (maxValue - minValue));
        if (newValue !== value) {
            value = newValue;
            changed();
        }
    }

    function _stepBy(delta) {
        let newValue = Math.min(maxValue, Math.max(minValue, value + delta));
        if (newValue !== value) {
            value = newValue;
            changed();
            if (getHintText !== null)
                keyHintTimer.restart();   // 按键改变时短暂显示气泡（原版 700ms）
        }
    }

    // 用 Connections 做值域钳制，避免占用实例的 onValueChanged 处理器
    Connections {
        target: slider
        function onValueChanged() { slider._clampValue(); }
        function onMaxValueChanged() { slider._clampValue(); }
        function onMinValueChanged() { slider._clampValue(); }
    }
    // 初始赋值早于 Connections 建立，完成后补一次钳制
    Component.onCompleted: slider._clampValue()

    // ---- 颜色逻辑（对应原版 RefreshColor） ----
    readonly property color _foreColor: {
        if (!slider.enabled) return Theme.gray5;
        if (slider.dragging || slider.hovered) return Theme.color3;
        return Theme.colorBg0;
    }

    // ---- 未完成轨道（原版 LineBack：1px，colorBg0 透明度 0.3） ----
    Rectangle {
        anchors { right: parent.right; rightMargin: 1; verticalCenter: parent.verticalCenter }
        width: slider._usableWidth - slider._pos
        height: 1
        color: Theme.colorBg0
        opacity: 0.3
    }

    // ---- 已完成轨道（原版 LineFore：2px，颜色随状态） ----
    Rectangle {
        anchors { left: parent.left; leftMargin: 1; verticalCenter: parent.verticalCenter }
        width: slider._pos
        height: 2
        color: slider._foreColor
        Behavior on color { ColorAnimation { duration: 100 } }
    }

    // ---- 圆点（原版 ShapeDot：10×10，拖动时放大 1.3） ----
    Rectangle {
        id: shapeDot
        x: slider._pos
        anchors.verticalCenter: parent.verticalCenter
        width: 10
        height: 10
        radius: 5
        border.width: 1
        border.color: slider._foreColor
        color: slider._foreColor
        scale: slider.dragging ? 1.3 : 1.0
        Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
        Behavior on color { ColorAnimation { duration: 100 } }
        Behavior on border.color { ColorAnimation { duration: 100 } }
    }

    // ---- 提示气泡（原版 Popup + TextHint，圆点下方） ----
    Popup {
        id: hintPopup
        parent: shapeDot
        x: (10 - width) / 2
        y: 24
        visible: slider.getHintText !== null && (slider.dragging || keyHintTimer.running)
        closePolicy: Popup.NoAutoClose
        padding: 0
        background: Rectangle {
            radius: 3
            border.width: 1
            border.color: Theme.color1
            color: Theme.pureWhite
        }
        contentItem: Text {
            text: slider.getHintText !== null ? slider.getHintText(slider.value) : ""
            color: Theme.color1
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
            leftPadding: 7
            rightPadding: 7
            topPadding: 5
            bottomPadding: 5
        }
    }

    Timer { id: keyHintTimer; interval: 700 }

    // ---- 拖动与按键 ----
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: slider.enabled
        hoverEnabled: true
        preventStealing: true
        onEntered: {
            slider.hovered = true;
            slider.forceActiveFocus();   // 原版 MouseEnter 时 Focus 以接收方向键
        }
        onExited: slider.hovered = false
        onPressed: mouse => {
            slider.dragging = true;
            slider._setValueFromX(mouse.x);
        }
        onPositionChanged: mouse => {
            if (slider.dragging)
                slider._setValueFromX(mouse.x);
        }
        onReleased: slider.dragging = false
        onCanceled: slider.dragging = false
    }

    Keys.onLeftPressed: event => {
        slider._stepBy(-slider.valueByKey);
        event.accepted = true;
    }
    Keys.onRightPressed: event => {
        slider._stepBy(slider.valueByKey);
        event.accepted = true;
    }
}
