pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Qt5Compat.GraphicalEffects
import MLC

// 消息弹窗层（对应原版 ModMain.MyMsgBox / MyMsgText + FormMain.PanMsg）
// 模态对话框：半透明遮罩覆盖整个窗口，居中卡片带滑入/旋转动画，一次一个、其余排队
// 用法：panMsg.show({ title, text, warn, button1, button2, button3, callback })
Item {
    id: msgLayer

    visible: false

    // ---- 对外 API ----

    // 弹出消息框；options 字段（均可选）：
    //   title    标题，默认“提示”
    //   text     正文内容
    //   warn     警告样式（红色标题/按钮/遮罩），默认 false
    //   button1  第一个按钮文本，默认“确定”
    //   button2  第二个按钮文本，默认空（不显示）
    //   button3  第三个按钮文本，默认空（不显示）
    //   callback 点击按钮后的回调，参数为按钮序号（1/2/3）
    // 多个弹窗请求会排队依次显示（对应原版 WaitingMyMsgBox）
    function show(options) {
        _queue.push(options || {});
        if (!_active)
            _showNext();
    }

    // ---- 内部状态 ----

    property var _queue: []
    property bool _active: false

    // 遮罩目标色（原版：普通 35% 黑 / 警告 55% 暗红，改用 Theme 色推导）
    readonly property color _dimNormal: Qt.rgba(Theme.color1.r, Theme.color1.g, Theme.color1.b, 0.35)
    readonly property color _dimWarn: Qt.rgba(Theme.redDark.r * 0.35, Theme.redDark.g * 0.35, Theme.redDark.b * 0.35, 0.55)

    function _showNext() {
        if (_queue.length === 0) {
            // 队列清空：遮罩淡出后隐藏整层（对应原版无等待弹窗时的 PanMsg 渐隐）
            _active = false;
            dim.color = "transparent";
            hideLayerTimer.start();
            return;
        }
        _active = true;
        msgLayer.visible = true;
        hideLayerTimer.stop();

        var opt = _queue.shift();
        // 卡片创建后自动播放入场动画，退出动画播完会回调 _showNext() 显示下一个
        var card = msgCardComp.createObject(cardHost, {
            "title": opt.title !== undefined ? String(opt.title) : "提示",
            "text": opt.text !== undefined ? String(opt.text) : "",
            "warn": opt.warn === true,
            "button1": opt.button1 !== undefined ? String(opt.button1) : "确定",
            "button2": opt.button2 !== undefined ? String(opt.button2) : "",
            "button3": opt.button3 !== undefined ? String(opt.button3) : "",
            "callback": opt.callback !== undefined ? opt.callback : null
        });
        if (card === null)
            return;
        dim.color = opt.warn === true ? _dimWarn : _dimNormal;
    }

    // 遮罩淡出计时（原版延迟 30ms 起播 200ms 渐隐，之后再隐藏整层）
    Timer {
        id: hideLayerTimer
        interval: 250
        repeat: false
        onTriggered: msgLayer.visible = false
    }

    // ---- 遮罩（对应 PanMsg.Background）----

    Rectangle {
        id: dim
        anchors.fill: parent
        color: "transparent"

        Behavior on color {
            ColorAnimation {
                duration: 200
            }
        }
    }

    // 模态阻断：吞掉遮罩上的所有鼠标与悬停事件
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
    }

    // ---- 卡片宿主 ----

    Item {
        id: cardHost
        anchors.fill: parent
    }

    // ---- 弹窗卡片（对应 MyMsgText：标题 + 下划线 + 正文 + 按钮）----

    Component {
        id: msgCardComp

        Item {
            id: card

            // ---- 创建参数 ----
            property string title: "提示"
            property string text: ""
            property bool warn: false
            property string button1: "确定"
            property string button2: ""
            property string button3: ""
            property var callback: null
            property bool _exited: false

            // 原版 Grid Margin=25 / MinWidth=400，宽度按内容自适应
            readonly property real _maxWidth: msgLayer.width - 50
            readonly property real _maxCaptionHeight: Math.max(40, msgLayer.height - 50 - 45 - (titleLabel.implicitHeight + 8) - 2 - 13 - 17 - 28)

            width: Math.min(Math.max(400, btnRow.implicitWidth + 202, titleLabel.implicitWidth + 121, Math.min(captionMetrics.advanceWidth + 66, _maxWidth)), _maxWidth)
            height: contentCol.height + 45
            anchors.centerIn: parent
            opacity: 0

            // 初始姿态：下沉 40px、以左缘中点为轴倾斜 -4°（对应原版 RenderTransform）
            transform: [
                Translate {
                    id: cardSlide
                    y: 40
                },
                Rotation {
                    id: cardTilt
                    angle: -4
                    origin.x: 0
                    origin.y: card.height / 2
                }
            ]

            // 创建后自动播放入场动画并接管键盘焦点（对应原版 Load + Btn1.Focus）
            Component.onCompleted: {
                enterAnim.start();
                forceActiveFocus();
            }

            // 点击按钮：先回调再关闭（对应原版 BtnX_Click → Close 解除阻塞）
            function accept(result) {
                if (_exited)
                    return;
                _exited = true;
                if (callback)
                    callback(result);
                exitAnim.start();
            }

            Keys.onReturnPressed: event => card.accept(1)
            Keys.onEnterPressed: event => card.accept(1)
            Keys.onEscapePressed: event => {
                if (card.button2 !== "")
                    card.accept(2);
            }

            // ---- 入场动画（对应原版 Load：淡入 + 上移 + 回正，延迟 60ms）----

            SequentialAnimation {
                id: enterAnim
                PauseAnimation {
                    duration: 60
                }
                ParallelAnimation {
                    NumberAnimation {
                        target: card
                        property: "opacity"
                        to: 1
                        duration: 120
                    }
                    NumberAnimation {
                        target: cardSlide
                        property: "y"
                        to: 0
                        duration: 300
                        easing.type: Easing.OutBack
                    }
                    NumberAnimation {
                        target: cardTilt
                        property: "angle"
                        to: 0
                        duration: 300
                        easing.type: Easing.OutCubic
                    }
                }
            }

            // ---- 退出动画（对应原版 Close：淡出 + 下沉 20 + 顺倾 6°）----

            SequentialAnimation {
                id: exitAnim
                ParallelAnimation {
                    SequentialAnimation {
                        PauseAnimation {
                            duration: 20
                        }
                        NumberAnimation {
                            target: card
                            property: "opacity"
                            to: 0
                            duration: 80
                        }
                    }
                    NumberAnimation {
                        target: cardSlide
                        property: "y"
                        to: 20
                        duration: 150
                        easing.type: Easing.OutCubic
                    }
                    NumberAnimation {
                        target: cardTilt
                        property: "angle"
                        to: 6
                        duration: 150
                        easing.type: Easing.InCubic
                    }
                }
                ScriptAction {
                    script: {
                        // 通知弹窗层显示队列中的下一个（无队列时遮罩淡出），随后销毁自身
                        msgLayer._showNext();
                        card.destroy();
                    }
                }
            }

            // 正文自然宽度测量（用于卡片宽度自适应）
            TextMetrics {
                id: captionMetrics
                text: card.text
                font: captionText.font
            }

            // 卡片阴影（对应原版 DropShadowEffect：Color1、模糊 20、深度 4、不透明 0.8）
            DropShadow {
                anchors.fill: cardBg
                source: cardBg
                radius: 20
                samples: 41
                verticalOffset: 4
                color: Qt.rgba(Theme.color1.r, Theme.color1.g, Theme.color1.b, 0.8)
                transparentBorder: true
            }

            // ---- 卡片主体（对应 PanBorder：圆角 7、近白底）----

            Rectangle {
                id: cardBg
                anchors.fill: parent
                radius: 7
                color: Theme.pureWhite
            }

            Column {
                id: contentCol
                x: 22
                y: 22
                width: card.width - 44
                spacing: 0

                // 标题（对应 LabTitle：23px、主题蓝、警告时红色，左边距 7 右边距 70 下边距 9）
                Item {
                    width: contentCol.width
                    height: titleLabel.implicitHeight + 8

                    Text {
                        id: titleLabel
                        x: 7
                        y: -1
                        width: parent.width - 77
                        text: card.title
                        color: card.warn ? Theme.redLight : Theme.color2
                        font.family: Theme.fontFamily
                        font.pixelSize: 23
                        elide: Text.ElideRight
                    }
                }

                // 标题下划线（对应 ShapeLine：2px、颜色同标题）
                Rectangle {
                    width: contentCol.width
                    height: 2
                    color: titleLabel.color
                }

                Item {
                    width: 1
                    height: 13
                }

                // 正文（对应 PanCaption + LabCaption：15px、行高 18、超长可滚动）
                Flickable {
                    id: captionFlick
                    width: contentCol.width
                    height: Math.min(captionText.contentHeight, card._maxCaptionHeight)
                    contentWidth: width
                    contentHeight: captionText.contentHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: MLCScrollBar {}

                    Text {
                        id: captionText
                        x: 7
                        width: captionFlick.width - 22
                        text: card.text
                        color: Theme.gray1
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        lineHeight: 18
                        lineHeightMode: Text.FixedHeight
                        wrapMode: Text.WordWrap
                    }
                }

                Item {
                    width: 1
                    height: 17
                }

                // 按钮区（对应 PanBtn：右对齐、左留白 150、右留白 8、间距 12）
                Item {
                    width: contentCol.width
                    height: 28

                    Row {
                        id: btnRow
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        spacing: 12

                        MLCButton {
                            id: btn1
                            text: card.button1
                            // 警告 → 红色；有第二按钮 → 高亮；否则普通（对应原版 Load 逻辑）
                            colorType: card.warn ? 2 : (btn2.visible ? 1 : 0)
                            onClicked: card.accept(1)
                        }
                        MLCButton {
                            id: btn2
                            text: card.button2
                            visible: card.button2 !== ""
                            onClicked: card.accept(2)
                        }
                        MLCButton {
                            id: btn3
                            text: card.button3
                            visible: card.button3 !== ""
                            onClicked: card.accept(3)
                        }
                    }
                }
            }
        }
    }
}
