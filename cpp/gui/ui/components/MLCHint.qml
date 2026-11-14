pragma ComponentBehavior: Bound
import QtQuick
import MLC

// 轻提示层（对应原版 ModMain.Hint / HintTick + FormMain.PanHint）
// 在窗口左下角滑入提示条，停留一段时间后向左滑出并自动消失
// 用法：panHint.show("提示文本", "info" | "success" | "error")
Item {
    id: hintLayer

    // ---- 对外 API ----

    // 弹出一条轻提示；type 支持 info（蓝）/ success（绿）/ error（红），默认 info
    // 与原版一致：相同文本的提示不重复堆叠，而是抖动已有条目并重新计时
    function show(text, type) {
        // 原版会去掉回车换行
        var clean = (text === undefined || text === null) ? "" : String(text).replace(/[\r\n]+/g, " ");
        var typeId = 0;
        if (type === "success")
            typeId = 1;
        else if (type === "error" || type === "fail")
            typeId = 2;

        // 重复提示：抖动已有条目并重新计时（对应原版 DoubleStack 分支）；
        // 入场动画未播完的重复提示直接忽略（对应原版 AniIsRun 检查）
        for (var i = 0; i < liveHints.length; i++) {
            var old = liveHints[i];
            if (old.canReuse && old.hintText === clean) {
                if (old.showDone)
                    old.replay();
                return;
            }
        }
        // 超量提示直接忽略（原版上限 20 条）
        if (liveHints.length >= 20)
            return;

        var hint = hintComp.createObject(hintColumn, {
            "hintText": clean,
            "hintType": typeId,
            "expandOnShow": liveHints.length > 0
        });
        if (hint === null)
            return;
        liveHints.push(hint);
    }

    // ---- 内部状态 ----

    // 存活提示列表（含正在播退出动画的，与原版 PanHint.Children 口径一致）
    property var liveHints: []

    function _remove(item) {
        var idx = liveHints.indexOf(item);
        if (idx >= 0)
            liveHints.splice(idx, 1);
    }

    // ---- 提示条容器（对应 PanHint：左下角、新提示把旧提示向上顶、不响应鼠标）----

    Column {
        id: hintColumn
        anchors {
            left: parent.left
            bottom: parent.bottom
            bottomMargin: 20
        }
        spacing: 0
    }

    // ---- 单条提示（对应 HintTick 中动态创建的 Border）----
    // 外层槽位负责高度动画与裁剪，内层视觉条负责左右滑动，避免 Column 接管 x 坐标

    Component {
        id: hintComp

        Item {
            id: hintSlot

            // ---- 创建参数 ----
            property string hintText: ""
            property int hintType: 0        // 0=信息 1=成功 2=失败
            property bool expandOnShow: true
            property bool canReuse: true
            property bool showDone: false

            // 颜色混合比例：0.3（混入白色）→ 1.0（目标色），入场时颜色逐渐加深
            property real mix: 0.3

            width: hintVisual.width + 70
            height: expandOnShow ? 0 : 26
            clip: true

            // ---- 配色（对应原版 HintType 渐变，透明度 215/255）----
            // 成功绿：原版 rgb(33,177,33)→rgb(29,160,29)；Theme 无绿色，经 Theme.hsl 推导
            readonly property color _target0: hintType === 1 ? Theme.hsl(120, 69, 41) : hintType === 2 ? Theme.redLight : Theme.color3
            readonly property color _target1: hintType === 1 ? Theme.hsl(120, 69, 37) : hintType === 2 ? Theme.redDark : Theme.color2

            function _mixColor(target) {
                var p = mix;
                return Qt.rgba(target.r * p + (1 - p), target.g * p + (1 - p), target.b * p + (1 - p), (215 * p + 255 * (1 - p)) / 255);
            }

            readonly property color gradColor0: _mixColor(_target0)
            readonly property color gradColor1: _mixColor(_target1)

            // ---- 视觉条（x 槽位坐标 = 窗口坐标 + 70：初始 0、静止 50，对应窗口 -70/-20）----

            Item {
                id: hintVisual
                anchors.bottom: parent.bottom
                width: hintLabel.implicitWidth + 41   // 左留白 33 + 右留白 8
                height: 26
                x: 0
                opacity: 0

                // 提示条主体（圆角 6）
                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    gradient: Gradient {
                        GradientStop {
                            position: 0
                            color: hintSlot.gradColor0
                        }
                        GradientStop {
                            position: 1
                            color: hintSlot.gradColor1
                        }
                    }
                }

                // 左侧直角补丁（原版 CornerRadius 0,6,6,0：贴窗口左缘的一侧为直角）
                Rectangle {
                    anchors {
                        left: parent.left
                        top: parent.top
                        bottom: parent.bottom
                    }
                    width: 6
                    gradient: Gradient {
                        GradientStop {
                            position: 0
                            color: hintSlot.gradColor0
                        }
                        GradientStop {
                            position: 1
                            color: hintSlot.gradColor1
                        }
                    }
                }

                Text {
                    id: hintLabel
                    x: 33
                    anchors.verticalCenter: parent.verticalCenter
                    text: hintSlot.hintText
                    color: Theme.pureWhite
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSize
                    elide: Text.ElideRight
                }
            }

            // ---- 入场动画（对应原版 Hint Show：弹性滑入 + 颜色加深 + 高度展开）----

            NumberAnimation {
                id: fadeIn
                target: hintVisual
                property: "opacity"
                to: 1
                duration: 100
            }

            SequentialAnimation {
                id: slideIn
                NumberAnimation {
                    target: hintVisual
                    property: "x"
                    to: 30
                    duration: 400
                    easing.type: Easing.OutElastic
                    easing.amplitude: 1.0
                    easing.period: 0.4
                }
                NumberAnimation {
                    target: hintVisual
                    property: "x"
                    to: 50
                    duration: 200
                    easing.type: Easing.OutCubic
                }
                onStopped: hintSlot.showDone = true
            }

            SequentialAnimation {
                id: mixIn
                PauseAnimation {
                    duration: 100
                }
                NumberAnimation {
                    target: hintSlot
                    property: "mix"
                    to: 1.0
                    duration: 250
                }
            }

            NumberAnimation {
                id: heightIn
                target: hintSlot
                property: "height"
                to: 26
                duration: 150
                easing.type: Easing.OutCubic
            }

            // ---- 停留计时（原版：800 + 字数钳制 5~23 × 180 毫秒）----

            Timer {
                id: stayTimer
                interval: 800 + Math.min(Math.max(hintSlot.hintText.length, 5), 23) * 180
                repeat: false
                onTriggered: hintSlot._startHide()
            }

            // ---- 退出动画（对应原版 Hint Hide：左滑淡出 → 收起高度 → 移除）----

            SequentialAnimation {
                id: hideAnim
                ParallelAnimation {
                    NumberAnimation {
                        target: hintVisual
                        property: "x"
                        to: 0
                        duration: 200
                        easing.type: Easing.InCubic
                    }
                    NumberAnimation {
                        target: hintVisual
                        property: "opacity"
                        to: 0
                        duration: 150
                    }
                }
                NumberAnimation {
                    target: hintSlot
                    property: "height"
                    to: 0
                    duration: 100
                    easing.type: Easing.OutCubic
                }
                ScriptAction {
                    script: hintSlot._dismiss()
                }
            }

            // ---- 抖动动画（重复提示时播放，对应原版 DoubleStack 的 AaX 序列）----

            SequentialAnimation {
                id: shakeAnim
                NumberAnimation {
                    target: hintVisual
                    property: "x"
                    to: 58
                    duration: 50
                    easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    target: hintVisual
                    property: "x"
                    to: 50
                    duration: 50
                    easing.type: Easing.InCubic
                }
                NumberAnimation {
                    target: hintVisual
                    property: "x"
                    to: 58
                    duration: 50
                    easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    target: hintVisual
                    property: "x"
                    to: 50
                    duration: 50
                    easing.type: Easing.InCubic
                }
            }

            // ---- 行为 ----

            // 重复提示：抖动 + 颜色重新加深 + 重新计时
            function replay() {
                hideAnim.stop();
                canReuse = true;
                hintVisual.opacity = 1;
                hintVisual.x = 50;
                height = 26;
                mix = 0.3;
                shakeAnim.start();
                mixIn.restart();
                stayTimer.restart();
            }

            function _startHide() {
                canReuse = false;
                hideAnim.start();
            }

            function _dismiss() {
                hintLayer._remove(hintSlot);
                hintSlot.destroy();
            }

            Component.onCompleted: {
                fadeIn.start();
                slideIn.start();
                mixIn.start();
                if (expandOnShow)
                    heightIn.start();
                stayTimer.start();
            }
        }
    }
}
