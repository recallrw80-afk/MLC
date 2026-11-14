import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MLC

// Settings tab — left sidebar + right content
Item {
    id: pageSettings
    property bool isActive: false
    visible: opacity > 0
    opacity: isActive ? 1 : 0
    scale: isActive ? 1 : 0.96
    Behavior on opacity { NumberAnimation { duration: 100 } }
    Behavior on scale { NumberAnimation { duration: 400; easing.type: Easing.OutBack } }

    // ---- Java 管理状态 ----
    // javaEntries: JavaManager.javaVariantList() 的缓存（[{pathJava, display, ...}]）
    // currentJavaSelect: 用户指定的 java 可执行文件全路径，空串 = 自动选择
    property var javaEntries: []
    property string currentJavaSelect: ""
    readonly property string effectiveJavaText: currentJavaSelect === ""
        // qmllint disable unqualified
        ? "自动选择（当前：" + JavaManager.selectedJava + "）"
        : currentJavaSelect

    // ---- 重建 Java 下拉列表，并按 Settings 的 LaunchJavaSelect 恢复选中 ----
    function rebuildJavaModel() {
        // qmllint disable unqualified
        var entries = JavaManager.javaVariantList();
        javaEntries = entries;
        var items = ["自动选择（推荐）"];
        for (var i = 0; i < entries.length; i++)
            items.push(entries[i].display);
        // qmllint disable unqualified
        var saved = String(Settings.value("LaunchJavaSelect", ""));
        var idx = 0;
        if (saved !== "") {
            for (var j = 0; j < entries.length; j++) {
                if (entries[j].pathJava === saved) {
                    idx = j + 1;
                    break;
                }
            }
            // 保存的 Java 已不存在：回退为自动并修正设置
            if (idx === 0) {
                saved = "";
                // qmllint disable unqualified
                Settings.setValue("LaunchJavaSelect", "");
            }
        }
        currentJavaSelect = saved;
        javaCombo.model = items;
        javaCombo.currentIndex = idx;
    }

    Connections {
        // qmllint disable unqualified
        target: JavaManager
        function onJavaListChanged() { pageSettings.rebuildJavaModel(); }
    }

    Connections {
        // qmllint disable unqualified
        target: InstallBridge
        function onJavaInstallFinished(ok, msg) {
            if (ok) {
                // qmllint disable missing-property
                Window.window.showHint("JRE 安装完成", "success");
            } else {
                // qmllint disable missing-property
                Window.window.showHint("JRE 安装失败：" + msg, "error");
            }
        }
    }

    Component.onCompleted: rebuildJavaModel()

    RowLayout {
        anchors.fill: parent
        spacing: 0

    Rectangle {
        Layout.preferredWidth: Theme.sidebarWidth
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
        Text {
            anchors.centerIn: parent
            text: "设置"
            color: Theme.gray3
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeLarge
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: "transparent"

        Flickable {
            id: scrollView
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

                Text {
                    text: "Settings"
                    color: Theme.color1
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeLogo
                    font.bold: true
                    Layout.bottomMargin: 10
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: launchGrid.height + 40
                    radius: Theme.buttonRadius
                    color: Theme.pureWhite
                    border.color: Theme.gray5
                    ColumnLayout {
                        anchors { left: parent.left; right: parent.right; top: parent.top }
                        anchors.margins: Theme.contentMargin; anchors.topMargin: 40
                        spacing: Theme.itemSpacing
                        Text { text: "Launch Settings"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLarge; font.bold: true }
                        GridLayout {
                            id: launchGrid
                            columns: 2; rowSpacing: 8; columnSpacing: 15
                            Text { text: "Max Memory:"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize }
                            MLCTextBox {
                                id: maxMemField; Layout.preferredWidth: 120; placeholderText: "0 = auto"
                                // qmllint disable unqualified
                                Component.onCompleted: text = Settings.value("LaunchMaxMemory", "0")
                                onEditingFinished: {
                                    var n = parseInt(text);
                                    if (isNaN(n) || n < 0) n = 0;
                                    text = String(n);
                                    // qmllint disable unqualified
                                    Settings.setValue("LaunchMaxMemory", text);
                                }
                            }
                            Text { text: "Window Size:"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize }
                            RowLayout {
                                MLCTextBox {
                                    id: widthField; Layout.preferredWidth: 70; placeholderText: "W"
                                    // qmllint disable unqualified
                                    Component.onCompleted: text = Settings.value("LaunchWidth", "854")
                                    onEditingFinished: {
                                        var n = parseInt(text);
                                        if (isNaN(n) || n <= 0) n = 854;
                                        text = String(n);
                                        // qmllint disable unqualified
                                        Settings.setValue("LaunchWidth", text);
                                    }
                                }
                                Text { text: "×"; color: Theme.gray3 }
                                MLCTextBox {
                                    id: heightField; Layout.preferredWidth: 70; placeholderText: "H"
                                    // qmllint disable unqualified
                                    Component.onCompleted: text = Settings.value("LaunchHeight", "480")
                                    onEditingFinished: {
                                        var n = parseInt(text);
                                        if (isNaN(n) || n <= 0) n = 480;
                                        text = String(n);
                                        // qmllint disable unqualified
                                        Settings.setValue("LaunchHeight", text);
                                    }
                                }
                            }
                            Text { text: "Java Args:"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize }
                            MLCTextBox {
                                id: javaArgsField; Layout.fillWidth: true; placeholderText: "Custom JVM arguments"
                                // qmllint disable unqualified
                                Component.onCompleted: text = Settings.value("LaunchAdvanceJvm", "")
                                // qmllint disable unqualified
                                onEditingFinished: Settings.setValue("LaunchAdvanceJvm", text)
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: javaContent.height + 40
                    radius: Theme.buttonRadius
                    color: Theme.pureWhite
                    border.color: Theme.gray5
                    ColumnLayout {
                        id: javaContent
                        anchors { left: parent.left; right: parent.right; top: parent.top }
                        anchors.margins: Theme.contentMargin; anchors.topMargin: 40
                        spacing: Theme.itemSpacing
                        Text { text: "Java Runtime"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLarge; font.bold: true }

                        // ---- Java 选择：自动 / 已检测到的 Java 列表 ----
                        RowLayout {
                            spacing: Theme.itemSpacing
                            Text { text: "Java:"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize }
                            MLCComboBox {
                                id: javaCombo
                                Layout.fillWidth: true
                                onActivated: index => {
                                    var path = index <= 0 ? "" : pageSettings.javaEntries[index - 1].pathJava;
                                    pageSettings.currentJavaSelect = path;
                                    // qmllint disable unqualified
                                    Settings.setValue("LaunchJavaSelect", path);
                                }
                            }
                        }

                        // ---- 状态行：安装数量 + 当前实际生效的选择 ----
                        // qmllint disable unqualified
                        Text { text: "Found " + JavaManager.javaCount + " Java installation(s)"; color: Theme.gray3; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall }
                        Text {
                            text: "Effective: " + pageSettings.effectiveJavaText
                            color: Theme.gray3; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                            Layout.fillWidth: true; elide: Text.ElideMiddle
                        }

                        // ---- 扫描系统 Java ----
                        RowLayout {
                            MLCButton {
                                // qmllint disable unqualified
                                text: JavaManager.isScanning ? "Scanning..." : "Scan for Java"
                                colorType: 1
                                // qmllint disable unqualified
                                enabled: !JavaManager.isScanning
                                // qmllint disable unqualified
                                onClicked: JavaManager.scanSystemJava()
                            }
                            Item { Layout.fillWidth: true }
                        }

                        // ---- 下载 JRE（Adoptium，安装后自动注册进列表） ----
                        RowLayout {
                            spacing: Theme.itemSpacing
                            Text { text: "Download JRE:"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize }
                            // qmllint disable unqualified
                            MLCButton { text: "下载 JRE 8"; enabled: !InstallBridge.busy; onClicked: InstallBridge.installJava(8) }
                            // qmllint disable unqualified
                            MLCButton { text: "下载 JRE 17"; enabled: !InstallBridge.busy; onClicked: InstallBridge.installJava(17) }
                            // qmllint disable unqualified
                            MLCButton { text: "下载 JRE 21"; enabled: !InstallBridge.busy; onClicked: InstallBridge.installJava(21) }
                            Item { Layout.fillWidth: true }
                        }
                        Text {
                            // qmllint disable unqualified
                            visible: InstallBridge.busy
                            // qmllint disable unqualified
                            text: InstallBridge.progressText + " (" + InstallBridge.progressPercent + "%)"
                            color: Theme.gray3; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                            Layout.fillWidth: true; elide: Text.ElideMiddle
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: uiContent.height + 40
                    radius: Theme.buttonRadius
                    color: Theme.pureWhite
                    border.color: Theme.gray5
                    ColumnLayout {
                        id: uiContent
                        anchors { left: parent.left; right: parent.right; top: parent.top }
                        anchors.margins: Theme.contentMargin; anchors.topMargin: 40
                        spacing: Theme.itemSpacing
                        Text { text: "UI Settings"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLarge; font.bold: true }
                        RowLayout {
                            Text { text: "Theme Color:"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize }
                            MLCComboBox { model: ["Blue (Default)", "Red", "Green", "Purple", "Orange"]; Layout.preferredWidth: 180 }
                        }
                        RowLayout {
                            Text { text: "Background Music:"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize }
                            MLCComboBox { model: ["Off", "On"]; Layout.preferredWidth: 120 }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: aboutContent.height + 40
                    radius: Theme.buttonRadius
                    color: Theme.pureWhite
                    border.color: Theme.gray5
                    ColumnLayout {
                        id: aboutContent
                        anchors { left: parent.left; right: parent.right; top: parent.top }
                        anchors.margins: Theme.contentMargin; anchors.topMargin: 40
                        spacing: 8
                        Text { text: "About MLC"; color: Theme.color1; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLarge; font.bold: true }
                        Text {
                            text: "Version 0.1 (MVP)\nBuilt with Qt 6.11 + QML + C++20\n\nPort of Plain Craft Launcher 2\nOriginal: ~49,000 lines VB.NET/WPF\n\nTarget: Windows, macOS, Linux"
                            color: Theme.gray3; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize; Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                    }
                }

                Item { Layout.preferredHeight: 10 }
            }
        }
    }
}
}
