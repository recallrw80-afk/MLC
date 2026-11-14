import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MLC

// Exact replica of PageLaunchLeft.xaml + PageLaunchRight.xaml layout + interaction logic
// Single file: left sidebar (300px) + right content
// Two modes: PanInput (default) ↔ PanLaunching (during launch)
// Sub-page: version selection (replaces both panels)
// 4-state launch button matching RefreshButtonsUI()
// 100ms polling timer with smooth progress
Item {
    id: page
    property bool isActive: false
    property int loginType: 0              // 0=Offline, 5=Ms
    property string selectedVersion: ""
    property string accountName: ""
    property string accountUuid: ""
    property var loginResult: null         // Last successful LoginResult (gadget)
    property string msDeviceCode: ""
    property string msVerifyUrl: ""
    property bool msPolling: false

    // ---- External drag-and-drop state (received via C++ FileDropHandler) ----
    property bool dragHovering: false
    // 导入进度文本（绑定 InstallBridge，busy 时显示在拖放悬浮层）
    // qmllint disable unqualified
    property string dragDropResult: InstallBridge.busy
                                    ? (InstallBridge.progressText + "（" + InstallBridge.progressPercent + "%）") : ""
    // qmllint enable unqualified
    // 待重试的 Mod 包路径（纯 Mod 包缺目标实例时二次导入用）
    property string pendingImportPath: ""

    // ---- Player profiles (source of truth: PlayerBridge 单例，持久化于 MLC.ini) ----
    property var playerListData: []

    // 当前选中玩家头像：skinType slim→Alex / wide→Steve（同 createLogin 语义）；
    // 无 skinType 时按 UUID 推导默认皮肤性别，无玩家回退 Steve
    // qmllint disable unqualified
    readonly property string offlineAvatar: {
        var st = PlayerBridge.currentSkinType;
        if (st === "wide")
            return "Steve";
        if (st === "slim")
            return "Alex";
        return PlayerBridge.currentUuid ? OfflineAuth.skinSexFromUuid(PlayerBridge.currentUuid) : "Steve";
    }

    function refreshPlayers() {
        playerListData = PlayerBridge.playerList();
        // 输入框跟随选中玩家，保证「输入新名字 → 启动/添加」流程一致
        if (PlayerBridge.currentName)
            page.accountName = PlayerBridge.currentName;
    }

    // 已存在同名玩家则直接选中，否则新建并选中；返回玩家 uuid
    function ensurePlayer(name) {
        var list = PlayerBridge.playerList();
        for (var i = 0; i < list.length; i++) {
            if (list[i].name === name) {
                PlayerBridge.selectPlayer(list[i].uuid);
                return list[i].uuid;
            }
        }
        var entry = PlayerBridge.addPlayer(name, "slim");
        PlayerBridge.selectPlayer(entry.uuid);
        return entry.uuid;
    }
    // qmllint enable unqualified

    // 玩家面板「添加玩家」入口：使用离线输入框中的用户名
    function addPlayerFromInput() {
        var name = page.accountName.trim();
        if (name === "") {
            // qmllint disable missing-property
            Window.window.showHint("请先在左侧输入游戏用户名", "info");
            return;
        }
        page.ensurePlayer(name);
        // qmllint disable missing-property
        Window.window.showHint("已添加玩家 " + name, "success");
    }

    function confirmRemovePlayer(uuid, name) {
        // qmllint disable missing-property
        Window.window.showMsg({
            title: "删除玩家",
            text: "确定删除玩家 " + name + "？",
            warn: true,
            button1: "删除",
            button2: "取消",
            callback: function (result) {
                // qmllint disable unqualified
                if (result === 1)
                    PlayerBridge.removePlayer(uuid);
                // qmllint enable unqualified
            }
        });
    }

    // ---- Identify dropped file type (only active when PageLaunch is visible) ----
    function identifyFileType(filePath) {
        var normalized = filePath.replace(/[\\/]/g, '/');
        var lower = normalized.toLowerCase();
        var leaf = normalized.split('/').pop();

        // Directory (trailing slash or no file extension)
        if (lower.endsWith('/') || (leaf.indexOf('.') === -1 && leaf.length > 0)) {
            return {
                kind: "folder",
                label: "文件夹",
                icon: "folder",
                detail: leaf
            };
        }

        var ext = leaf.split('.').pop().toLowerCase();

        if (ext === "jar") {
            return {
                kind: "jar",
                label: "Mod / Jar 文件",
                icon: "file-archive",
                detail: leaf
            };
        }
        if (ext === "zip") {
            return {
                kind: "zip",
                label: "压缩包",
                icon: "file-archive",
                detail: leaf
            };
        }
        if (ext === "mrpack") {
            return {
                kind: "mrpack",
                label: "Modrinth 整合包",
                icon: "package",
                detail: leaf
            };
        }
        if (ext === "json") {
            return {
                kind: "json",
                label: "JSON 配置文件",
                icon: "file-code",
                detail: leaf
            };
        }
        if (ext === "png" || ext === "jpg" || ext === "jpeg" || ext === "svg") {
            return {
                kind: "image",
                label: "图片文件",
                icon: "image",
                detail: leaf
            };
        }
        if (ext === "txt" || ext === "log") {
            return {
                kind: "text",
                label: "文本文件",
                icon: "file-text",
                detail: leaf
            };
        }

        return {
            kind: "unknown",
            label: "未知文件 (." + ext + ")",
            icon: "file",
            detail: leaf
        };
    }

    // ---- 拖放导入：zip/mrpack → 整合包；jar → 提示打包；其余不支持 ----
    // qmllint disable unqualified
    function handleFilesDropped(files) {
        if (!page.isActive || !files || files.length === 0) {
            page.dragHovering = false;
            return;
        }
        page.dragHovering = false;

        // 单任务守卫：busy 时拒绝新导入
        if (InstallBridge.busy) {
            // qmllint disable missing-property
            Window.window.showHint("正在导入其他文件，请稍候", "info");
            return;
        }

        // 一次只处理第一个文件
        var filePath = files[0].toString();
        var info = page.identifyFileType(filePath);

        if (info.kind === "zip" || info.kind === "mrpack") {
            // qmllint disable missing-property
            Window.window.showMsg({
                title: "导入整合包",
                text: "导入整合包 " + info.detail + "？",
                button1: "导入",
                button2: "取消",
                callback: function (result) {
                    if (result === 1) {
                        page.pendingImportPath = filePath;
                        InstallBridge.importModpack(filePath, "");
                    }
                }
            });
        } else if (info.kind === "jar") {
            // qmllint disable missing-property
            Window.window.showHint("请将 Mod 打包为 zip 后导入", "info");
        } else if (info.kind === "folder") {
            // qmllint disable missing-property
            Window.window.showHint("不支持导入文件夹", "info");
        } else {
            // qmllint disable missing-property
            Window.window.showHint("不支持导入该类型文件", "info");
        }
    }

    // 导入结果：成功刷新版本列表；纯 Mod 包缺目标实例时询问导入到当前实例
    Connections {
        target: InstallBridge
        function onImportFinished(ok, msg, data) {
            if (ok) {
                // qmllint disable missing-property
                Window.window.showHint("整合包导入完成", "success");
                page.refreshVersions();
                return;
            }
            if (data && data.length > 0) {
                if (page.selectedVersion === "") {
                    // qmllint disable missing-property
                    Window.window.showMsg({
                        title: "导入失败",
                        text: "这是一个 Mod 包，但当前没有可用的游戏实例",
                        warn: true
                    });
                    return;
                }
                // qmllint disable missing-property
                Window.window.showMsg({
                    title: "导入 Mod 包",
                    text: "这是一个 Mod 包，导入到当前实例 " + page.selectedVersion + "？",
                    button1: "导入",
                    button2: "取消",
                    callback: function (result) {
                        if (result === 1)
                            InstallBridge.importModpack(page.pendingImportPath, page.selectedVersion);
                    }
                });
                return;
            }
            // qmllint disable missing-property
            Window.window.showHint("导入失败", "error");
            // qmllint disable missing-property
            Window.window.showMsg({
                title: "导入失败",
                text: msg,
                warn: true
            });
        }
    }
    // qmllint enable unqualified

    visible: opacity > 0
    opacity: isActive ? 1 : 0
    scale: isActive ? 1 : 0.96
    Behavior on opacity {
        NumberAnimation {
            duration: 100
        }
    }
    Behavior on scale {
        NumberAnimation {
            duration: 400
            easing.type: Easing.OutBack
        }
    }

    // 4-state launch button (matching RefreshButtonsUI)
    // State 0: Loading — "正在加载", disabled
    // State 1: No versions (download hidden) — "启动游戏", disabled
    // State 2: No versions (download available) — "下载游戏", enabled
    // State 3: Ready — "启动游戏", enabled
    property int btnLaunchState: 0
    property string statusText: "正在加载版本列表，请稍候"

    readonly property string btnLaunchText: {
        switch (page.btnLaunchState) {
        case 0:
            return "正在加载";
        case 1:
            return "启动游戏";
        case 2:
            return "下载游戏";
        case 3:
            return "启动游戏";
        default:
            return "启动游戏";
        }
    }

    // qmllint disable unqualified
    function refreshButtonsUI() {
        if (VersionManager.isLoading) {
            page.btnLaunchState = 0;
            page.statusText = "正在加载中，请稍候";
        } else if (VersionManager.versionIds.length === 0) {
            page.btnLaunchState = 2;
            page.statusText = "未找到可用的游戏版本";
        } else if (!page.selectedVersion) {
            page.btnLaunchState = 1;
            page.statusText = "未找到可用的游戏版本";
        } else {
            page.btnLaunchState = 3;
            page.statusText = page.selectedVersion;  // Original shows raw version name
        }
    }
    // qmllint enable unqualified

    // Launch state
    // qmllint disable unqualified
    readonly property bool isLaunching: {
        var s = Launcher.launchState;
        return s >= Launcher.Prechecking && s < Launcher.Running;
    }
    readonly property bool isRunning: Launcher.launchState === Launcher.Running
    // qmllint enable unqualified

    // PanInput ↔ PanLaunching transition animations
    // TO launching: PanInput fades+zooms out, PanLaunching bounces in
    // BACK to input: PanLaunching shrinks+fades, PanInput bounces back
    property real panInputOpacity: 1
    property real panInputScale: 1
    property real panLaunchingOpacity: 0
    property real panLaunchingScale: 0.8

    function pageChangeToLaunching() {
        page.panLaunchingTitle = "正在启动游戏";
        page.showProgress = 0;
        // Match original timing: PanInput fades immediately, PanLaunching delayed 100ms
        panInputFadeOut.start();
        panInputZoomOut.start();
        panLaunchingDelay.start();
    }

    function pageChangeToLogin() {
        // Match original: PanLaunching shrinks immediately, PanInput delayed 50ms
        panLaunchingFadeOut.start();
        panLaunchingShrink.start();
        panInputDelay.start();
        page.refreshButtonsUI();
    }

    // PanInput animations
    NumberAnimation {
        id: panInputFadeOut
        target: page
        property: "panInputOpacity"
        to: 0
        duration: 50
    }
    NumberAnimation {
        id: panInputZoomOut
        target: page
        property: "panInputScale"
        to: 1.2
        duration: 160
    }
    NumberAnimation {
        id: panInputFadeIn
        target: page
        property: "panInputOpacity"
        to: 1
        duration: 250
    }
    NumberAnimation {
        id: panInputScaleIn
        target: page
        property: "panInputScale"
        to: 1
        duration: 300
        easing.type: Easing.OutBack
    }

    // PanLaunching animations
    NumberAnimation {
        id: panLaunchingFadeIn
        target: page
        property: "panLaunchingOpacity"
        to: 1
        duration: 150
    }
    NumberAnimation {
        id: panLaunchingScaleIn
        target: page
        property: "panLaunchingScale"
        to: 1
        duration: 500
        easing.type: Easing.OutBack
    }
    NumberAnimation {
        id: panLaunchingFadeOut
        target: page
        property: "panLaunchingOpacity"
        to: 0
        duration: 150
    }
    NumberAnimation {
        id: panLaunchingShrink
        target: page
        property: "panLaunchingScale"
        to: 0.8
        duration: 150
    }

    // Delay timers matching original animation sequence
    Timer {
        id: panLaunchingDelay
        interval: 100
        repeat: false
        onTriggered: {
            panLaunchingFadeIn.start();
            panLaunchingScaleIn.start();
        }
    }
    Timer {
        id: panInputDelay
        interval: 50
        repeat: false
        onTriggered: {
            panInputFadeIn.start();
            panInputScaleIn.start();
        }
    }

    // Launching refresh (100ms polling, matching LaunchingRefresh)
    property real showProgress: 0
    property string panLaunchingTitle: "正在启动游戏"

    Timer {
        id: launchingRefreshTimer
        interval: 100
        running: page.isLaunching || page.isRunning
        repeat: true
        onTriggered: page.launchingRefresh()
    }

    // qmllint disable unqualified
    function launchingRefresh() {
        if (page.isRunning) {
            page.panLaunchingTitle = "已启动游戏";
        } else if (Launcher.launchState === Launcher.Failed) {
            page.panLaunchingTitle = "启动失败";
        } else if (Launcher.launchState === Launcher.Finished) {
            page.panLaunchingTitle = "游戏已退出";
        } else {
            page.panLaunchingTitle = "正在启动游戏";
        }

        var actual = Launcher.progress;
        if (actual < page.showProgress) {
            page.showProgress = actual;
        } else if (page.isRunning) {
            page.showProgress = 100;
        } else {
            page.showProgress += (actual - page.showProgress) * 0.1 + 0.0025;
            if (page.showProgress > actual)
                page.showProgress = actual;
        }
    }
    // qmllint enable unqualified

    // Sub-page navigation (version selection / player panel)
    property bool isSubPage: false
    property string subPageTitle: ""
    property string subPageId: "version"     // version=版本选择 player=玩家面板
    property string currentAvatar: "Steve"   // 正版登录视图本地头像（Steve ↔ Alex 切换）

    function pushSubPage(title, id) {
        page.subPageTitle = title;
        page.subPageId = id !== undefined ? id : "version";
        page.isSubPage = true;
    }
    function popSubPage() {
        page.isSubPage = false;
        page.subPageTitle = "";
    }

    // ---- 实例设置子页数据（数据源：InstanceBridge 单例） ----
    property var instanceInfoData: ({})  // 当前实例信息 {dirName, path, version, modCount, exists}
    property var modListData: []         // 当前实例 Mod 列表 [{fileName, size, enabled}]

    // qmllint disable unqualified
    function refreshInstanceInfo() {
        if (page.selectedVersion === "") {
            page.instanceInfoData = ({});
            return;
        }
        page.instanceInfoData = InstanceBridge.instanceInfo(page.selectedVersion);
    }

    function refreshModList() {
        if (page.selectedVersion === "") {
            page.modListData = [];
            return;
        }
        page.modListData = InstanceBridge.modList(page.selectedVersion);
    }
    // qmllint enable unqualified

    // 仅当实例子页处于打开状态时刷新概览与 Mod 列表
    function refreshInstanceSubPage() {
        if (!page.isSubPage || page.subPageId !== "instance")
            return;
        page.refreshInstanceInfo();
        page.refreshModList();
    }

    // 进入实例子页 / 子页内切换选中实例时刷新
    onIsSubPageChanged: page.refreshInstanceSubPage()
    onSubPageIdChanged: page.refreshInstanceSubPage()
    onSelectedVersionChanged: page.refreshInstanceSubPage()

    // Mod 显示名：去掉 .jar / .jar.disabled 后缀
    function modDisplayName(fileName) {
        var n = fileName;
        if (n.endsWith(".disabled"))
            n = n.substring(0, n.length - 9);
        if (n.toLowerCase().endsWith(".jar"))
            n = n.substring(0, n.length - 4);
        return n;
    }

    // Mod 大小：>=1MB 显示 MB，否则 KB
    function formatModSize(bytes) {
        if (bytes >= 1048576)
            return (bytes / 1048576).toFixed(1) + " MB";
        return Math.max(1, Math.round(bytes / 1024)) + " KB";
    }

    // 删除单个 Mod：warn 确认后调用 InstanceBridge，失败轻提示（成功由 modsChanged 驱动刷新）
    function confirmDeleteMod(fileName) {
        // qmllint disable missing-property
        Window.window.showMsg({
            title: "删除 Mod",
            text: "确定删除 Mod " + fileName + "？",
            warn: true,
            button1: "删除",
            button2: "取消",
            callback: function (result) {
                if (result !== 1)
                    return;
                // qmllint disable unqualified
                var ok = InstanceBridge.deleteMod(page.selectedVersion, fileName);
                // qmllint enable unqualified
                if (!ok) {
                    // qmllint disable missing-property
                    Window.window.showHint("Mod 删除失败", "error");
                }
            }
        });
    }

    // 删除当前实例：warn 确认 → 成功则返回主界面并刷新版本列表，失败弹窗提示
    function confirmRemoveInstance() {
        var name = page.selectedVersion;
        // qmllint disable missing-property
        Window.window.showMsg({
            title: "删除实例",
            text: "确定删除实例 " + name + "？该操作不可恢复",
            warn: true,
            button1: "删除",
            button2: "取消",
            callback: function (result) {
                if (result !== 1)
                    return;
                // qmllint disable unqualified
                var ok = InstanceBridge.removeInstance(name);
                // qmllint enable unqualified
                if (!ok) {
                    // qmllint disable missing-property
                    Window.window.showMsg({
                        title: "删除失败",
                        text: "无法删除实例 " + name,
                        warn: true
                    });
                    return;
                }
                // qmllint disable missing-property
                Window.window.showHint("已删除实例 " + name, "success");
                page.popSubPage();
                // 清空已删除的选中项，版本列表刷新后自动回退到首个实例
                page.selectedVersion = "";
                page.refreshVersions();
            }
        });
    }

    // Launch state watcher
    Connections {
        // qmllint disable unqualified
        target: Launcher
        function onStateChanged() {
            var s = Launcher.launchState;
            if (s >= Launcher.Prechecking && s < Launcher.Running) {
                page.pageChangeToLaunching();
            } else if (s === Launcher.Finished || s === Launcher.Failed || s === Launcher.Interrupted) {
                page.pageChangeToLogin();
            }
        }
        // qmllint enable unqualified
    }

    // Player profiles changed → refresh panel list
    Connections {
        // qmllint disable unqualified
        target: PlayerBridge
        // qmllint enable unqualified
        function onPlayersChanged() {
            page.refreshPlayers();
        }
    }

    // Mod 启用/禁用/删除 → 重新拉取实例概览与 Mod 列表
    Connections {
        // qmllint disable unqualified
        target: InstanceBridge
        // qmllint enable unqualified
        function onModsChanged(displayName) {
            if (displayName === page.selectedVersion) {
                page.refreshInstanceInfo();
                page.refreshModList();
            }
        }
    }

    // External file drag-and-drop (only acts when this page is active)
    Connections {
        // qmllint disable unqualified
        target: FileDropHandler
        function onDragEntered() {
            if (page.isActive)
                page.dragHovering = true;
        }
        function onDragExited() {
            page.dragHovering = false;
        }
        function onFilesDropped(files) {
            page.handleFilesDropped(files);
        }
    }

    // Version list loaded
    Connections {
        target: VersionManager
        function onVersionListChanged() {
            page.refreshButtonsUI();
            if (VersionManager.versionIds.length > 0 && !page.selectedVersion) {
                page.selectedVersion = VersionManager.versionIds[0];
            }
        }
        function onLoadingChanged() {
            page.refreshButtonsUI();
        }
    }

    // Main layout: left sidebar (300px) + right content
    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Left sidebar ----
        Rectangle {
            Layout.preferredWidth: Theme.sidebarWidth
            Layout.fillHeight: true
            color: Theme.sidebarBg

            // Sidebar shadow
            Rectangle {
                anchors {
                    right: parent.right
                    top: parent.top
                    bottom: parent.bottom
                }
                width: 4
                opacity: Theme.shadowOpacity
                gradient: Gradient {
                    GradientStop {
                        position: 0
                        color: "#000000"
                    }
                    GradientStop {
                        position: 1
                        color: "#00000000"
                    }
                }
            }

            // ---- Main view (launch sidebar) ----
            Item {
                id: mainView
                anchors.fill: parent
                visible: !page.isSubPage

                // PanInput — Default mode
                Item {
                    id: panInput
                    anchors.fill: parent
                    visible: page.panInputOpacity > 0
                    opacity: page.panInputOpacity
                    scale: page.panInputScale
                    transformOrigin: Item.Center

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        // Login type buttons
                        RowLayout {
                            Layout.preferredHeight: 35
                            Layout.topMargin: 22
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 8

                            MLCButton {
                                id: msBtn
                                Layout.preferredWidth: 80
                                padding: 5
                                radius: height / 2
                                colorType: page.loginType === 5 ? 1 : 0
                                hasBorder: false
                                contentItem: Row {
                                    spacing: 6
                                    MLCIcon {
                                        size: 16
                                        lucideIcon: "shield-check"
                                        anchors.verticalCenter: parent.verticalCenter
                                        iconColor: msBtn.labelColor
                                    }
                                    Text {
                                        text: "正版"
                                        color: msBtn.labelColor
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSize
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                onClicked: {
                                    page.loginType = 5;
                                }
                            }
                            MLCButton {
                                id: offlineBtn
                                Layout.preferredWidth: 80
                                padding: 5
                                radius: height / 2
                                colorType: page.loginType === 0 ? 1 : 0
                                hasBorder: false
                                contentItem: Row {
                                    spacing: 6
                                    MLCIcon {
                                        size: 16
                                        lucideIcon: "unlink"
                                        anchors.verticalCenter: parent.verticalCenter
                                        iconColor: offlineBtn.labelColor
                                    }
                                    Text {
                                        text: "离线"
                                        color: offlineBtn.labelColor
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSize
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                onClicked: {
                                    page.loginType = 0;
                                }
                            }
                        }

                        Item {
                            Layout.fillHeight: true
                        }

                        // 正版 — 微软登录（device code flow）
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 110
                            visible: page.loginType === 5

                            // 未登录：登录按钮
                            ColumnLayout {
                                anchors.centerIn: parent
                                spacing: Theme.itemSpacing
                                visible: !page.loginResult && !page.msPolling

                                MLCIcon {
                                    Layout.alignment: Qt.AlignHCenter
                                    size: 48
                                    lucideIcon: "shield-check"
                                    iconColor: Theme.gray3
                                    opacity: Theme.textOpacityDim
                                }
                                MLCButton {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 150
                                    text: "登录 Microsoft 账户"
                                    colorType: 1
                                    onClicked: page.startMsLogin()
                                }
                            }

                            // 轮询中：设备代码 + 验证链接
                            ColumnLayout {
                                anchors.centerIn: parent
                                spacing: 6
                                visible: page.msPolling

                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "请在浏览器中打开："
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.gray3
                                }
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: page.msVerifyUrl || ""
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSize
                                    color: Theme.color2
                                }
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "输入代码：" + (page.msDeviceCode || "")
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSize
                                    color: Theme.color1
                                }
                                MLCButton {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 60
                                    text: "取消"
                                    colorType: 2
                                    onClicked: {
                                        // 必须显式取消后端轮询，否则迟到的成功结果
                                        // 会在切回离线后仍写入 loginResult 并被用于启动
                                        msAuth.cancel();
                                        page.msPolling = false;
                                        page.loginType = 0;
                                    }
                                }
                            }

                            // 已登录：头像 + 名称 + 切换
                            ColumnLayout {
                                anchors.centerIn: parent
                                spacing: 12
                                visible: page.loginResult !== null && !page.msPolling

                                MLCIcon {
                                    Layout.alignment: Qt.AlignHCenter
                                    size: 48
                                    assetsIcon: page.currentAvatar
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: page.currentAvatar = (page.currentAvatar === "Steve") ? "Alex" : "Steve"
                                    }
                                }
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: page.loginResult ? page.loginResult.name : ""
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeLaunchName
                                    font.bold: true
                                    color: Theme.color1
                                }
                                MLCButton {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 100
                                    text: "切换账户"
                                    colorType: 0
                                    onClicked: {
                                        page.loginResult = null;
                                        page.accountName = "";
                                        page.accountUuid = "";
                                        page.startMsLogin();
                                    }
                                }
                            }
                        }

                        // 离线模式
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 110
                            visible: page.loginType === 0

                            // 当前玩家：头像 + 名字（点击展开玩家面板）
                            Item {
                                id: playerChip
                                anchors {
                                    horizontalCenter: parent.horizontalCenter
                                    top: parent.top
                                }
                                width: parent.width - 40
                                height: 50

                                MLCIcon {
                                    id: pageIcoTwo
                                    anchors {
                                        left: parent.left
                                        verticalCenter: parent.verticalCenter
                                    }
                                    size: 46
                                    assetsIcon: page.offlineAvatar
                                }
                                Column {
                                    anchors {
                                        left: pageIcoTwo.right
                                        leftMargin: 12
                                        right: parent.right
                                        verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        width: parent.width
                                        text: PlayerBridge.currentName || "未登录"
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeLarge
                                        font.bold: true
                                        color: Theme.color1
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: "点击切换玩家"
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.gray3
                                        opacity: Theme.textOpacityDim
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: page.pushSubPage("玩家管理", "player")
                                }
                            }

                            // Username input
                            MLCTextBox {
                                anchors {
                                    horizontalCenter: parent.horizontalCenter
                                    top: playerChip.bottom
                                    topMargin: Theme.cardPadding
                                }
                                width: parent.width - 40
                                placeholderText: "游戏用户名"
                                text: page.accountName || ""
                                onTextChanged: {
                                    page.accountName = text;
                                }
                            }
                        }

                        // Spacer
                        Item {
                            Layout.fillHeight: true
                        }

                        // Launch button + version status
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 75

                            MLCButton {
                                id: btnLaunch
                                anchors {
                                    left: parent.left
                                    right: parent.right
                                    top: parent.top
                                }
                                anchors.leftMargin: 20
                                anchors.rightMargin: 20
                                height: Theme.launchBtnHeight
                                colorType: (page.btnLaunchState === 3 || page.btnLaunchState === 2) ? 1 : 3
                                text: page.btnLaunchText
                                enabled: page.btnLaunchState >= 2
                                padding: 0
                                onClicked: page.launchButtonClick()
                            }
                        }

                        // Version select + version settings
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 45
                            Layout.leftMargin: 20
                            Layout.rightMargin: 20

                            MLCButton {
                                id: btnVersion
                                Layout.fillWidth: true
                                Layout.preferredHeight: Theme.versionBtnHeight
                                colorType: 0
                                text: "版本选择"   // Original always shows "版本选择"
                                enabled: page.btnLaunchState !== 0
                                onClicked: page.pushSubPage("版本选择")
                            }
                            MLCButton {
                                id: btnMore
                                Layout.preferredWidth: 80
                                Layout.preferredHeight: Theme.versionBtnHeight
                                colorType: 0
                                text: "版本设置"
                                visible: page.btnLaunchState === 3
                                onClicked: page.pushSubPage("版本设置", "instance")
                            }
                        }

                        Item {
                            Layout.preferredHeight: 20
                        }
                    }
                }

                // PanLaunching — Launch progress overlay
                Item {
                    id: panLaunching
                    anchors.fill: parent
                    visible: page.panLaunchingOpacity > 0
                    opacity: page.panLaunchingOpacity
                    scale: page.panLaunchingScale
                    transformOrigin: Item.Center

                    // Content centered vertically (matching original Grid with * rows)
                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - 40
                        spacing: 0

                        Item {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 50
                            Layout.preferredHeight: 50
                            Layout.topMargin: 10
                            MLCProgressBar {
                                anchors.fill: parent
                                indeterminate: Launcher.launchState < Launcher.Downloading
                            }
                        }

                        Text {
                            id: launchTitleText
                            text: page.panLaunchingTitle
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeLaunchTitle
                            color: Theme.color3
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 10
                            transform: Rotation {
                                angle: -3
                                origin.x: launchTitleText.width / 2
                                origin.y: launchTitleText.height / 2
                            }
                        }
                        Text {
                            id: launchNameText
                            text: page.selectedVersion
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeLaunchName
                            color: Theme.color3
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 5
                            Layout.bottomMargin: 12
                            transform: Rotation {
                                angle: -3
                                origin.x: launchNameText.width / 2
                                origin.y: launchNameText.height / 2
                            }
                        }

                        // Progress bar (margin 30,12,30,27 matching original)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Theme.progressBarHeight
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            Layout.topMargin: 12
                            Layout.bottomMargin: 27
                            radius: 0
                            color: Theme.gray6
                            opacity: Theme.textOpacityHint
                            Rectangle {
                                width: parent.width * (page.showProgress / 100.0)
                                height: parent.height
                                gradient: Gradient {
                                    GradientStop {
                                        position: 0
                                        color: Theme.color4
                                    }
                                    GradientStop {
                                        position: 0.6
                                        color: Theme.color3
                                    }
                                }
                                Behavior on width {
                                    NumberAnimation {
                                        duration: 130
                                    }
                                }
                            }
                        }

                        // Status info grid (matching PanLaunchingInfo)
                        GridLayout {
                            Layout.alignment: Qt.AlignHCenter
                            columns: 2
                            rowSpacing: 5
                            columnSpacing: 15
                            Text {
                                text: "当前步骤"
                                font: page.infoFont
                                color: Theme.gray3
                                opacity: Theme.textOpacityDim
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: 80
                            }
                            Text {
                                text: Launcher.statusText
                                font: page.infoFont
                                color: Theme.color3
                                Layout.preferredWidth: 100
                            }
                            Text {
                                text: "登录方式"
                                font: page.infoFont
                                color: Theme.gray3
                                opacity: Theme.textOpacityDim
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: 80
                            }
                            Text {
                                text: page.loginType === 5 ? "登录" : "离线登录"
                                font: page.infoFont
                                color: Theme.color3
                                Layout.preferredWidth: 100
                            }
                            Text {
                                text: "启动进度"
                                font: page.infoFont
                                color: Theme.gray3
                                opacity: Theme.textOpacityDim
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: 80
                            }
                            Text {
                                text: page.showProgress.toFixed(2) + " %"
                                font: page.infoFont
                                color: Theme.color3
                                Layout.preferredWidth: 100
                            }
                            Text {
                                text: "下载速度"
                                font: page.infoFont
                                color: Theme.gray3
                                opacity: Theme.textOpacityDim
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: 80
                                visible: false
                            }
                            Text {
                                text: ""
                                font: page.infoFont
                                color: Theme.color3
                                Layout.preferredWidth: 100
                                visible: false
                            }
                        }
                    }

                    // Cancel button at bottom (full-width, matching original Margin="20,0,20,20" Height=35)
                    MLCButton {
                        anchors {
                            left: parent.left
                            right: parent.right
                            bottom: parent.bottom
                        }
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        anchors.bottomMargin: 20
                        height: Theme.versionBtnHeight
                        text: page.isRunning ? "结束游戏" : "取消"
                        colorType: 2
                        onClicked: Launcher.interrupt()
                    }
                }
            }

            // ---- 子页容器：版本选择 / 玩家管理 / 实例设置均为 mainView 的兄弟节点 ----
            // （mainView 的 visible: !page.isSubPage 会连带隐藏子级，子页必须放它外面）

        // Version selection sub-page (replaces sidebar content)
        // Matching original PageSelectLeft + PageSelectRight navigation
            Item {
                id: versionSelectView
                anchors.fill: parent
                visible: page.isSubPage && page.subPageId === "version"
                opacity: visible ? 1 : 0
                Behavior on opacity {
                    NumberAnimation {
                        duration: 100
                    }
                }

                // Loading spinner overlay
                Rectangle {
                    anchors.fill: parent
                    color: Theme.pureWhite
                    visible: VersionManager.isLoading
                    z: 1

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 12
                        MLCProgressBar {
                            Layout.preferredWidth: 50
                            Layout.preferredHeight: 50
                            indeterminate: true
                        }
                        Text {
                            text: "正在扫描版本..."
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSize
                            color: Theme.gray3
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }

                // Version list (matching original PageSelectRight layout)
                Flickable {
                    anchors.fill: parent
                    visible: !VersionManager.isLoading
                    contentWidth: width
                    contentHeight: verCol.implicitHeight + 20
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: MLCScrollBar {}

                    ColumnLayout {
                        id: verCol
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                        }
                        spacing: 2

                        // Section header
                        Text {
                            text: "常规版本"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityHint
                            Layout.leftMargin: 15
                            Layout.topMargin: 12
                            Layout.bottomMargin: 4
                        }

                        Repeater {
                            model: VersionManager.versionIds

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                Layout.leftMargin: 5
                                Layout.rightMargin: 5
                                radius: Theme.buttonRadius
                                color: verMouse.containsMouse ? Theme.color7 : "transparent"

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 8
                                    spacing: Theme.itemSpacing

                                    Rectangle {
                                        implicitWidth: 24
                                        implicitHeight: 24
                                        radius: 3
                                        color: Theme.color6
                                        Layout.alignment: Qt.AlignVCenter
                                        Text {
                                            anchors.centerIn: parent
                                            text: "⛏"
                                            font.pixelSize: 14
                                        }
                                    }
                                    Text {
                                        // qmllint disable unqualified
                                        text: modelData
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSize
                                        color: Theme.color1
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }
                                }

                                MouseArea {
                                    id: verMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        // qmllint disable unqualified
                                        page.selectedVersion = modelData;
                                        page.refreshButtonsUI();
                                        page.popSubPage();
                                    }
                                }
                            }
                        }

                        Item {
                            Layout.preferredHeight: 10
                        }
                    }
                }

                // Back button overlay (top-right corner, matching original PageBack)
                MLCButton {
                    anchors {
                        right: parent.right
                        top: parent.top
                        margins: 10
                    }
                    width: 60
                    height: 30
                    text: "← 返回"
                    colorType: 0
                    z: 5
                    onClicked: page.popSubPage()
                }
            }

            // Player panel sub-page (replaces sidebar content, same navigation as version selection)
            Item {
                id: playerPanelView
                anchors.fill: parent
                visible: page.isSubPage && page.subPageId === "player"
                opacity: visible ? 1 : 0
                Behavior on opacity {
                    NumberAnimation {
                        duration: 100
                    }
                }

                // Player list (matching version list layout)
                Flickable {
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: playerCol.implicitHeight + 20
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: MLCScrollBar {}

                    ColumnLayout {
                        id: playerCol
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                        }
                        spacing: 2

                        // Section header
                        Text {
                            text: "玩家列表"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityHint
                            Layout.leftMargin: 15
                            Layout.topMargin: 12
                            Layout.bottomMargin: 4
                        }

                        Repeater {
                            model: page.playerListData

                            MLCListItem {
                                id: playerItem
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                Layout.leftMargin: 5
                                Layout.rightMargin: 5
                                title: playerItem.modelData.name
                                info: playerItem.modelData.skinType === "wide" ? "Steve 皮肤" : "Alex 皮肤"
                                imageSource: "qrc:/gui/assets/"
                                             + (playerItem.modelData.skinType === "wide" ? "Steve" : "Alex") + ".svg"
                                checkType: 2
                                checked: playerItem.modelData.selected
                                onClicked: PlayerBridge.selectPlayer(playerItem.modelData.uuid)

                                buttons: [
                                    MLCIconButton {
                                        lucideIcon: "x"
                                        theme: 3
                                        onClicked: page.confirmRemovePlayer(playerItem.modelData.uuid,
                                                                            playerItem.modelData.name)
                                    }
                                ]
                            }
                        }

                        // Section header
                        Text {
                            text: "操作"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityHint
                            Layout.leftMargin: 15
                            Layout.topMargin: 8
                            Layout.bottomMargin: 4
                        }

                        // 添加玩家入口：使用离线输入框中的用户名
                        MLCListItem {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            Layout.leftMargin: 5
                            Layout.rightMargin: 5
                            title: "添加玩家"
                            info: "使用输入框中的用户名"
                            lucideIcon: "file-plus"
                            checkType: 1
                            onClicked: page.addPlayerFromInput()
                        }

                        Item {
                            Layout.preferredHeight: 10
                        }
                    }
                }

                // Back button overlay (top-right corner, matching original PageBack)
                MLCButton {
                    anchors {
                        right: parent.right
                        top: parent.top
                        margins: 10
                    }
                    width: 60
                    height: 30
                    text: "← 返回"
                    colorType: 0
                    z: 5
                    onClicked: page.popSubPage()
                }
            }

            // ---- 实例设置子页（复用子页容器/动画/返回按钮模式） ----
            // 注意：作为 mainView 的兄弟节点而非子级，避免被 mainView 的
            // visible: !page.isSubPage 连带隐藏（QML 父级不可见时子级不渲染）
            Item {
                id: instanceView
                anchors.fill: parent
                visible: page.isSubPage && page.subPageId === "instance"
                opacity: visible ? 1 : 0
                Behavior on opacity {
                    NumberAnimation {
                        duration: 100
                    }
                }

                // 内容滚动区（同玩家面板布局）
                Flickable {
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: instCol.implicitHeight + 20
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: MLCScrollBar {}

                    ColumnLayout {
                        id: instCol
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                        }
                        spacing: 2

                        // ---- 概览区 ----
                        Text {
                            text: "概览"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityHint
                            Layout.leftMargin: 15
                            Layout.topMargin: 12
                            Layout.bottomMargin: 4
                        }

                        // 实例名（大字）
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 15
                            Layout.rightMargin: 15
                            text: page.selectedVersion
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeLogo
                            font.bold: true
                            color: Theme.color1
                            elide: Text.ElideRight
                        }

                        // 版本与 Mod 数量
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 15
                            Layout.rightMargin: 15
                            Layout.topMargin: 2
                            text: "版本 " + (page.instanceInfoData.version || "未知") + " · "
                                  + (page.instanceInfoData.modCount || 0) + " 个 Mod"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            elide: Text.ElideRight
                        }

                        // 实例路径（灰色小字，中部省略）
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 15
                            Layout.rightMargin: 15
                            text: page.instanceInfoData.path || ""
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityDim
                            elide: Text.ElideMiddle
                        }

                        // 打开实例文件夹
                        MLCButton {
                            Layout.leftMargin: 15
                            Layout.topMargin: 6
                            Layout.preferredWidth: 110
                            text: "打开文件夹"
                            colorType: 0
                            enabled: (page.instanceInfoData.path || "") !== ""
                            onClicked: Qt.openUrlExternally("file://" + page.instanceInfoData.path)
                        }

                        // ---- Mod 管理区 ----
                        Text {
                            text: "Mod 管理（" + page.modListData.length + " 个）"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityHint
                            Layout.leftMargin: 15
                            Layout.topMargin: 10
                            Layout.bottomMargin: 4
                        }

                        Repeater {
                            model: page.modListData

                            MLCListItem {
                                id: modItem
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                Layout.leftMargin: 5
                                Layout.rightMargin: 5
                                title: page.modDisplayName(modItem.modelData.fileName)
                                info: page.formatModSize(modItem.modelData.size)
                                      + (modItem.modelData.enabled ? "" : " · 已禁用")
                                lucideIcon: "file-archive"

                                buttons: [
                                    // 启用/禁用开关：失败时轻提示并还原勾选态
                                    MLCCheckBox {
                                        id: modCheck
                                        height: 28
                                        checked: modItem.modelData.enabled
                                        onChanged: function (byUser) {
                                            if (!byUser)
                                                return;
                                            // qmllint disable unqualified
                                            var ok = InstanceBridge.setModEnabled(page.selectedVersion,
                                                                                  modItem.modelData.fileName,
                                                                                  modCheck.checked);
                                            // qmllint enable unqualified
                                            if (!ok) {
                                                // qmllint disable missing-property
                                                Window.window.showHint("Mod 状态切换失败", "error");
                                                // 还原勾选态（触发 changed(false)，被 byUser 守卫忽略）
                                                modCheck.checked = !modCheck.checked;
                                            }
                                        }
                                    },
                                    // 删除 Mod（悬停出现，红色）
                                    MLCIconButton {
                                        lucideIcon: "x"
                                        theme: 3
                                        // qmllint disable unqualified
                                        onClicked: page.confirmDeleteMod(modItem.modelData.fileName)
                                        // qmllint enable unqualified
                                    }
                                ]
                            }
                        }

                        // 空列表占位
                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            Layout.bottomMargin: 6
                            visible: page.modListData.length === 0
                            text: "该实例没有 Mod"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityDim
                            horizontalAlignment: Text.AlignHCenter
                        }

                        // ---- 危险区 ----
                        Text {
                            text: "危险区"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.gray3
                            opacity: Theme.textOpacityHint
                            Layout.leftMargin: 15
                            Layout.topMargin: 10
                            Layout.bottomMargin: 4
                        }

                        MLCButton {
                            Layout.fillWidth: true
                            Layout.leftMargin: 5
                            Layout.rightMargin: 5
                            Layout.preferredHeight: Theme.versionBtnHeight
                            text: "删除该实例"
                            colorType: 2
                            enabled: page.selectedVersion !== ""
                            onClicked: page.confirmRemoveInstance()
                        }

                        Item {
                            Layout.preferredHeight: 10
                        }
                    }
                }

                // 返回按钮（右上角，同其他子页）
                MLCButton {
                    anchors {
                        right: parent.right
                        top: parent.top
                        margins: 10
                    }
                    width: 60
                    height: 30
                    text: "← 返回"
                    colorType: 0
                    z: 5
                    onClicked: page.popSubPage()
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
                contentHeight: panMain.implicitHeight + 25
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: MLCScrollBar {}

                ColumnLayout {
                    id: panMain
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                    }
                    anchors.margins: Theme.contentMargin
                    spacing: Theme.sectionSpacing

                    // Quick launch card
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: quickCardCol.height + 36
                        radius: Theme.buttonRadius
                        color: Theme.pureWhite
                        border {
                            width: 1
                            color: Theme.gray5
                        }

                        ColumnLayout {
                            id: quickCardCol
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                            }
                            anchors.margins: 20
                            spacing: 12

                            Text {
                                text: "添加"
                                font: page.boldFont
                                color: Theme.color1
                            }
                        }
                    }

                    // Log card
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 150
                        radius: Theme.buttonRadius
                        color: Theme.pureWhite
                        border {
                            width: 1
                            color: Theme.gray5
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 0
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 38
                                color: "transparent"
                                Text {
                                    anchors {
                                        left: parent.left
                                        top: parent.top
                                        leftMargin: Theme.cardPadding
                                        topMargin: 18
                                    }
                                    text: "启动日志"
                                    font: page.boldFont
                                    color: Theme.color1
                                }
                            }
                            Flickable {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 23
                                Layout.bottomMargin: 18
                                clip: true
                                contentWidth: width
                                contentHeight: labLog.implicitHeight
                                ScrollBar.vertical: MLCScrollBar {}
                                Text {
                                    id: labLog
                                    width: parent.width
                                    text: "MLC v" + Qt.application.version + "\nReady.\n"
                                    color: Theme.color1
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSize
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }
            }

            Connections {
                // qmllint disable unqualified
                target: Launcher
                // qmllint enable unqualified
                function onGameLog(line) {
                    labLog.text += line + "\n";
                }
            }
        }
    }

    // ---- MS OAuth ----
    // qmllint disable import
    MsAuth {
        id: msAuth
        onDeviceCodeReady: function (code, url) {
            page.msDeviceCode = code;
            page.msVerifyUrl = url;
        }
        onLoginProgress: function (status) {
            page.statusText = status;
        }
        onLoginFinished: function (success, result) {
            page.msPolling = false;
            if (success) {
                page.accountName = result.name;
                page.accountUuid = result.uuid;
                page.loginResult = result;
                page.statusText = "已登录: " + page.accountName;
            } else {
                page.loginType = 0;
            }
        }
    }

    // ---- Launch button click (matching LaunchButtonClick) ----
    function launchButtonClick() {
        if (page.btnLaunchState === 2) {
            // 无版本时按钮为「下载游戏」→ 跳到下载 tab
            // qmllint disable missing-property
            Window.window.switchTab(1);
            return;
        }
        if (page.btnLaunchState !== 3 && page.btnLaunchState !== 1)
            return;
        // 在线模式未登录 → 触发登录
        if (page.loginType === 5 && !page.loginResult) {
            page.startMsLogin();
            return;
        }
        page.doLaunch();
    }

    // qmllint disable unqualified
    function doLaunch() {
        if (!page.selectedVersion)
            return;
        var login = page.loginResult;
        // 在线模式未登录 → 不允许启动（双重保险）
        if (page.loginType === 5 && !login)
            return;
        // 离线模式绝不带在线登录态（防止取消登录后残留的 MS token 被误用）
        if (page.loginType !== 5)
            login = null;
        // 离线模式 → 输入了新名字则先落库为玩家（已存在同名直接选中），再用选中玩家生成登录态
        if (login === null) {
            var typedName = page.accountName.trim();
            if (typedName !== "" && typedName !== PlayerBridge.currentName)
                page.ensurePlayer(typedName);
            login = PlayerBridge.createLogin();
        }
        Launcher.launchVersion(page.selectedVersion, login);
    }
    // qmllint enable unqualified

    function startMsLogin() {
        // 防重入：轮询进行中再点启动不得重新发起登录（旧会话会被顶掉并污染状态）
        if (page.msPolling)
            return;
        page.loginResult = null;
        page.msPolling = true;
        msAuth.startLogin();
    }

    // qmllint disable unqualified
    function refreshVersions() {
        VersionManager.loadLocalVersions();
    }
    // qmllint enable unqualified

    // ---- Fonts ----
    readonly property font smallFont: Qt.font({
        family: Theme.fontFamily,
        pixelSize: Theme.fontSizeSmall
    })
    readonly property font infoFont: Qt.font({
        family: Theme.fontFamily,
        pixelSize: Theme.fontSizeLaunchLabel
    })
    readonly property font boldFont: Qt.font({
        family: Theme.fontFamily,
        pixelSize: Theme.fontSizeLaunchName,
        bold: true
    })

    // ---- Keyboard shortcuts ----
    Keys.onReturnPressed: {
        if (page.isActive && !page.isSubPage)
            page.launchButtonClick();
    }
    Keys.onEscapePressed: {
        if (page.isSubPage)
            page.popSubPage();
    }

    // ---- Initialize ----
    Component.onCompleted: {
        page.refreshButtonsUI();
        page.refreshPlayers();
    }

    // ---- Persistent DropArea (secondary pathway for external file drag-and-drop) ----
    DropArea {
        id: pageDropArea
        anchors.fill: parent
        z: 998
        keys: ["text/uri-list"]
        onEntered: function (drag) {
            if (page.isActive && drag.hasUrls) {
                page.dragHovering = true;
                drag.acceptProposedAction();
            }
        }
        onExited: {
            page.dragHovering = false;
        }
        onDropped: function (drop) {
            if (!page.isActive)
                return;
            var urls = drop.urls;
            var files = [];
            for (var i = 0; i < urls.length; i++) {
                files.push(urls[i].toString().replace(/^(file:\/\/)/i, ''));
            }
            if (files.length > 0) {
                page.handleFilesDropped(files);
            }
            drop.acceptProposedAction();
        }
    }

    // ---- Drag-and-drop visual overlay (appears when dragging over this page or an import is running) ----
    Rectangle {
        id: dragOverlay
        anchors.fill: parent
        z: 999
        radius: Theme.buttonRadius
        color: Qt.rgba(0, 0, 0, 0.2)
        // qmllint disable unqualified
        visible: page.dragHovering || InstallBridge.busy
        // qmllint enable unqualified
        opacity: visible ? 1 : 0
        Behavior on opacity {
            NumberAnimation {
                duration: 150
            }
        }

        // Dashed-border effect via a second rounded rect slightly inset
        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            radius: Theme.buttonRadius - 2
            color: "transparent"
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: Theme.itemSpacing
            MLCIcon {
                Layout.alignment: Qt.AlignHCenter
                size: 48
                // qmllint disable unqualified
                lucideIcon: InstallBridge.busy ? "package" : "file-plus"
                // qmllint enable unqualified
                iconColor: Theme.color2
                opacity: 0.7
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                // qmllint disable unqualified
                text: InstallBridge.busy ? page.dragDropResult : "释放文件以导入整合包"
                // qmllint enable unqualified
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeLaunchName
                font.bold: true
                color: Theme.color2
                opacity: 0.8
            }
        }

        // Dismiss on click-through (导入中点击无效，busy 保持悬浮层可见)
        MouseArea {
            anchors.fill: parent
            onClicked: page.dragHovering = false
        }
    }
}
