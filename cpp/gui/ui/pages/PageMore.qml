import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MLC

// More tab — about, tools, help links
Item {
    id: root
    property bool isActive: false

    // 发布前替换为真实仓库地址
    readonly property string repoUrl: "https://github.com/owner/MLC"

    // qmllint disable unqualified
    // qmllint disable missing-property
    // 打开目录：无法可靠预检目录是否存在，直接交给系统处理，分发失败时提示
    function openFolder(path) {
        if (!Qt.openUrlExternally("file://" + path))
            Window.window.showHint("目录不存在", "error");
    }

    // 工具区动作：act = mods / versions / game / logs
    function openTool(act) {
        if (act === "mods") {
            // 当前实例与 PageLaunch 自动选中首个实例的逻辑一致
            if (VersionManager.versionIds.length === 0) {
                Window.window.showHint("当前没有实例", "info");
                return;
            }
            var info = InstanceBridge.instanceInfo(VersionManager.versionIds[0]);
            openFolder(info.path + "mods/");
        } else if (act === "versions") {
            openFolder(VersionManager.mcFolder + "versions/");
        } else if (act === "game") {
            openFolder(VersionManager.mcFolder);
        } else if (act === "logs") {
            openFolder(VersionManager.mcFolder + "logs/");
        }
    }

    // 关于区动作：act = docs / github / update / vote
    function openAbout(act) {
        if (act === "update") {
            // GUI 更新功能未做，引导到 CLI
            Window.window.showHint("请使用 mlc update 检查更新", "info");
        } else {
            Qt.openUrlExternally(repoUrl);
        }
    }
    // qmllint enable missing-property
    // qmllint enable unqualified

    visible: opacity > 0
    opacity: isActive ? 1 : 0
    scale: isActive ? 1 : 0.96
    Behavior on opacity { NumberAnimation { duration: 100 } }
    Behavior on scale { NumberAnimation { duration: 400; easing.type: Easing.OutBack } }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Left sidebar ----
        Rectangle {
            Layout.preferredWidth: 280
            Layout.fillHeight: true
            color: Theme.sidebarBg

            Rectangle {
                anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                width: Theme.shadowWidth; opacity: Theme.shadowOpacity
                gradient: Gradient {
                    GradientStop { position: 0; color: "#000000" }
                    GradientStop { position: 1; color: "#00000000" }
                }
            }

            Flickable {
                anchors.fill: parent
                contentWidth: width
                contentHeight: panMore.implicitHeight + 20
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: MLCScrollBar {}

                ColumnLayout {
                    id: panMore
                    anchors { left: parent.left; right: parent.right; top: parent.top }
                    spacing: 0

                    // Section: 工具
                    Text {
                        text: "工具"
                        color: Theme.color1; opacity: Theme.textOpacityHint
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                        Layout.leftMargin: 15; Layout.topMargin: 18; Layout.bottomMargin: 4
                    }
                    Repeater {
                        model: [
                            { name: "打开 Mods 文件夹", icon: "layout-grid", act: "mods" },
                            { name: "打开版本文件夹", icon: "layout-grid", act: "versions" },
                            { name: "打开游戏目录", icon: "layout-grid", act: "game" },
                            { name: "启动器日志", icon: "layout-grid", act: "logs" }
                        ]
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            color: toolMouse.containsMouse ? Theme.color7 : "transparent"
                            radius: Theme.buttonRadius
                            Layout.leftMargin: 8; Layout.rightMargin: 8

                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; spacing: Theme.itemSpacing
                                            // qmllint disable unqualified
                                MLCIcon { size: 16; lucideIcon: modelData.icon; iconColor: Theme.color3 }
                                Text {
                                            // qmllint disable unqualified
                                    text: modelData.name; color: Theme.color1
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                                    Layout.fillWidth: true; elide: Text.ElideRight
                                }
                            }
                            MouseArea {
                                id: toolMouse; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                // qmllint disable unqualified
                                onClicked: root.openTool(modelData.act)
                            }
                        }
                    }

                    // Section: 关于
                    Text {
                        text: "关于"
                        color: Theme.color1; opacity: Theme.textOpacityHint
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                        Layout.leftMargin: 15; Layout.topMargin: 18; Layout.bottomMargin: 4
                    }
                    Repeater {
                        model: [
                            { name: "帮助文档", icon: "bolt", act: "docs" },
                            { name: "GitHub 仓库", icon: "bolt", act: "github" },
                            { name: "检查更新", icon: "bolt", act: "update" },
                            { name: "功能投票", icon: "bolt", act: "vote" }
                        ]
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            color: aboutMouse.containsMouse ? Theme.color7 : "transparent"
                            radius: Theme.buttonRadius
                            Layout.leftMargin: 8; Layout.rightMargin: 8

                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; spacing: Theme.itemSpacing
                                            // qmllint disable unqualified
                                MLCIcon { size: 16; lucideIcon: modelData.icon; iconColor: Theme.color3 }
                                Text {
                                            // qmllint disable unqualified
                                    text: modelData.name; color: Theme.color1
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                                    Layout.fillWidth: true; elide: Text.ElideRight
                                }
                            }
                            MouseArea {
                                id: aboutMouse; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                // qmllint disable unqualified
                                onClicked: root.openAbout(modelData.act)
                            }
                        }
                    }
                    Item { Layout.preferredHeight: 20 }
                }
            }
        }

        // ---- Right content ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"

            Flickable {
                anchors.fill: parent
                contentWidth: width
                contentHeight: panContent.implicitHeight + 35
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: MLCScrollBar {}

                ColumnLayout {
                    id: panContent
                    anchors { left: parent.left; right: parent.right; top: parent.top }
                    anchors.margins: Theme.contentMargin
                    spacing: Theme.sectionSpacing

                    // About MLC card
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: aboutInner.height + 36
                        radius: Theme.buttonRadius
                        color: Theme.pureWhite
                        border { width: 1; color: Theme.gray5 }

                        ColumnLayout {
                            id: aboutInner
                            anchors { left: parent.left; right: parent.right; top: parent.top }
                            anchors.margins: 20; spacing: 8
                            Text {
                                text: "关于 MinecraftLauncherCLI"
                                color: Theme.color1; font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeLarge; font.bold: true
                            }
                            Text {
                                text: "版本: v" + Qt.application.version + "\n\n" +
                                      "PCL 的 Qt6/C++20 Linux 移植版本。\n" +
                                      "使用 QML + C++ 构建，支持跨平台运行。\n\n" +
                                      "原版 Windows PCL 使用 VB.NET/WPF，\n" +
                                      "约 49,000 行代码，由 LTCatt 开发。\n\n" +
                                      "目标: Windows | macOS | Linux"
                                color: Theme.gray3; font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSize; Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    // Help links card
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: helpInner.height + 36
                        radius: Theme.buttonRadius
                        color: Theme.pureWhite
                        border { width: 1; color: Theme.gray5 }

                        ColumnLayout {
                            id: helpInner
                            anchors { left: parent.left; right: parent.right; top: parent.top }
                            anchors.margins: 20; spacing: Theme.itemSpacing
                            Text {
                                text: "相关链接"
                                color: Theme.color1; font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeLarge; font.bold: true
                            }
                            Repeater {
                                model: [
                                    { text: "PCL 下载", desc: "下载正式版 PCL (Windows)", url: "https://meloong.com/afd/p/0164034c016c11ebafcb52540025c377" },
                                    { text: "GitHub", desc: "Meloong-Git/PCL 仓库", url: "https://github.com/Meloong-Git/PCL" },
                                    { text: "帮助文档", desc: "PCL2Help 帮助文档库", url: "https://github.com/LTCatt/PCL2Help" },
                                    { text: "功能投票", desc: "参与功能投票，决定开发优先级", url: "https://github.com/Meloong-Git/PCL/discussions/2" }
                                ]
                                MLCButton {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 40
                                    colorType: 0
                                    // qmllint disable unqualified
                                    onClicked: Qt.openUrlExternally(modelData.url)
                                    contentItem: ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 2
                                        Text {
                                            // qmllint disable unqualified
                                            text: modelData.text
                                            color: Theme.color3; font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontSize; font.bold: true
                                            Layout.alignment: Qt.AlignHCenter
                                        }
                                        Text {
                                            // qmllint disable unqualified
                                            text: modelData.desc
                                            color: Theme.gray3; font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontSizeXSmall
                                            Layout.alignment: Qt.AlignHCenter
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item { Layout.preferredHeight: 10 }
                }
            }
        }
    }
}
