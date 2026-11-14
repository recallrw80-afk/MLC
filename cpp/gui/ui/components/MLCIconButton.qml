import QtQuick
import MLC

// 复刻原版 MyIconButton（Controls/MyIconButton.xaml + .xaml.vb）
// 纯图标按钮：圆形背景，悬停变色，按下缩放回弹
Item {
    id: control

    // ---- 公开 API ----
    property string lucideIcon: ""        // lucide 图标名 → qrc:/gui/assets/icons/lucide/<name>.svg
    property url iconSource: ""           // 自定义图标 qrc 路径（lucideIcon 为空时使用）
    property real logoScale: 1.0          // 图标缩放（原版 LogoScale）
    property int theme: 0                 // 0=Color 1=White 2=Black 3=Red 4=Custom（原版 Theme 枚举）
    property color foreground: Theme.gray3 // theme=4 (Custom) 时的前景色（原版默认 128 灰，取 gray3 近似）
    property bool hovered: false
    property bool pressed: false

    signal clicked

    implicitWidth: 28
    implicitHeight: 28

    // ---- 颜色逻辑（对应原版 RefreshAnim） ----
    readonly property color iconColor: {
        switch (control.theme) {
        case 1: // White：图标恒为 color8，仅背景变化
            return Theme.color8;
        case 2: // Black：常态 alpha 160，悬停 230
            return Qt.alpha(Theme.gray1, control.hovered ? 230 / 255 : 160 / 255);
        case 3: // Red：常态 alpha 160，悬停不透明
            return Qt.alpha(Theme.redLight, control.hovered ? 1.0 : 160 / 255);
        case 4: // Custom：使用 foreground
            return Qt.alpha(control.foreground, control.hovered ? 1.0 : 160 / 255);
        default: // Color：常态 color4，悬停 color2
            return control.hovered ? Theme.color2 : Theme.color4;
        }
    }
    // 仅 White 主题悬停时显示圆形背景（原版为白色 alpha 50）
    readonly property color backColor: control.theme === 1 && control.hovered
                                       ? Qt.alpha(Theme.pureWhite, 50 / 255) : "transparent"

    // 按下 0.8 缩放，松开 OutBack 回弹（原版两段 ScaleTransform 动画的简化）
    scale: control.pressed ? 0.8 : 1.0
    Behavior on scale {
        NumberAnimation { duration: 250; easing.type: Easing.OutBack }
    }

    // ---- 背景圆（原版 PanBack，CornerRadius=1000） ----
    Rectangle {
        id: panBack
        anchors.fill: parent
        radius: width / 2
        color: control.backColor
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    // ---- 图标（原版 Path Margin=5，即图标区 = 整体 - 10） ----
    MLCIcon {
        id: icon
        anchors.centerIn: parent
        width: (control.width - 10) * control.logoScale
        height: (control.height - 10) * control.logoScale
        lucideIcon: control.lucideIcon
        iconSource: control.iconSource
        iconColor: control.iconColor
        Behavior on iconColor { ColorAnimation { duration: 120 } }
    }

    // ---- 鼠标处理 ----
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: control.enabled
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        onEntered: control.hovered = true
        onExited: {
            control.hovered = false;
            control.pressed = false;
        }
        onPressed: control.pressed = true
        onReleased: control.pressed = false
        onCanceled: control.pressed = false
        onClicked: control.clicked()
    }
}
