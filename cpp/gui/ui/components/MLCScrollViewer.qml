import QtQuick
import QtQuick.Controls.Basic
import MLC

// 复刻原版 MyScrollViewer（Controls/MyScrollViewer.vb）
// 滚动容器：自带 MLCScrollBar，滚轮接管为 300ms 平滑滚动动画
// 默认约定：内容宽度随视图（不横向滚动），高度由内容自动撑开；
// 内容请使用顶部锚定的容器（如 ColumnLayout），也可直接显式设置 contentHeight
Flickable {
    id: view

    // ---- 公开 API ----
    property real deltaMult: 1              // 滚轮滚动倍率（原版 DeltaMult）
    property bool wheelAnimationEnabled: true
    property alias scrollBar: vScroll       // 垂直滚动条实例（可调整 policy 等）

    contentWidth: width
    contentHeight: contentItem.childrenRect.height
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    flickableDirection: Flickable.VerticalFlick

    ScrollBar.vertical: MLCScrollBar { id: vScroll }

    // ---- 滚轮平滑滚动（原版 PerformVerticalOffsetDelta：300ms 缓动） ----
    property real _targetY: 0

    NumberAnimation {
        id: wheelAnim
        target: view
        property: "contentY"
        duration: 300
        easing.type: Easing.OutCubic
    }

    onDragStarted: wheelAnim.stop()   // 拖动时立即中断滚轮动画

    // 覆盖式 MouseArea：只接收滚轮，鼠标事件全部穿透
    // 位置跟随可视区域（y = contentY），避免撑大内容高度造成绑定循环
    MouseArea {
        id: wheelArea
        x: 0
        y: view.contentY
        width: view.width
        height: view.height
        acceptedButtons: Qt.NoButton
        onWheel: wheel => {
            // 内容不足一屏时不接管，事件继续传递（同原版 ScrollableHeight=0 直接返回）
            if (view.contentHeight <= view.height || view.height <= 0)
                return;
            let base = wheelAnim.running ? view._targetY : view.contentY;
            view._targetY = Math.max(0, Math.min(view.contentHeight - view.height,
                                                 base - wheel.angleDelta.y * view.deltaMult));
            if (view.wheelAnimationEnabled) {
                wheelAnim.to = view._targetY;
                wheelAnim.restart();
            } else {
                view.contentY = view._targetY;
            }
            wheel.accepted = true;
        }
    }
}
