import QtQuick
import Qt5Compat.GraphicalEffects
import MLC

// 复刻原版 MyCard（Controls/MyCard.vb）
// 圆角白卡片：投影、悬停高亮、可选标题栏折叠
// 内容通过默认属性放入，请使用可计算 implicitHeight 的容器（如 ColumnLayout，顶部锚定）
Item {
    id: card

    // ---- 公开 API ----
    default property alias content: contentItem.data
    property string title: ""
    property bool collapsible: false      // 原版 CanSwap：是否可折叠
    property bool collapsed: false        // 原版 IsSwapped：当前是否已折叠
    property bool swapLogoRight: false    // 折叠时箭头朝右而非朝下（原版 SwapLogoRight）
    property bool hasMouseAnimation: true // 悬停时投影/标题高亮（原版 HasMouseAnimation）
    property int contentMargin: Theme.contentMargin // 内容左右边距（原版页面约定 25）

    signal swap   // 用户点击标题栏切换折叠后触发（原版 Swap 事件）

    readonly property int collapsedHeight: 40  // 原版 SwapedHeight
    readonly property int _headerHeight: title !== "" ? 40 : 0
    readonly property bool hovered: hoverArea.containsMouse
    readonly property bool _hoverGlow: hovered && hasMouseAnimation

    implicitWidth: 200
    implicitHeight: collapsible && collapsed
                    ? collapsedHeight
                    : _headerHeight + contentItem.childrenRect.height + Theme.sectionSpacing

    // 高度变化动画（原版 UseAnimation：150ms 缓动）
    Behavior on implicitHeight {
        NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
    }

    // ---- 投影（原版 MyDropShadow：常态 color1/7%，悬停 color4/40%，90ms） ----
    DropShadow {
        anchors.fill: cardBg
        source: cardBg
        radius: 6
        samples: 13
        color: Qt.alpha(card._hoverGlow ? Theme.color4 : Theme.color1,
                        card._hoverGlow ? 0.4 : 0.07)
        Behavior on color { ColorAnimation { duration: 90 } }
    }

    // ---- 卡片本体（原版 CornerRadius=5，Theme 无对应圆角项故直接写 5；
    //       背景 F5FFFFFF ≈ 96% 白，由 pureWhite 派生） ----
    Rectangle {
        id: cardBg
        anchors.fill: parent
        radius: 5
        color: Qt.alpha(Theme.pureWhite, 245 / 255)
    }

    // ---- 标题（原版 MainTextBlock：粗体，Margin=15,12,0,0，悬停变 color2） ----
    Text {
        id: labTitle
        visible: card.title !== ""
        anchors { left: parent.left; top: parent.top; leftMargin: 15; topMargin: 12 }
        text: card.title
        color: card._hoverGlow ? Theme.color2 : Theme.color1
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
        font.bold: true
        Behavior on color { ColorAnimation { duration: 90 } }
    }

    // ---- 折叠箭头（原版 MainSwap：10×6，Margin=0,17,16,0，展开时旋转 180°） ----
    Canvas {
        id: swapArrow
        visible: card.collapsible
        width: 10
        height: 6
        anchors { right: parent.right; top: parent.top; rightMargin: 16; topMargin: 17 }
        rotation: card.collapsed ? (card.swapLogoRight ? 270 : 0) : 180
        Behavior on rotation {
            RotationAnimation { duration: 250; easing.type: Easing.OutCubic; direction: RotationAnimation.Shortest }
        }

        property color arrowColor: card._hoverGlow ? Theme.color2 : Theme.color1
        onArrowColorChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            ctx.fillStyle = arrowColor;
            ctx.beginPath();
            // 原版几何 M2,4 l-2,2 10,10 10,-10 -2,-2 -8,8 -8,-8 z 等比缩放到 10×6
            ctx.moveTo(1, 0);
            ctx.lineTo(0, 0.75);
            ctx.lineTo(5, 4.5);
            ctx.lineTo(10, 3.75);
            ctx.lineTo(9, 3);
            ctx.lineTo(5, 6);
            ctx.closePath();
            ctx.fill();
        }
    }

    // ---- 内容容器（折叠时随卡片高度裁剪） ----
    Item {
        id: contentItem
        clip: true
        anchors {
            left: parent.left; right: parent.right; top: parent.top
            leftMargin: card.contentMargin; rightMargin: card.contentMargin
            topMargin: card._headerHeight
        }
        height: Math.max(0, card.height - card._headerHeight)
        opacity: card.collapsible && card.collapsed ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 100 } }
    }

    // ---- 悬停检测（不拦截任何点击，事件穿透到内容） ----
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    // ---- 标题栏点击折叠（原版：展开时仅顶部 34px 触发，折叠后整卡可点） ----
    MouseArea {
        id: headerArea
        enabled: card.collapsible
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: card.collapsed ? card.collapsedHeight : 34
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            card.collapsed = !card.collapsed;
            card.swap();
        }
    }
}
