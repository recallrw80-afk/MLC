import QtQuick
import MLC

// 复刻原版 MyListItem（Controls/MyListItem.xaml + .xaml.vb）
// 列表项：左侧勾选竖条 + 图标 + 标题/副标题 + 右侧悬停按钮区
Item {
    id: item

    // ---- 公开 API ----
    property string title: ""
    property string info: ""                 // 副标题（原版 Info）
    property alias font: labTitle.font       // 标题字体（原版默认 FontSize=14）
    property color foreground: Theme.color1  // 标题前景色（原版 Foreground）
    property string lucideIcon: ""           // 左侧矢量图标（替代原版 Logo 的 Path）
    property url imageSource: ""             // 左侧位图图标（替代原版 Logo 的 png/jpg）
    property color iconColor: item._titleColor // 矢量图标着色（原版绑定 Foreground，随勾选变色）
    property real logoScale: 1.0
    property bool logoClickable: false
    property int paddingLeft: 4
    property int minPaddingRight: 4
    property int checkType: 0                // 0=None 1=Clickable 2=RadioBox 3=CheckBox
    property bool checked: false
    property bool scaleAnimationEnabled: true
    property alias buttons: buttonRow.data   // 右侧按钮列表（悬停显示），一般放 MLCIconButton
    property bool hovered: false
    property bool pressed: false

    signal clicked
    signal logoClicked
    signal toggled(bool checked)   // 用户点击导致 checkType>=2 的勾选变化

    implicitWidth: 200
    implicitHeight: 42

    readonly property bool _clickable: checkType !== 0
    readonly property bool _hasIcon: lucideIcon !== "" || imageSource.toString() !== ""
    readonly property bool _hasButtons: buttonRow.children.length > 0
    // 勾选时标题与图标变主题色（原版：高度 <40 用 color3，否则 color2）
    readonly property color _titleColor: checked
                                         ? (height < 40 ? Theme.color3 : Theme.color2) : foreground

    // 按下时整体轻微缩放（原版 IsScaleAnimationEnabled 的 0.98）
    scale: item.scaleAnimationEnabled && item.pressed ? 0.98 : 1.0
    Behavior on scale {
        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
    }

    // ---- 悬停背景（原版 RectBack：常态 color7，按下 color6，缩放淡入） ----
    Rectangle {
        id: rectBack
        anchors.fill: parent
        radius: item.scaleAnimationEnabled || item.height > 40 ? 6 : 0
        border.width: 1
        border.color: Theme.color6
        // ColorBrushBg1 = color7 加 190/255 透明度（ModSecret.ThemeRefresh）
        color: item.pressed && item._clickable ? Theme.color6 : Qt.alpha(Theme.color7, 190 / 255)
        opacity: item.hovered ? 1 : 0
        scale: item.scaleAnimationEnabled ? (item.hovered ? 1 : 0.8) : 1
        Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        Behavior on color { ColorAnimation { duration: 120 } }
        Behavior on scale { NumberAnimation { duration: 190; easing.type: Easing.OutCubic } }
    }

    // ---- 左侧勾选竖条（原版 RectCheck：宽 5 圆角 2，勾选时展开） ----
    Rectangle {
        id: rectCheck
        visible: item.checkType >= 2
        anchors { left: parent.left; leftMargin: -1; verticalCenter: parent.verticalCenter }
        width: 5
        radius: 2
        color: Theme.color3
        height: item.checked ? item.height - 12 : 0
        opacity: item.checked ? 1 : 0
        Behavior on height { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }
        Behavior on opacity { NumberAnimation { duration: 70 } }
    }

    // ---- 点击处理（checkType=0 时仅提供悬停效果，不响应点击） ----
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: item.enabled
        hoverEnabled: true
        onEntered: item.hovered = true
        onExited: {
            item.hovered = false;
            item.pressed = false;
        }
        onPressed: if (item._clickable) item.pressed = true
        onReleased: item.pressed = false
        onCanceled: item.pressed = false
        onClicked: {
            if (!item._clickable)
                return;
            if (item.checkType === 2 && !item.checked) {
                // RadioBox：只能勾上，同组互斥由使用方管理
                item.checked = true;
                item.toggled(true);
            } else if (item.checkType === 3) {
                item.checked = !item.checked;
                item.toggled(item.checked);
            }
            item.clicked();
        }
    }

    // ---- 左侧图标区（原版 ColumnLogo 宽 34，图标约 24px 居中） ----
    Item {
        id: logoContainer
        visible: item._hasIcon
        anchors { left: parent.left; leftMargin: item.paddingLeft; top: parent.top; bottom: parent.bottom }
        width: item._hasIcon ? 34 : 0

        MLCIcon {
            anchors.centerIn: parent
            visible: item.lucideIcon !== ""
            width: 24 * item.logoScale
            height: 24 * item.logoScale
            lucideIcon: item.lucideIcon
            iconColor: item.iconColor
            Behavior on iconColor { ColorAnimation { duration: 120 } }
        }
        Image {
            anchors.centerIn: parent
            visible: item.lucideIcon === "" && item.imageSource.toString() !== ""
            width: 24 * item.logoScale
            height: 24 * item.logoScale
            source: item.imageSource
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
        }

        // 图标单独点击（原版 LogoClickable / LogoClick）
        MouseArea {
            anchors.fill: parent
            enabled: item.logoClickable
            cursorShape: item.logoClickable ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: item.logoClicked()
        }
    }

    // ---- 标题与副标题 ----
    Column {
        id: textColumn
        anchors {
            left: logoContainer.right; leftMargin: 4
            right: parent.right
            // 悬停时为按钮区让位（原版 ColumnPaddingRight 动画：5 + 按钮宽）
            rightMargin: item._hasButtons && item.hovered
                         ? buttonRow.width + 10 : item.minPaddingRight
            verticalCenter: parent.verticalCenter
        }
        Behavior on anchors.rightMargin { NumberAnimation { duration: 120 } }

        Text {
            id: labTitle
            width: parent.width
            text: item.title
            color: item._titleColor
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeLarge
            elide: Text.ElideRight
            Behavior on color { ColorAnimation { duration: 150 } }
        }
        Text {
            id: labInfo
            visible: item.info !== ""
            width: parent.width
            text: item.info
            color: Theme.gray2
            opacity: 0.6
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
        }
    }

    // ---- 右侧按钮区（原版 ButtonStack：悬停淡入，平时隐藏） ----
    Row {
        id: buttonRow
        anchors { right: parent.right; rightMargin: 5; verticalCenter: parent.verticalCenter }
        spacing: 0
        opacity: item.hovered ? 1 : 0
        visible: opacity > 0
        enabled: item.hovered
        Behavior on opacity { NumberAnimation { duration: 90 } }
    }
}
