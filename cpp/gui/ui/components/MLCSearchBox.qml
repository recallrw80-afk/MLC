import QtQuick
import QtQuick.Controls.Basic
import MLC

// 复刻原版 MySearchBox（Controls/MySearchBox.xaml + .xaml.vb）
// 搜索框：40px 卡片外观（继承 MLCCard），左侧放大镜，右侧清空按钮
MLCCard {
    id: box

    // ---- 公开 API ----
    property alias text: field.text
    property alias hintText: field.placeholderText
    readonly property alias input: field   // 内部 TextField，便于设置 validator 等

    signal accepted   // 回车触发（原版 Enter → RaiseCustomEvent）

    implicitWidth: 260
    implicitHeight: 40
    contentMargin: 0

    // ---- 输入框（原版 MyTextBox：无边框无背景，Padding=32,0,40,0） ----
    TextField {
        id: field
        anchors.fill: parent
        maximumLength: 50
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
        color: enabled ? Theme.color1 : Theme.gray4
        selectionColor: Theme.color3
        selectedTextColor: Theme.pureWhite
        verticalAlignment: TextInput.AlignVCenter
        leftPadding: 32
        rightPadding: 40
        placeholderTextColor: Theme.gray4
        background: Item { }
        onAccepted: box.accepted()
        Component.onCompleted: forceActiveFocus()   // 原版 Loaded 时聚焦输入框
    }

    // ---- 放大镜图标（原版左侧 Path：14×14，Margin=14,0,0,0，ColorBrush1） ----
    Canvas {
        id: searchIcon
        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
        width: 14
        height: 14

        property color iconColor: Theme.color1
        onIconColorChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            ctx.strokeStyle = iconColor;
            ctx.lineWidth = 1.6;
            ctx.lineCap = "round";
            ctx.beginPath();
            ctx.arc(5.5, 5.5, 4.5, 0, Math.PI * 2);
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(9, 9);
            ctx.lineTo(13, 13);
            ctx.stroke();
        }
    }

    // ---- 清空按钮（原版 BtnClear：24×24，Theme=Black，有文本时淡入） ----
    MLCIconButton {
        id: btnClear
        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
        width: 24
        height: 24
        lucideIcon: "x"
        theme: 2
        logoScale: 0.75
        opacity: field.text !== "" ? 1 : 0
        visible: opacity > 0
        enabled: field.text !== ""
        Behavior on opacity { NumberAnimation { duration: 90 } }
        onClicked: {
            field.text = "";
            field.forceActiveFocus();
        }
    }
}
