import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import MLC

ApplicationWindow {
    id: window

    // ---- App version (injected by main.cpp via setInitialProperties) ----
    property string appVersion: ""

    // ---- Window sizing: 850:500 ratio, 40% of screen short side ----active:
    readonly property real baseRatio: 850 / 600
    readonly property real screenShort: Math.min(Screen.width, Screen.height)
    readonly property real targetHeight: window.screenShort * 0.56
    readonly property real targetWidth: targetHeight * window.baseRatio

    width: window.targetWidth
    height: window.targetHeight
    minimumWidth: 850
    minimumHeight: 500

    // Center on screen (overlays splash icon position)
    Component.onCompleted: {
        // Screen.virtualGeometry can be undefined early on; Screen.width/height
        // are the same attached object's scalar props and are reliably available.
        x = (Screen.width - width) / 2;
        y = (Screen.height - height) / 2;
    }

    visible: true
    title: "MinecraftLauncherCLI"

    // Opacity driven by custom property for reliable animation (entrance fade-in)
    property real winOpacity: 0
    opacity: winOpacity

    // Frameless + transparent for custom chrome (WindowStyle="None" AllowsTransparency="True" Topmost="True")
    flags: Qt.FramelessWindowHint | Qt.Window
    color: "transparent"

    // ---- 全局弹窗 / 轻提示 API（页面通过 Window.window.showMsg(...) / showHint(...) 调用）----
    function showMsg(options) {
        panMsg.show(options);
    }

    function showHint(text, type) {
        panHint.show(text, type);
    }

    // ---- 全局 tab 切换（0=启动 1=下载 2=设置 3=更多），页面经 Window.window.switchTab(i) 调用 ----
    function switchTab(index) {
        navTabs.currentIndex = index;
    }

    // PanBack (Grid, Margin=10)

    Item {
        id: panBack
        anchors.fill: parent
        anchors.margins: Theme.windowMargin

        // Entrance animation props (match WPF TranslateTransform Y="60" + RotateTransform Angle="-4")
        property real entranceSlide: 60
        property real entranceTilt: -4
        transform: [
            Translate {
                y: panBack.entranceSlide
            },
            Rotation {
                angle: panBack.entranceTilt
                origin.x: panBack.width / 2
                origin.y: panBack.height / 2
            }
        ]

        // ---- 8 Resizer handles (exact match FormMain.xaml resizers) ----
        // Top edge
        MouseArea {
            id: resizerT
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                leftMargin: Theme.resizerCorner
                rightMargin: Theme.resizerCorner
            }
            height: Theme.resizerThickness
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => window.startSystemResize(Qt.TopEdge)
        }
        // Bottom edge
        MouseArea {
            id: resizerB
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                leftMargin: Theme.resizerCorner
                rightMargin: Theme.resizerCorner
            }
            height: Theme.resizerThickness
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => window.startSystemResize(Qt.BottomEdge)
        }
        // Left edge
        MouseArea {
            id: resizerL
            anchors {
                left: parent.left
                top: parent.top
                bottom: parent.bottom
                topMargin: Theme.resizerCorner
                bottomMargin: Theme.resizerCorner
            }
            width: Theme.resizerThickness
            cursorShape: Qt.SizeHorCursor
            onPressed: mouse => window.startSystemResize(Qt.LeftEdge)
        }
        // Right edge
        MouseArea {
            id: resizerR
            anchors {
                right: parent.right
                top: parent.top
                bottom: parent.bottom
                topMargin: Theme.resizerCorner
                bottomMargin: Theme.resizerCorner
            }
            width: Theme.resizerThickness
            cursorShape: Qt.SizeHorCursor
            onPressed: mouse => window.startSystemResize(Qt.RightEdge)
        }
        // Top-left corner
        MouseArea {
            id: resizerLT
            anchors {
                left: parent.left
                top: parent.top
            }
            width: Theme.resizerCorner
            height: Theme.resizerCorner
            cursorShape: Qt.SizeFDiagCursor
            onPressed: mouse => window.startSystemResize(Qt.LeftEdge | Qt.TopEdge)
        }
        // Top-right corner
        MouseArea {
            id: resizerRT
            anchors {
                right: parent.right
                top: parent.top
            }
            width: Theme.resizerCorner
            height: Theme.resizerCorner
            cursorShape: Qt.SizeBDiagCursor
            onPressed: mouse => window.startSystemResize(Qt.RightEdge | Qt.TopEdge)
        }
        // Bottom-left corner
        MouseArea {
            id: resizerLB
            anchors {
                left: parent.left
                bottom: parent.bottom
            }
            width: Theme.resizerCorner
            height: Theme.resizerCorner
            cursorShape: Qt.SizeBDiagCursor
            onPressed: mouse => window.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
        }
        // Bottom-right corner
        MouseArea {
            id: resizerRB
            anchors {
                right: parent.right
                bottom: parent.bottom
            }
            width: Theme.resizerCorner
            height: Theme.resizerCorner
            cursorShape: Qt.SizeFDiagCursor
            onPressed: mouse => window.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        }

        // BorderForm (Margin=8, Clip RadiusX=6 RadiusY=6)
        Rectangle {
            id: borderForm
            anchors.fill: parent
            anchors.margins: Theme.borderMargin
            radius: Theme.windowCornerRadius
            clip: true
            color: Theme.pureWhite

            // ---- PanForm (Grid with 2 rows: Title + Content) ----
            Item {
                id: panForm
                anchors.fill: parent

                // ---- Background gradient (ImgBack substitute) ----
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop {
                            position: -0.1
                            color: Theme.colorBgLeft
                        }
                        GradientStop {
                            position: 0.4
                            color: Theme.colorBgCenter
                        }
                        GradientStop {
                            position: 1.1
                            color: Theme.colorBgRight
                        }
                    }
                }

                // PanTitle (Height=48) — Title bar
                Rectangle {
                    id: panTitle
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                    }
                    height: Theme.titleBarHeight
                    z: 10
                    color: Theme.color2

                    // ---- Drag window by title bar (native, no stutter) ----
                    MouseArea {
                        anchors.fill: parent
                        onPressed: mouse => window.startSystemMove()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.resizerCorner
                        anchors.rightMargin: 8
                        spacing: 0

                        // MLC text + version label (baseline-aligned)
                        Item {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.leftMargin: 6
                            implicitWidth: mlcText.implicitWidth + (versionText.visible ? versionText.implicitWidth + 3 : 0)
                            implicitHeight: mlcText.implicitHeight

                            Text {
                                id: mlcText
                                text: "MLC"
                                color: Theme.pureWhite
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeLogo
                                font.bold: true
                            }

                            Text {
                                id: versionText
                                anchors.left: mlcText.right
                                anchors.leftMargin: 3
                                anchors.baseline: mlcText.baseline
                                text: window.appVersion ? window.appVersion : ""
                                color: Theme.pureWhite
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSmall
                                visible: window.appVersion !== ""
                            }
                        }

                        // Left spacer
                        Item {
                            Layout.fillWidth: true
                        }

                        // Navigation tabs
                        Row {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredHeight: 27
                            spacing: 12

                            Repeater {
                                model: [
                                    {
                                        text: "启动",
                                        tag: 0,
                                        icon: "play"
                                    },
                                    {
                                        text: "下载",
                                        tag: 1,
                                        icon: "arrow-down-to-line"
                                    },
                                    {
                                        text: "设置",
                                        tag: 2,
                                        icon: "bolt"
                                    },
                                    {
                                        text: "更多",
                                        tag: 3,
                                        icon: "layout-grid"
                                    }
                                ]

                                Item {
                                    width: tabRowContent.width + 25
                                    implicitWidth: width
                                    height: 27
                                    property bool hovered: false

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: tabRowContent.width
                                            // qmllint disable unqualified
                                        color: navTabs.currentIndex === modelData.tag ? Theme.pureWhite : parent.hovered ? "#33ffffff" : "transparent"
                                        Behavior on color {
                                            ColorAnimation {
                                                duration: 100
                                            }
                                        }

                                        Row {
                                            id: tabRowContent
                                            anchors.centerIn: parent
                                            height: 27
                                            spacing: Theme.tightSpacing

                                            // SVG icon from assets (external file, no inline code)
                                            MLCIcon {
                                                size: 16
                                                anchors.verticalCenter: parent.verticalCenter
                                            // qmllint disable unqualified
                                                lucideIcon: modelData.icon
                                            // qmllint disable unqualified
                                                iconColor: navTabs.currentIndex === modelData.tag ? Theme.color2 : Theme.pureWhite
                                            }

                                            Text {
                                            // qmllint disable unqualified
                                                text: modelData.text
                                            // qmllint disable unqualified
                                                color: navTabs.currentIndex === modelData.tag ? Theme.color2 : Theme.pureWhite
                                                font.family: Theme.fontFamily
                                                font.pixelSize: Theme.fontSize
                                                height: 27
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onEntered: parent.hovered = true
                                        onExited: parent.hovered = false
                                        onClicked: {
                                            // qmllint disable unqualified
                                            navTabs.currentIndex = modelData.tag;
                                        }
                                    }
                                }
                            }
                        }

                        // Right spacer
                        Item {
                            Layout.fillWidth: true
                        }

                        // Minimize button
                        Item {
                            id: btnTitleMin
                            Layout.preferredWidth: Theme.titleBtnSize
                            Layout.preferredHeight: Theme.titleBtnSize
                            Layout.alignment: Qt.AlignVCenter
                            property bool hovered: false

                            Rectangle {
                                anchors.centerIn: parent
                                width: Theme.titleBtnSize
                                height: Theme.titleBtnSize
                                radius: 3
                                color: btnTitleMin.hovered ? "#33ffffff" : "transparent"
                            }
                            MLCIcon {
                                size: 24
                                anchors.centerIn: parent
                                lucideIcon: "minus"
                                iconColor: Theme.pureWhite
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: btnTitleMin.hovered = true
                                onExited: btnTitleMin.hovered = false
                                onClicked: window.showMinimized()
                            }
                        }

                        // Close button
                        Item {
                            id: btnTitleClose
                            Layout.preferredWidth: Theme.titleBtnSize
                            Layout.preferredHeight: Theme.titleBtnSize
                            Layout.alignment: Qt.AlignVCenter
                            property bool hovered: false

                            Rectangle {
                                anchors.centerIn: parent
                                width: Theme.titleBtnSize
                                height: Theme.titleBtnSize
                                radius: 3
                                color: btnTitleClose.hovered ? "#33ffffff" : "transparent"
                            }
                            MLCIcon {
                                size: 24
                                anchors.centerIn: parent
                                lucideIcon: "x"
                                iconColor: Theme.pureWhite
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: btnTitleClose.hovered = true
                                onExited: btnTitleClose.hovered = false
                                onClicked: window.close()
                            }
                        }  // btnTitleClose
                    }  // RowLayout

                    // Bottom-right extra buttons
                    ColumnLayout {
                        anchors {
                            right: parent.right
                            bottom: parent.bottom
                            margins: 15
                        }
                        spacing: 6
                        z: 50

                        // Back to top
                        Item {
                            id: btnExtraBack
                            Layout.preferredWidth: Theme.titleBtnSize
                            Layout.preferredHeight: Theme.titleBtnSize
                            property bool hovered: false
                            visible: false

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.buttonRadius
                                color: btnExtraBack.hovered ? Theme.color7 : "transparent"
                            }
                            MLCIcon {
                                size: 16
                                anchors.centerIn: parent
                                lucideIcon: "arrow-down-to-line"
                                rotation: 180
                                iconColor: Theme.gray3
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: btnExtraBack.hovered = true
                                onExited: btnExtraBack.hovered = false
                            }
                        }

                        // Download manager
                        Item {
                            id: btnExtraDownload
                            Layout.preferredWidth: Theme.titleBtnSize
                            Layout.preferredHeight: Theme.titleBtnSize
                            property bool hovered: false
                            visible: false

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.buttonRadius
                                color: btnExtraDownload.hovered ? Theme.color7 : "transparent"
                            }
                            MLCIcon {
                                size: 16
                                anchors.centerIn: parent
                                lucideIcon: "arrow-down-to-line"
                                iconColor: Theme.gray3
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: btnExtraDownload.hovered = true
                                onExited: btnExtraDownload.hovered = false
                                onClicked: navTabs.currentIndex = 1
                            }
                        }
                    }
                }  // panTitle

                // Tab 0-3: each page handles its own transition internally
                PageLaunch {
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: panTitle.bottom
                        bottom: parent.bottom
                    }
                    isActive: navTabs.currentIndex === 0
                }
                PageDownload {
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: panTitle.bottom
                        bottom: parent.bottom
                    }
                    isActive: navTabs.currentIndex === 1
                }
                PageSettings {
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: panTitle.bottom
                        bottom: parent.bottom
                    }
                    isActive: navTabs.currentIndex === 2
                }
                PageMore {
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: panTitle.bottom
                        bottom: parent.bottom
                    }
                    isActive: navTabs.currentIndex === 3
                }

                // PanDownload — 下载任务面板（对应原版 PageSpeedLeft 精简版：左下角悬浮按钮 + 可展开任务列表）
                MLCDownloadPanel {
                    id: panDownload
                    anchors.fill: parent
                    z: 80  // 低于 PanHint(90) 与 PanMsg(100)，避免遮挡提示和弹窗
                }

                // DownloadManager 全局下载信号 → 任务面板
                // qmllint disable unqualified
                Connections {
                    target: DownloadManager

                    function onDownloadStarted(url) { panDownload.addTask(url); }
                    function onDownloadProgress(url, received, total) { panDownload.updateTask(url, received, total); }
                    function onDownloadFinished(url, success, msg) { panDownload.finishTask(url, success, msg); }
                }
                // qmllint enable unqualified

                // PanHint — 轻提示层（对应原版 PanHint：左下角滑入提示，覆盖于页面之上）
                MLCHint {
                    id: panHint
                    anchors.fill: parent
                    z: 90
                }

                // PanMsg — 消息弹窗层（对应原版 PanMsg：模态遮罩覆盖整个窗口含标题栏，默认隐藏）
                MLCMsg {
                    id: panMsg
                    anchors.fill: parent
                    z: 100
                }
            }  // borderForm
        }  // panBack

        // Entrance animation (splash is a separate window in main.cpp — see FrmStart)
        // Original FormMain_Loaded:
        //   AaOpacity 250ms | AaDouble Y 60→0 600ms EaseOutBack | AaDouble Angle -4→0 500ms EaseOutBack
        //   All beginTime=100, run in parallel

        Timer {
            id: entranceDelay
            interval: 100  // matches original beginTime=100
            repeat: false
            onTriggered: {
                slideUp.start();
                tiltBack.start();
            }
        }

        NumberAnimation {
            id: fadeInAnim
            target: window
            property: "winOpacity"
            to: 1.0
            duration: 250
            easing.type: Easing.OutCubic
        }

        NumberAnimation {
            id: slideUp
            target: panBack
            property: "entranceSlide"
            to: 0
            duration: 600
            easing.type: Easing.OutBack
        }

        NumberAnimation {
            id: tiltBack
            target: panBack
            property: "entranceTilt"
            to: 0
            duration: 500
            easing.type: Easing.OutBack
        }

        // Navigation

        QtObject {
            id: navTabs
            property int currentIndex: 0
        }

        Component.onCompleted: {
            // Bring window to front (above splash)
            window.raise();
            window.requestActivate();

            // Entrance animation (splash closes concurrently from main.cpp)
            //   Fade in: opacity 0 → 1.0, 250ms (original AaOpacity)
            //   Slide up: Y 60 → 0, 600ms after 100ms delay (original AaDouble EaseOutBack)
            fadeInAnim.start();
            entranceDelay.start();

            // Initialize — C++ singletons handle startup logic
            // qmllint disable unqualified
            JavaManager.scanSystemJava();
            VersionManager.loadLocalVersions();
            // qmllint enable unqualified
        }
    }
}
