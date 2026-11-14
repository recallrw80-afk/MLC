pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MLC

// Download tab — 游戏安装与资源下载
// 左侧：分类导航（游戏安装默认选中） | 右侧：游戏安装页 / Mod·整合包·材质包·光影包资源浏览 / 数据包“即将推出”占位
Item {
    id: page
    property bool isActive: false
    visible: opacity > 0
    opacity: isActive ? 1 : 0
    scale: isActive ? 1 : 0.96
    Behavior on opacity { NumberAnimation { duration: 100 } }
    Behavior on scale { NumberAnimation { duration: 400; easing.type: Easing.OutBack } }

    // ---- 导航状态 ----
    property int selectedNav: 0
    property var currentNavItem: null
    property var navModel: [
        { name: "游戏安装", icon: "arrow-down-to-line" },
        { name: "Mod", icon: "package" },
        { name: "整合包", icon: "file-archive" },
        { name: "材质包", icon: "image" },
        { name: "光影包", icon: "bolt" },
        { name: "数据包", icon: "file-code" }
    ]

    // ---- 版本数据状态 ----
    // remoteVersions: VersionManager.remoteVersionList() 的缓存
    // installedIds: 本地已装版本 id（远程清单合入后 versionIds 会混入远程条目，
    //               故仅在列表处于“仅本地”状态时快照，安装成功后手动补充）
    // sessionInstalled: 本次会话内由本页安装成功的版本 id
    property var remoteVersions: []
    property var filteredVersions: []
    property var installedIds: []
    property var sessionInstalled: []
    property string pendingInstallId: ""

    // C++ 单例属性集中绑定（qmllint 无法解析单例，按项目惯例成对 disable）
    // qmllint disable unqualified
    readonly property bool versionsLoading: VersionManager.isLoading
    readonly property bool installBusy: InstallBridge.busy
    readonly property string installProgressText: InstallBridge.progressText
    readonly property int installProgressPercent: InstallBridge.progressPercent
    readonly property bool modSearching: ModPlatformBridge.searching
    readonly property bool modDownloading: ModPlatformBridge.downloading
    readonly property int modDownloadPercent: ModPlatformBridge.downloadPercent
    // qmllint enable unqualified

    // ---- 资源浏览状态（Mod/整合包/材质包/光影包共用一套工作区，对应当前分类） ----
    // modLastReq: 最近一次成功发起的搜索请求 {platform, category, query, page}，
    //             全局单例信号回调时据此校验，避免旧响应覆盖新列表
    // modSearchQueued: 搜索飞行中又触发新搜索时排队（bridge 互斥会吞掉并发请求），
    //                  待 searchFinished 后补发最新意图
    // resCache: 每分类最近一次搜索缓存 cat → {results, page, hasMore, platform, query}，
    //           切走再切回时若平台与关键词未变则直接恢复，不重搜
    // instanceList: 目标实例下拉快照（远程清单合入后 versionIds 会混入远程条目，
    //               与 installedIds 同理仅在“仅本地”状态时刷新）
    property int modPlatform: 0
    property int modPage: -1
    property var modResults: []
    property bool modSearched: false
    property bool modHasMore: false
    property var modLastReq: null
    property bool modSearchQueued: false
    property bool modQueuedReset: true
    property var resCache: ({})
    property var instanceList: []

    // ---- 文件选择面板状态 ----
    property bool filesVisible: false
    property bool filesLoading: false
    property string filesModId: ""
    property string filesModName: ""
    property int filesPlatform: 0
    property var fileList: []

    // ---- 下载状态（两个 Pending 标记由本页发起的下载/导入，回调据此过滤） ----
    property string downloadTargetInstance: ""
    property bool modDownloadPending: false
    property bool modpackImportPending: false
    property string pendingModpackName: ""

    readonly property string selectedNavName: selectedNav >= 0
                                              && selectedNav < navModel.length ? navModel[selectedNav].name : ""
    readonly property string selectedNavIcon: selectedNav >= 0
                                              && selectedNav < navModel.length ? navModel[selectedNav].icon : "package"
    // 当前资源分类：导航 1..4 → 分类 0..3（0=Mod 1=整合包 2=材质包 3=光影包），其余为 -1
    readonly property int currentCategory: selectedNav >= 1 && selectedNav <= 4 ? selectedNav - 1 : -1

    // ---- 导航单选（RadioBox 互斥由使用方维护） ----
    function selectNav(idx, item) {
        if (idx === selectedNav)
            return;
        selectedNav = idx;
        if (currentNavItem !== null && currentNavItem !== item)
            currentNavItem.checked = false;
        currentNavItem = item;
        // 切换分类一律关闭文件选择面板；进入资源分类时恢复缓存或自动搜一次
        closeFiles();
        if (currentCategory >= 0)
            enterCategory(currentCategory);
    }

    // ---- 进入资源分类：平台与关键词未变且有缓存则直接恢复，否则清空并自动搜一次（默认空串=热门） ----
    function enterCategory(cat) {
        var c = resCache[cat];
        if (c !== undefined && c.platform === modPlatform && c.query === modSearchBox.text.trim()) {
            modResults = c.results;
            modPage = c.page;
            modHasMore = c.hasMore;
            modSearched = true;
        } else {
            modResults = [];
            modPage = -1;
            modHasMore = false;
            modSearched = false;
            startModSearch(true);
        }
    }

    // ---- 从 VersionManager 同步远程清单与本地已装快照 ----
    function syncFromManager() {
        // qmllint disable unqualified
        var remote = VersionManager.remoteVersionList();
        var ids = VersionManager.versionIds;
        // qmllint enable unqualified
        remoteVersions = remote;
        if (remote.length === 0) {
            // 远程清单未合入时 versionIds 即本地已装列表；目标实例下拉同步快照
            instanceList = ids;
            var merged = ids.slice();
            for (var i = 0; i < sessionInstalled.length; i++) {
                if (merged.indexOf(sessionInstalled[i]) < 0)
                    merged.push(sessionInstalled[i]);
            }
            installedIds = merged;
        }
        rebuildFiltered();
    }

    // ---- 按搜索词与类型开关重建展示列表 ----
    function rebuildFiltered() {
        var kw = searchBox.text.trim().toLowerCase();
        var out = [];
        for (var i = 0; i < remoteVersions.length; i++) {
            var v = remoteVersions[i];
            if (!chkSnapshots.checked && v.type !== "release")
                continue;
            if (kw !== "" && String(v.id).toLowerCase().indexOf(kw) < 0)
                continue;
            var dateText = String(v.releaseTime).split("T")[0];
            out.push({
                vid: String(v.id),
                info: (dateText !== "" ? dateText + " · " : "") + typeLabel(String(v.type)),
                installed: installedIds.indexOf(v.id) >= 0
            });
        }
        filteredVersions = out;
    }

    // ---- 版本类型中文标签 ----
    function typeLabel(t) {
        switch (t) {
        case "release": return "正式版";
        case "snapshot": return "快照";
        case "old_alpha": return "远古版 Alpha";
        case "old_beta": return "远古版 Beta";
        default: return t;
        }
    }

    // ---- 拉取远程版本清单 ----
    function refreshRemote() {
        // qmllint disable unqualified
        VersionManager.fetchVersionManifest();
        // qmllint enable unqualified
    }

    // ---- 安装确认 → InstallBridge ----
    function confirmInstall(vid) {
        if (installBusy)
            return;
        pendingInstallId = vid;
        // qmllint disable missing-property
        Window.window.showMsg({
            title: "安装游戏",
            text: "下载并安装 Minecraft " + vid + "？",
            button1: "安装",
            button2: "取消",
            callback: function (result) {
                if (result === 1) {
                    // qmllint disable unqualified
                    InstallBridge.installMcVersion(vid);
                    // qmllint enable unqualified
                }
            }
        });
        // qmllint enable missing-property
    }

    // ---- 安装成功后记入已装列表 ----
    function markInstalled(vid) {
        if (vid === "")
            return;
        if (sessionInstalled.indexOf(vid) < 0) {
            var s = sessionInstalled.slice();
            s.push(vid);
            sessionInstalled = s;
        }
        if (installedIds.indexOf(vid) < 0) {
            var m = installedIds.slice();
            m.push(vid);
            installedIds = m;
        }
        rebuildFiltered();
    }

    // ---- 下载量格式化（如 12.3万） ----
    function formatCount(n) {
        var v = Number(n);
        if (isNaN(v) || v < 0)
            return "0";
        if (v >= 100000000)
            return (v / 100000000).toFixed(1) + "亿";
        if (v >= 10000)
            return (v / 10000).toFixed(1) + "万";
        return String(Math.floor(v));
    }

    // ---- 文件大小格式化 ----
    function formatSize(bytes) {
        var v = Number(bytes);
        if (isNaN(v) || v < 0)
            return "0 KB";
        if (v >= 1073741824)
            return (v / 1073741824).toFixed(2) + " GB";
        if (v >= 1048576)
            return (v / 1048576).toFixed(1) + " MB";
        return Math.max(1, Math.round(v / 1024)) + " KB";
    }

    // ---- 资源列表项副标题：作者 · 下载量 · 简介 ----
    function modInfoText(m) {
        var parts = [];
        var author = String(m.author);
        if (author !== "")
            parts.push(author);
        parts.push(formatCount(m.downloadCount) + " 次下载");
        var summary = String(m.summary);
        if (summary !== "")
            parts.push(summary);
        return parts.join(" · ");
    }

    // ---- 文件项副标题：适配版本（前 3 个）+ 加载器 + 大小 ----
    function modFileInfo(f) {
        var parts = [];
        var gv = f.gameVersions || [];
        var gvShow = gv.slice(0, 3).join(" / ");
        if (gv.length > 3)
            gvShow += " 等";
        if (gvShow !== "")
            parts.push(gvShow);
        var loaders = (f.loaders || []).join(" / ");
        if (loaders !== "")
            parts.push(loaders);
        parts.push(formatSize(f.fileSize));
        return parts.join(" · ");
    }

    // ---- 发起当前分类搜索（reset=true 重搜第 0 页，false 追加下一页） ----
    function startModSearch(reset) {
        if (currentCategory < 0)
            return;
        if (modSearching) {
            // bridge 互斥会吞掉飞行中的并发请求：记录最新意图，完成后补发
            modSearchQueued = true;
            modQueuedReset = reset;
            return;
        }
        var query = modSearchBox.text.trim();
        var targetPage = reset ? 0 : modPage + 1;
        modLastReq = { "platform": modPlatform, "category": currentCategory,
                       "query": query, "page": targetPage };
        modSearched = true;
        // qmllint disable unqualified
        ModPlatformBridge.search(modPlatform, currentCategory, query, targetPage);
        // qmllint enable unqualified
    }

    // ---- 打开文件选择面板并拉取文件列表 ----
    function openModFiles(modData) {
        filesModId = String(modData.id);
        filesModName = String(modData.name);
        filesPlatform = modPlatform;
        fileList = [];
        filesLoading = true;
        filesVisible = true;
        // qmllint disable unqualified
        ModPlatformBridge.getModFiles(modPlatform, filesModId);
        // qmllint enable unqualified
    }

    // ---- 关闭文件选择面板（进行中的下载不中断） ----
    function closeFiles() {
        filesVisible = false;
        filesLoading = false;
        fileList = [];
    }

    // ---- 安装指定文件：整合包走下载+导入管线（创建新实例），其余下载到目标实例对应子目录 ----
    function installModFile(fileData) {
        if (modDownloading)
            return;
        if (currentCategory === 1) {
            // 先置标志再调用：bridge 校验失败会同步 emit modpackImportFinished
            modpackImportPending = true;
            pendingModpackName = filesModName;
            // qmllint disable unqualified
            ModPlatformBridge.downloadModpackAndImport(filesPlatform, filesModId,
                                                       String(fileData.id), String(fileData.fileName));
            // qmllint enable unqualified
            return;
        }
        var target = cmbInstance.currentText;
        if (target === "")
            return;
        downloadTargetInstance = target;
        // 先置标志再调用：实例不存在时 bridge 会同步 emit downloadFinished
        modDownloadPending = true;
        // qmllint disable unqualified
        if (currentCategory === 2 || currentCategory === 3) {
            // 材质包装到 resourcepacks/，光影包装到 shaderpacks/
            ModPlatformBridge.downloadModToInstance(filesPlatform, filesModId,
                                                    String(fileData.id), String(fileData.fileName),
                                                    target,
                                                    currentCategory === 2 ? "resourcepacks" : "shaderpacks");
        } else {
            ModPlatformBridge.downloadModToInstance(filesPlatform, filesModId,
                                                    String(fileData.id), String(fileData.fileName),
                                                    target);
        }
        // qmllint enable unqualified
    }

    Connections {
        // qmllint disable unqualified
        target: VersionManager
        // qmllint enable unqualified
        function onVersionListChanged() { page.syncFromManager(); }
    }

    Connections {
        // qmllint disable unqualified
        target: InstallBridge
        // qmllint enable unqualified
        function onMcInstallFinished(ok, msg) {
            if (ok) {
                page.markInstalled(page.pendingInstallId);
                // qmllint disable missing-property
                Window.window.showHint("Minecraft " + page.pendingInstallId + " 安装完成", "success");
                // qmllint enable missing-property
            } else {
                // qmllint disable missing-property
                Window.window.showMsg({ title: "安装失败", text: msg, warn: true });
                // qmllint enable missing-property
            }
        }
    }

    // ---- ModPlatformBridge 全局单例信号：校验当前分类与请求匹配，旧响应直接丢弃 ----
    Connections {
        // qmllint disable unqualified
        target: ModPlatformBridge
        // qmllint enable unqualified
        function onSearchFinished(ok, mods) {
            var req = page.modLastReq;
            var matched = req !== null && req.platform === page.modPlatform
                          && req.category === page.currentCategory
                          && req.query === modSearchBox.text.trim();
            if (page.currentCategory >= 0 && matched) {
                if (ok) {
                    var newResults = req.page === 0 ? mods : page.modResults.concat(mods);
                    page.modResults = newResults;
                    page.modPage = req.page;
                    // 每页 25 条，不足一页即没有更多
                    page.modHasMore = mods.length >= 25;
                    // 写入当前分类缓存，切走再切回可直接恢复
                    var cache = page.resCache;
                    cache[req.category] = { "results": newResults, "page": req.page,
                                            "hasMore": page.modHasMore,
                                            "platform": req.platform, "query": req.query };
                    page.resCache = cache;
                } else {
                    if (req.page === 0)
                        page.modResults = [];
                    page.modHasMore = false;
                    // qmllint disable missing-property
                    Window.window.showHint(page.selectedNavName + " 搜索失败，请稍后重试", "error");
                    // qmllint enable missing-property
                }
            }
            // 飞行中排队的新搜索此时补发
            if (page.modSearchQueued) {
                page.modSearchQueued = false;
                page.startModSearch(page.modQueuedReset);
            }
        }

        function onModFilesFinished(ok, modId, files) {
            // 面板已关闭或已切到另一个资源 → 丢弃
            if (!page.filesVisible || String(modId) !== page.filesModId)
                return;
            page.filesLoading = false;
            page.fileList = ok ? files : [];
            if (!ok) {
                // qmllint disable missing-property
                Window.window.showHint("获取文件列表失败", "error");
                // qmllint enable missing-property
            }
        }

        function onDownloadFinished(ok, msg) {
            // 仅处理本页发起的下载
            if (!page.modDownloadPending)
                return;
            page.modDownloadPending = false;
            if (ok) {
                // qmllint disable missing-property
                Window.window.showHint("已安装到 " + page.downloadTargetInstance, "success");
                // qmllint enable missing-property
            } else {
                // qmllint disable missing-property
                Window.window.showMsg({ title: "资源安装失败", text: msg, warn: true });
                // qmllint enable missing-property
            }
        }

        function onModpackImportFinished(ok, msg, data) {
            // 仅处理本页发起的整合包导入；data 为附带的实例列表，本页暂不使用
            if (!page.modpackImportPending)
                return;
            page.modpackImportPending = false;
            if (ok) {
                // qmllint disable missing-property
                Window.window.showHint("整合包 " + page.pendingModpackName + " 导入完成", "success");
                // qmllint enable missing-property
            } else {
                // qmllint disable missing-property
                Window.window.showMsg({ title: "整合包导入失败", text: msg, warn: true });
                // qmllint enable missing-property
            }
        }
    }

    // 搜索输入停顿 400ms 自动触发
    Timer {
        id: modSearchTimer
        interval: 400
        onTriggered: page.startModSearch(true)
    }

    // 进入页面时远程清单为空且不在加载 → 自动拉取
    onIsActiveChanged: {
        if (isActive && remoteVersions.length === 0 && !versionsLoading)
            refreshRemote();
        // 页面激活且停在资源分类、尚未搜索过时补一次自动搜索
        if (isActive && currentCategory >= 0 && !modSearched)
            startModSearch(true);
    }

    Component.onCompleted: syncFromManager()

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- 左侧分类导航 ----
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

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Text {
                    text: "下载"
                    color: Theme.color1
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLogo; font.bold: true
                    Layout.leftMargin: 15; Layout.topMargin: 18; Layout.bottomMargin: 10
                }

                Repeater {
                    id: navRepeater
                    model: page.navModel

                    MLCListItem {
                        id: navItem
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        title: navItem.modelData.name
                        lucideIcon: navItem.modelData.icon
                        checkType: 2
                        checked: navItem.index === 0
                        onClicked: page.selectNav(navItem.index, navItem)
                        Component.onCompleted: if (navItem.index === 0) page.currentNavItem = navItem
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        // ---- 右侧内容 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"

            // ---- 游戏安装 ----
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.contentMargin
                spacing: Theme.sectionSpacing
                visible: page.selectedNav === 0

                // 搜索 + 刷新
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.itemSpacing

                    MLCSearchBox {
                        id: searchBox
                        Layout.fillWidth: true
                        hintText: "搜索版本号..."
                        onTextChanged: page.rebuildFiltered()
                        onAccepted: page.rebuildFiltered()
                    }
                    MLCButton {
                        text: "刷新"
                        Layout.preferredHeight: 40
                        enabled: !page.versionsLoading && !page.installBusy
                        onClicked: page.refreshRemote()
                    }
                }

                // 类型开关 + 版本计数
                RowLayout {
                    Layout.fillWidth: true

                    MLCCheckBox {
                        id: chkSnapshots
                        text: "显示快照与远古版本"
                        onChanged: page.rebuildFiltered()
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: "共 " + page.filteredVersions.length + " 个版本"
                        color: Theme.gray3
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                    }
                }

                // 安装进度行（busy 时显示）
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    visible: page.installBusy
                    radius: Theme.buttonRadius
                    color: Theme.pureWhite
                    border { width: 1; color: Theme.gray5 }

                    ColumnLayout {
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                        anchors.margins: 12
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: page.installProgressText
                                color: Theme.color1
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: page.installProgressPercent + "%"
                                color: Theme.gray3
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                        MLCProgressBar {
                            Layout.fillWidth: true
                            value: page.installProgressPercent / 100
                        }
                    }
                }

                // 版本列表
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Flickable {
                        anchors.fill: parent
                        visible: !page.versionsLoading && page.filteredVersions.length > 0
                        contentWidth: width
                        contentHeight: verCol.implicitHeight + 10
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: MLCScrollBar {}

                        ColumnLayout {
                            id: verCol
                            anchors { left: parent.left; right: parent.right; top: parent.top }
                            spacing: 4

                            Repeater {
                                model: page.filteredVersions

                                MLCListItem {
                                    id: versionItem
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 46
                                    title: versionItem.modelData.vid
                                    info: versionItem.modelData.installed ? "已安装" : versionItem.modelData.info
                                    lucideIcon: "arrow-down-to-line"
                                    checkType: versionItem.modelData.installed ? 0 : 1
                                    enabled: !page.installBusy
                                    onClicked: page.confirmInstall(versionItem.modelData.vid)
                                }
                            }
                        }
                    }

                    // 加载中占位
                    Text {
                        anchors.centerIn: parent
                        visible: page.versionsLoading
                        text: "正在获取版本列表..."
                        color: Theme.gray3
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                    }

                    // 空结果占位
                    Text {
                        anchors.centerIn: parent
                        visible: !page.versionsLoading && page.filteredVersions.length === 0
                        text: page.remoteVersions.length === 0
                              ? "尚未获取到版本列表，点击“刷新”重试" : "没有匹配的版本"
                        color: Theme.gray3
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                    }
                }
            }

            // ---- 资源浏览（Mod/整合包/材质包/光影包共用） ----
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.contentMargin
                spacing: Theme.itemSpacing
                visible: page.currentCategory >= 0

                // 搜索行：关键词 + 来源平台 + 目标实例（整合包创建新实例，不显示实例下拉）
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.itemSpacing

                    MLCSearchBox {
                        id: modSearchBox
                        Layout.fillWidth: true
                        hintText: "搜索 " + page.selectedNavName + "..."
                        onTextChanged: modSearchTimer.restart()
                        onAccepted: {
                            modSearchTimer.stop();
                            page.startModSearch(true);
                        }
                    }
                    MLCComboBox {
                        id: cmbPlatform
                        Layout.preferredWidth: 130
                        Layout.preferredHeight: 40
                        model: ["CurseForge", "Modrinth"]
                        currentIndex: page.modPlatform
                        onActivated: function (index) {
                            if (page.modPlatform !== index) {
                                page.modPlatform = index;
                                page.closeFiles();
                                page.startModSearch(true);
                            }
                        }
                    }
                    Text {
                        visible: page.currentCategory !== 1
                        text: "安装到"
                        color: Theme.gray3
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                    }
                    MLCComboBox {
                        id: cmbInstance
                        visible: page.currentCategory !== 1
                        Layout.preferredWidth: 180
                        Layout.preferredHeight: 40
                        model: page.instanceList
                    }
                }

                // 结果列表
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Flickable {
                        anchors.fill: parent
                        visible: page.modResults.length > 0
                        contentWidth: width
                        contentHeight: modCol.implicitHeight + 10
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: MLCScrollBar {}

                        ColumnLayout {
                            id: modCol
                            anchors { left: parent.left; right: parent.right; top: parent.top }
                            spacing: 4

                            Repeater {
                                model: page.modResults

                                MLCListItem {
                                    id: modItem
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 52
                                    title: String(modItem.modelData.name)
                                    info: page.modInfoText(modItem.modelData)
                                    logoScale: 1.2
                                    imageSource: String(modItem.modelData.iconUrl) !== ""
                                                 ? String(modItem.modelData.iconUrl) : ""
                                    lucideIcon: String(modItem.modelData.iconUrl) === "" ? page.selectedNavIcon : ""
                                    checkType: 1
                                    onClicked: page.openModFiles(modItem.modelData)
                                }
                            }

                            // 加载更多：page+1 追加结果，搜索中禁用
                            MLCButton {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.topMargin: 6
                                visible: page.modHasMore
                                enabled: !page.modSearching
                                text: page.modSearching ? "正在加载..." : "加载更多"
                                onClicked: page.startModSearch(false)
                            }
                        }
                    }

                    // 搜索中占位
                    Text {
                        anchors.centerIn: parent
                        visible: page.modSearching && page.modResults.length === 0
                        text: "正在搜索..."
                        color: Theme.gray3
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                    }

                    // 空结果占位
                    Text {
                        anchors.centerIn: parent
                        visible: !page.modSearching && page.modResults.length === 0
                        text: page.modSearched ? "没有匹配的 " + page.selectedNavName : "输入关键词开始搜索"
                        color: Theme.gray3
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                    }
                }
            }

            // ---- 数据包：即将推出（装到具体存档，语义复杂，后续实现） ----
            ColumnLayout {
                anchors.centerIn: parent
                visible: page.selectedNav === 5
                spacing: 10

                MLCIcon {
                    Layout.alignment: Qt.AlignHCenter
                    size: 48
                    lucideIcon: page.selectedNavIcon
                    iconColor: Theme.gray4
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: page.selectedNavName
                    color: Theme.color1
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLarge; font.bold: true
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "该分类即将推出，敬请期待"
                    color: Theme.gray3
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                }
            }

            // ---- 文件选择覆盖层（参考 MLCMsg 遮罩：半透明 + 点击空白关闭） ----
            Rectangle {
                id: filesOverlay
                anchors.fill: parent
                visible: page.filesVisible && page.currentCategory >= 0
                color: Qt.rgba(Theme.color1.r, Theme.color1.g, Theme.color1.b, 0.35)

                // 遮罩：吞掉事件，点击关闭面板（进行中的下载不中断，完成后照常提示）
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: page.closeFiles()
                }

                // 面板卡片
                Rectangle {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 40, 620)
                    height: parent.height - 50
                    radius: Theme.buttonRadius
                    color: Theme.pureWhite
                    border { width: 1; color: Theme.gray5 }

                    // 拦截卡片区域内的点击，避免穿透到遮罩触发关闭
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.itemSpacing

                        // 标题行：Mod 名 + 关闭按钮
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.itemSpacing

                            Text {
                                Layout.fillWidth: true
                                text: page.filesModName
                                color: Theme.color1
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeTitle
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            MLCIconButton {
                                lucideIcon: "x"
                                theme: 2
                                onClicked: page.closeFiles()
                            }
                        }

                        // 下载进度行（downloading 时显示）
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            visible: page.modDownloading

                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: page.modpackImportPending
                                          ? "正在下载并导入 " + page.pendingModpackName + "..."
                                          : "正在下载到 " + page.downloadTargetInstance + "..."
                                    color: Theme.color1
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: page.modDownloadPercent + "%"
                                    color: Theme.gray3
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeSmall
                                }
                            }
                            MLCProgressBar {
                                Layout.fillWidth: true
                                value: page.modDownloadPercent / 100
                            }
                        }

                        // 文件列表
                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Flickable {
                                anchors.fill: parent
                                visible: page.fileList.length > 0
                                contentWidth: width
                                contentHeight: fileCol.implicitHeight + 10
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: MLCScrollBar {}

                                ColumnLayout {
                                    id: fileCol
                                    anchors { left: parent.left; right: parent.right; top: parent.top }
                                    spacing: 4

                                    Repeater {
                                        model: page.fileList

                                        MLCListItem {
                                            id: fileItem
                                            required property var modelData
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 48
                                            title: String(fileItem.modelData.displayName)
                                            info: page.modFileInfo(fileItem.modelData)
                                            lucideIcon: "file-archive"
                                            checkType: 0
                                            buttons: [
                                                MLCButton {
                                                    text: "安装"
                                                    colorType: 1
                                                    enabled: (page.currentCategory === 1
                                                              || cmbInstance.currentText !== "")
                                                             && !page.modDownloading
                                                    onClicked: page.installModFile(fileItem.modelData)
                                                }
                                            ]
                                        }
                                    }
                                }
                            }

                            // 文件列表加载中占位
                            Text {
                                anchors.centerIn: parent
                                visible: page.filesLoading
                                text: "正在获取文件列表..."
                                color: Theme.gray3
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                            }

                            // 无文件占位
                            Text {
                                anchors.centerIn: parent
                                visible: !page.filesLoading && page.fileList.length === 0
                                text: "没有可用的文件"
                                color: Theme.gray3
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fontSize
                            }
                        }
                    }
                }
            }
        }
    }
}
