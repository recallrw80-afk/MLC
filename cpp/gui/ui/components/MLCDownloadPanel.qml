pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Qt5Compat.GraphicalEffects
import MLC

// 下载任务面板（对应原版 PageSpeedLeft/PageSpeedRight 的精简版）
// 左下角悬浮按钮：有进行中任务时显示数量徽标与总进度（大小全部未知时改为脉冲动画）；
// 点击按钮在其上方展开任务列表。任务数据由 Main.qml 经 DownloadManager
// 全局信号转发，调用 addTask / updateTask / finishTask 喂入（任务以 url 为 key）
Item {
    id: panel

    // ---- 对外 API ----

    // 新任务：进行中任务始终排在已完成/失败任务之前；同 url 重复开始视为重新下载
    function addTask(url) {
        var idx = _indexOf(url);
        if (idx >= 0) {
            taskModel.set(idx, {
                "received": 0,
                "total": 0,
                "status": 0
            });
            _moveToActive(idx);
        } else {
            var entry = {
                "url": String(url),
                "name": _fileName(url),
                "received": 0,
                "total": 0,
                "status": 0
            };
            var pos = _activeEnd(-1);
            if (pos >= taskModel.count)
                taskModel.append(entry);
            else
                taskModel.insert(pos, entry);
        }
        _refreshTotals();
    }

    // 进度更新；total 可能为 0（大小未知），已完成/失败任务忽略迟到的进度包
    function updateTask(url, received, total) {
        var idx = _indexOf(url);
        if (idx < 0 || taskModel.get(idx).status !== 0)
            return;
        taskModel.set(idx, {
            "received": received,
            "total": total
        });
        _refreshTotals();
    }

    // 完成/失败：成功时把进度补齐到 100%，然后沉到进行中区段之后（灰显，5 秒后自动移除）
    function finishTask(url, success, msg) {
        var idx = _indexOf(url);
        if (idx < 0)
            return;
        var patch = {
            "status": success ? 1 : 2
        };
        var row = taskModel.get(idx);
        if (success && row.total > 0)
            patch["received"] = row.total;
        taskModel.set(idx, patch);
        _moveToActive(idx);
        _refreshTotals();
    }

    // 移除任务（由行内 Timer 在完成/失败 5 秒后调用）；全部移除后复位为收起态
    function removeTask(url) {
        var idx = _indexOf(url);
        if (idx < 0)
            return;
        taskModel.remove(idx);
        if (taskModel.count === 0)
            expanded = false;
        _refreshTotals();
    }

    // ---- 内部状态 ----

    property bool expanded: false
    property int activeCount: 0      // 进行中任务数（徽标数据源）
    property real totalReceived: 0   // 进行中任务已下载字节合计（仅统计已知大小的任务）
    property real totalSize: 0       // 进行中任务总字节合计（0 = 进行中的任务全部大小未知）

    readonly property int _btnSize: 40
    readonly property int _headerHeight: 34
    // 列表最大高度：窗口高的 40% 减去标题行
    readonly property real _maxListHeight: Math.max(80, height * 0.4 - _headerHeight)
    // 展开后的面板内容总高（标题行 + 列表可视高）
    readonly property real _contentHeight: _headerHeight + Math.min(taskList.contentHeight, _maxListHeight)

    // 无任务时整体隐藏（含悬浮按钮）
    visible: taskModel.count > 0

    // ---- 任务数据（status: 0=进行中 1=已完成 2=失败）----

    ListModel {
        id: taskModel
    }

    function _indexOf(url) {
        var key = String(url);
        for (var i = 0; i < taskModel.count; i++) {
            if (taskModel.get(i).url === key)
                return i;
        }
        return -1;
    }

    // 从 url 提取显示文件名：去查询串、取最后一个 '/' 之后部分并 URL 解码
    function _fileName(url) {
        var s = String(url);
        var q = s.indexOf("?");
        if (q >= 0)
            s = s.substring(0, q);
        var name = s.substring(s.lastIndexOf("/") + 1);
        try {
            name = decodeURIComponent(name);
        } catch (e) {
            // 非法转义序列时保留原样
        }
        return name === "" ? s : name;
    }

    function _formatSize(bytes) {
        if (bytes >= 1048576)
            return (bytes / 1048576).toFixed(1) + " MB";
        if (bytes >= 1024)
            return (bytes / 1024).toFixed(1) + " KB";
        return Math.floor(bytes) + " B";
    }

    // 进行中区段的末尾索引（第一个已完成/失败任务的位置），except 为不计入的行
    function _activeEnd(except) {
        var n = 0;
        for (var i = 0; i < taskModel.count; i++) {
            if (i !== except && taskModel.get(i).status === 0)
                n++;
        }
        return n;
    }

    // 把 idx 行移到进行中区段末尾，维持“进行中在前”的分区不变式
    function _moveToActive(idx) {
        var target = _activeEnd(idx);
        if (idx !== target)
            taskModel.move(idx, target, 1);
    }

    // 重算进行中任务数与总进度（按钮徽标 / 总进度条数据源）
    function _refreshTotals() {
        var active = 0, recv = 0, size = 0;
        for (var i = 0; i < taskModel.count; i++) {
            var t = taskModel.get(i);
            if (t.status !== 0)
                continue;
            active++;
            if (t.total > 0) {
                recv += t.received;
                size += t.total;
            }
        }
        activeCount = active;
        totalReceived = recv;
        totalSize = size;
    }

    // 标题行右侧的整体状态文本
    function _overallText() {
        if (activeCount <= 0)
            return "全部完成";
        if (totalSize > 0)
            return Math.min(100, Math.round(totalReceived * 100 / totalSize)) + "%";
        return activeCount + " 项进行中";
    }

    // ---- 收起态：左下角悬浮按钮 ----

    Item {
        id: btnBox
        anchors {
            left: parent.left
            bottom: parent.bottom
            leftMargin: 15
            bottomMargin: 15
        }
        width: panel._btnSize
        height: panel._btnSize

        // 投影（同 MLCCard：color1 7%）
        DropShadow {
            anchors.fill: btnCard
            source: btnCard
            radius: 6
            samples: 13
            color: Qt.alpha(Theme.color1, 0.07)
        }

        Rectangle {
            id: btnCard
            anchors.fill: parent
            radius: 5
            color: Qt.alpha(Theme.pureWhite, 245 / 255)
        }

        MLCIconButton {
            id: btnIcon
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -3
            lucideIcon: "arrow-down-to-line"
            onClicked: panel.expanded = !panel.expanded
        }

        // 总进度细条（仅收起且已知总大小时显示）
        MLCProgressBar {
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                leftMargin: 6
                rightMargin: 6
                bottomMargin: 5
            }
            height: 3
            visible: !panel.expanded && panel.totalSize > 0
            value: panel.totalSize > 0 ? Math.min(1, panel.totalReceived / panel.totalSize) : 0
        }

        // 进行中任务数量徽标
        Rectangle {
            visible: panel.activeCount > 0
            anchors {
                right: parent.right
                top: parent.top
                rightMargin: -7
                topMargin: -7
            }
            width: Math.max(16, badgeText.implicitWidth + 8)
            height: 16
            radius: 8
            color: Theme.color2

            Text {
                id: badgeText
                anchors.centerIn: parent
                text: panel.activeCount > 99 ? "99+" : panel.activeCount
                color: Theme.pureWhite
                font.family: Theme.fontFamily
                font.pixelSize: 10
            }
        }
    }

    // 大小全部未知时的脉冲动画（呼吸提示下载仍在进行）
    SequentialAnimation {
        id: pulseAnim
        loops: Animation.Infinite
        running: panel.visible && !panel.expanded && panel.activeCount > 0 && panel.totalSize <= 0

        NumberAnimation {
            target: btnIcon
            property: "opacity"
            to: 0.35
            duration: 700
        }
        NumberAnimation {
            target: btnIcon
            property: "opacity"
            to: 1.0
            duration: 700
        }
        onStopped: btnIcon.opacity = 1.0
    }

    // ---- 展开态：按钮上方的任务列表面板 ----

    Item {
        id: popup
        anchors {
            left: btnBox.left
            bottom: btnBox.top
            bottomMargin: 8
        }
        width: 300
        height: panel.expanded ? panel._contentHeight : 0
        enabled: panel.expanded

        // 展开/收起 150ms 高度动画（同 MLCHint 的时长与缓动）
        Behavior on height {
            NumberAnimation {
                duration: 150
                easing.type: Easing.OutCubic
            }
        }

        DropShadow {
            anchors.fill: popupCard
            source: popupCard
            radius: 6
            samples: 13
            color: Qt.alpha(Theme.color1, 0.07)
        }

        Rectangle {
            id: popupCard
            anchors.fill: parent
            radius: 5
            color: Qt.alpha(Theme.pureWhite, 245 / 255)
            clip: true

            // 内容锚定卡片底部：收起时上缘向下收拢到按钮方向
            Column {
                id: popupContent
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }

                // 标题行（含底部分隔线）
                Item {
                    width: parent.width
                    height: panel._headerHeight

                    Text {
                        anchors {
                            left: parent.left
                            leftMargin: 12
                            verticalCenter: parent.verticalCenter
                        }
                        text: "下载任务"
                        color: Theme.color1
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        font.bold: true
                    }
                    Text {
                        anchors {
                            right: parent.right
                            rightMargin: 12
                            verticalCenter: parent.verticalCenter
                        }
                        text: panel._overallText()
                        color: Theme.gray3
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeXSmall
                    }
                    Rectangle {
                        anchors {
                            left: parent.left
                            right: parent.right
                            bottom: parent.bottom
                        }
                        height: 1
                        color: Theme.gray7
                    }
                }

                // 任务列表（超出最大高度后滚动）
                ListView {
                    id: taskList
                    width: parent.width
                    height: Math.min(contentHeight, panel._maxListHeight)
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentHeight > height
                    model: taskModel
                    ScrollBar.vertical: MLCScrollBar {}
                    delegate: taskDelegate
                }
            }
        }
    }

    // ---- 单行任务委托：文件名 + 进度条 + 状态 ----

    Component {
        id: taskDelegate

        Item {
            id: row

            required property string url
            required property string name
            required property real received
            required property real total
            required property int status

            width: ListView.view ? ListView.view.width : 0
            height: 46

            // 已完成/失败灰显
            opacity: status === 0 ? 1 : 0.5
            Behavior on opacity {
                NumberAnimation {
                    duration: 150
                }
            }

            function _statusText() {
                if (status === 1)
                    return "完成";
                if (status === 2)
                    return "失败";
                if (total > 0)
                    return Math.min(100, Math.round(received * 100 / total)) + "%";
                // 大小未知：显示已下载量
                return panel._formatSize(received);
            }

            Column {
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: 12
                    rightMargin: 12
                }
                spacing: 6

                Row {
                    width: parent.width

                    Text {
                        width: parent.width - statusLabel.implicitWidth - 8
                        text: row.name
                        color: row.status === 2 ? Theme.redLight : Theme.color1
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        elide: Text.ElideMiddle
                    }
                    Text {
                        id: statusLabel
                        text: row._statusText()
                        color: row.status === 2 ? Theme.redLight : Theme.gray3
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeXSmall
                    }
                }

                MLCProgressBar {
                    width: parent.width
                    indeterminate: row.status === 0 && row.total <= 0
                    value: row.status === 1 ? 1 : (row.total > 0 ? Math.min(1, row.received / row.total) : 0)
                    fillColorStart: row.status === 2 ? Theme.redLight : row.status === 1 ? Theme.gray5 : Theme.color4
                    fillColorEnd: row.status === 2 ? Theme.redDark : row.status === 1 ? Theme.gray4 : Theme.color3
                }
            }

            // 完成/失败 5 秒后自动移除
            Timer {
                interval: 5000
                running: row.status !== 0
                onTriggered: panel.removeTask(row.url)
            }
        }
    }
}
