import bb.cascades 1.4
import bb.system 1.2
import QtQuick 1.0

TabbedPane {
    id: root
    objectName: "batteryTabs"
    showTabsOnActionBar: true
    property bool showSoc: true
    property bool showEvents: false
    property int recordIndex: 0
    property bool showQualityDetails: false
    property bool interfaceReady: false
    onCreationCompleted: interfaceReady = true
    function testView(args) {
        var tabs = [liveTab, trendTab, recordTab, qualityTab];
        var index = Number(args.tab || 0);
        if (index >= 0 && index < tabs.length) activeTab = tabs[index];
        if (args.hours) backend.setWindow(Number(args.hours));
        if (args.parameter) backend.setParameter(args.parameter);
        if (args.move) backend.moveWindow(Number(args.move));
        if (args.date) backend.browseDate(new Date(args.date));
        if (args.follow === true) backend.followLatest();
        if (args.showSoc !== undefined) showSoc = args.showSoc;
        if (args.events !== undefined) recordIndex = args.events ? 3 : 0;
        if (args.filter !== undefined) {
            if (args.filter === "charge") recordIndex = 1;
            else if (args.filter === "discharge") recordIndex = 2;
            else if (!showEvents) recordIndex = 0;
            backend.filterSessions(args.filter);
        }
        if (args.settings === true) settings.open();
        if (args.display === true) displaySettings.open();
        if (args.calendar === true) calendar.open();
        if (args.session === true) sessionSheet.open();
        if (args.diagnostics === true) detailDiagnostics.open();
        if (args.qualityDetails !== undefined) showQualityDetails = args.qualityDetails;
        if (args.scroll !== undefined) {
            if (index === 1) historyScroll.scrollToPoint(0, Number(args.scroll), ScrollAnimation.None);
            if (index === 3) diagnosisScroll.scrollToPoint(0, Number(args.scroll), ScrollAnimation.None);
        }
        if (args.close === true) { settings.close(); sessionSheet.close(); detailDiagnostics.close(); calendar.close(); displaySettings.close(); }
        return { tab: index, width: overviewLayout.layoutFrame.width,
            height: overviewLayout.layoutFrame.height, records: backend.sessionCount };
    }
    Menu.definition: MenuDefinition {
        settingsAction: SettingsActionItem { title: "采集与提醒"; imageSource: "asset:///icons/settings.png"; onTriggered: settings.open() }
        actions: [ ActionItem { title: "导出全部记录"; imageSource: "asset:///icons/save.png"; onTriggered: backend.exportData(false) } ]
    }
    Tab {
        id: liveTab
        title: "概览"
        imageSource: "asset:///icons/battery.png"
        Page {
            ScrollView {
                scrollViewProperties.scrollMode: ScrollMode.Vertical
                Container {
                    objectName: "overviewContent"
                    background: Color.create("#101820")
                    leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(1.5)
                    attachedObjects: [ LayoutUpdateHandler { id: overviewLayout } ]
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Container {
                            preferredWidth: ui.du(0.7); minWidth: preferredWidth; maxWidth: preferredWidth
                            preferredHeight: ui.du(0.7); minHeight: preferredHeight; maxHeight: preferredHeight
                            verticalAlignment: VerticalAlignment.Center; rightMargin: ui.du(0.7)
                            background: Color.create(backend.live.fresh && !backend.live.paused ? "#63d6a0" : "#efc56a")
                        }
                        Label {
                            objectName: "collectorStatus"
                            text: backend.live.paused ? "已暂停" : backend.live.fresh ? "采集中" : "等待采集"
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#93a4b2")
                            topMargin: 0; bottomMargin: 0
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        }
                        Label { text: backend.live.lastText || ""; topMargin: 0; bottomMargin: 0; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#748896") }
                    }
                    BatteryGauge {
                        soc: backend.live.socText || "--"
                        available: backend.live.socAvailable === true
                        mode: backend.live.mode || "unknown"
                        subtitle: backend.live.socAvailable !== true ? "等待数据" : !backend.live.fresh ? "历史读数" : backend.live.paused ? "已暂停" : backend.live.modeText || "状态未知"
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "电池平均电流"; value: backend.live.currentText || "--"; unit: "mA"; accent: "#78c9ee" }
                        Metric { label: "温度"; value: backend.live.temperatureText || "--"; unit: "°C" }
                    }
                    Label {
                        text: backend.live.alertText || ""
                        visible: text !== ""
                        textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#f09b87")
                        multiline: true; bottomMargin: 0
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "放电估算"; value: backend.capacity.dischargeText || "--"; unit: "mAh"; note: backend.capacity.dischargeNote || "待完整区间"; accent: "#78c9ee" }
                        Metric { label: "充电估算"; value: backend.capacity.chargeText || "--"; unit: "mAh"; note: backend.capacity.chargeNote || "待完整区间"; accent: "#63d6a0" }
                    }
                    Chart { title: "近 1 小时"; plot: backend.overviewImage; axes: backend.overviewAxes; compact: true }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Label {
                            text: backend.live.sessionActive ? backend.live.sessionText : "暂无过程"
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#93a4b2")
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        }
                        Label { text: backend.live.sessionActive ? backend.live.sessionDuration : ""; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2") }
                    }
                }
            }
            shortcuts: [ Shortcut { key: "r"; onTriggered: backend.refresh() } ]
        }
    }
    Tab {
        id: trendTab
        title: "历史"
        imageSource: "asset:///icons/trend.png"
        Page {
            ScrollView {
                scrollViewProperties.scrollMode: ScrollMode.Vertical
                id: historyScroll
                Container {
                    background: Color.create("#101820")
                    leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(0.5); bottomPadding: ui.du(2)
                    DropDown {
                        title: "时间范围"
                        options: [
                            Option { text: "1 小时"; value: 1; selected: backend.windowHours === 1 },
                            Option { text: "6 小时"; value: 6; selected: backend.windowHours === 6 },
                            Option { text: "24 小时"; value: 24; selected: backend.windowHours === 24 },
                            Option { text: "7 天"; value: 168; selected: backend.windowHours === 168 },
                            Option { text: "30 天"; value: 720; selected: backend.windowHours === 720 }
                        ]
                        onSelectedValueChanged: if (selectedValue) backend.setWindow(Number(selectedValue))
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        ImageButton {
                            defaultImageSource: "asset:///icons/previous.png"; pressedImageSource: defaultImageSource
                            preferredWidth: ui.du(5); minWidth: preferredWidth; maxWidth: preferredWidth
                            preferredHeight: ui.du(5); minHeight: preferredHeight; maxHeight: preferredHeight
                            accessibility.name: "上一段"
                            onClicked: backend.moveWindow(-1)
                        }
                        Label {
                            text: backend.following ? "实时" : "历史"
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create(backend.following ? "#63d6a0" : "#b1b6bc")
                            verticalAlignment: VerticalAlignment.Center
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        }
                        ImageButton {
                            defaultImageSource: "asset:///icons/history.png"; pressedImageSource: defaultImageSource
                            preferredWidth: ui.du(5); minWidth: preferredWidth; maxWidth: preferredWidth
                            preferredHeight: ui.du(5); minHeight: preferredHeight; maxHeight: preferredHeight
                            accessibility.name: "选择日期"
                            onClicked: calendar.open()
                        }
                        ImageButton {
                            defaultImageSource: "asset:///icons/refresh.png"; pressedImageSource: defaultImageSource
                            preferredWidth: ui.du(5); minWidth: preferredWidth; maxWidth: preferredWidth
                            preferredHeight: ui.du(5); minHeight: preferredHeight; maxHeight: preferredHeight
                            accessibility.name: "回到实时"
                            onClicked: backend.followLatest()
                        }
                        ImageButton {
                            defaultImageSource: "asset:///icons/settings.png"; pressedImageSource: defaultImageSource
                            preferredWidth: ui.du(5); minWidth: preferredWidth; maxWidth: preferredWidth
                            preferredHeight: ui.du(5); minHeight: preferredHeight; maxHeight: preferredHeight
                            accessibility.name: "曲线"
                            onClicked: displaySettings.open()
                        }
                        ImageButton {
                            defaultImageSource: "asset:///icons/next.png"; pressedImageSource: defaultImageSource
                            disabledImageSource: defaultImageSource; enabled: !backend.following
                            opacity: enabled ? 1 : 0.3
                            preferredWidth: ui.du(5); minWidth: preferredWidth; maxWidth: preferredWidth
                            preferredHeight: ui.du(5); minHeight: preferredHeight; maxHeight: preferredHeight
                            accessibility.name: "下一段"
                            onClicked: backend.moveWindow(1)
                        }
                    }
                    Label {
                        text: backend.history.rangeText || ""
                        textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2")
                        topMargin: ui.du(0.5); bottomMargin: 0
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Label { text: "充电"; textStyle.color: Color.create("#63d6a0"); textStyle.base: SystemDefaults.TextStyles.SmallText; rightMargin: ui.du(2) }
                        Label { text: "放电"; textStyle.color: Color.create("#78c9ee"); textStyle.base: SystemDefaults.TextStyles.SmallText; rightMargin: ui.du(2) }
                        Label { text: "接电静置"; textStyle.color: Color.create("#efc56a"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    }
                    Label {
                        text: backend.inspection
                        preferredHeight: ui.du(6); minHeight: preferredHeight; maxHeight: preferredHeight
                        multiline: true; autoSize.maxLineCount: 2
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#f1f3f4")
                        bottomMargin: 0
                    }
                    Chart { title: "电量 · %"; plot: backend.socImage; axes: backend.socAxes; cursor: backend.inspectionCursor; inspectable: true; visible: root.showSoc }
                    Chart {
                        visible: backend.parameter !== "none"
                        title: backend.parameter === "current" ? "电池平均电流 · mA" : backend.parameter === "voltage" ? "电压 · V" : "温度 · °C"
                        plot: backend.parameterImage; axes: backend.parameterAxes; cursor: backend.inspectionCursor; inspectable: true
                    }
                    Divider {}
                    Label { text: "区间汇总"; textStyle.fontSize: FontSize.Medium; bottomMargin: 0 }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "累计充入"; value: backend.history.chargeText || "未提供"; accent: "#63d6a0" }
                        Metric { label: "累计放出"; value: backend.history.dischargeText || "未提供"; accent: "#78c9ee" }
                    }
                    Label { text: backend.history.summaryText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2") }
                }
            }
        }
    }
    Tab {
        id: recordTab
        title: "记录"
        imageSource: "asset:///icons/history.png"
        Page {
            Container {
                background: Color.create("#101820")
                leftPadding: ui.du(1); rightPadding: ui.du(1); topPadding: ui.du(1); bottomPadding: ui.du(1)
                SegmentedControl {
                    selectedIndex: root.recordIndex
                    options: [ Option { text: "全部" }, Option { text: "充电" }, Option { text: "放电" }, Option { text: "事件" } ]
                    onSelectedIndexChanged: if (selectedIndex >= 0) {
                        root.recordIndex = selectedIndex;
                        root.showEvents = selectedIndex === 3;
                        if (!root.showEvents) backend.filterSessions(selectedIndex === 1 ? "charge" : selectedIndex === 2 ? "discharge" : "");
                    }
                }
                Label {
                    visible: root.showEvents ? backend.eventCount === 0 : backend.sessionCount === 0
                    text: root.showEvents ? "暂无事件与提醒" : "暂无充放电记录"
                    textStyle.color: Color.create("#93a4b2")
                    horizontalAlignment: HorizontalAlignment.Center; topMargin: ui.du(4)
                }
                ListView {
                    objectName: "sessionList"
                    visible: !root.showEvents
                    dataModel: backend.sessions
                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    listItemComponents: [ ListItemComponent {
                        type: ""
                        StandardListItem { title: ListItemData.title; description: ListItemData.description; status: ListItemData.status; imageSource: ListItemData.image }
                    } ]
                    onTriggered: { backend.selectSession(dataModel.data(indexPath).id); sessionSheet.open(); }
                }
                ListView {
                    visible: root.showEvents
                    dataModel: backend.events
                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    listItemComponents: [ ListItemComponent {
                        type: ""
                        StandardListItem { title: ListItemData.title; description: ListItemData.description; imageSource: ListItemData.image }
                    } ]
                }
            }
        }
    }
    Tab {
        id: qualityTab
        title: "诊断"
        imageSource: "asset:///icons/info.png"
        Page {
            ScrollView {
                scrollViewProperties.scrollMode: ScrollMode.Vertical
                id: diagnosisScroll
                Container {
                    background: Color.create("#101820")
                    leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(2)
                    Label {
                        text: backend.live.paused ? "采集已暂停" : !backend.live.fresh ? "采集离线" : "数据未就绪"
                        visible: backend.live.paused || !backend.live.fresh || !backend.live.ready
                        textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#efc56a")
                    }
                    Label { text: "容量预估 · mAh"; textStyle.fontSize: FontSize.Medium; bottomMargin: ui.du(0.5) }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "放电积分"; value: backend.capacity.dischargeText || "--"; note: backend.capacity.dischargeNote || "待完整区间"; accent: "#78c9ee" }
                        Metric { label: "充电积分"; value: backend.capacity.chargeText || "--"; note: backend.capacity.chargeNote || "待完整区间"; accent: "#63d6a0" }
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "余量 / SOC"; value: backend.capacity.remainingText || "--"; note: backend.capacity.remainingNote || "缺少有效读数"; accent: "#b5a4ec" }
                        Metric { label: "健康度折算"; value: backend.capacity.healthText || "--"; note: backend.capacity.healthNote || "缺少有效读数"; accent: "#b5a4ec" }
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "系统满充"; value: backend.capacity.reportedText || "--"; note: "系统读数" }
                        Metric { label: "设计容量"; value: backend.capacity.designText || "--"; note: "设计参考" }
                    }
                    Label { text: "实时参数"; textStyle.fontSize: FontSize.Medium; topMargin: ui.du(1); bottomMargin: ui.du(0.5) }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "电压"; value: backend.live.voltageText || "--"; unit: "V" }
                        Metric { label: "剩余电量"; value: backend.live.remainingText || "--" }
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "系统健康度"; value: backend.live.healthText || "--" }
                        Metric { label: "循环次数"; value: backend.live.cyclesText || "--" }
                    }
                    Label { text: "电源"; textStyle.fontSize: FontSize.Medium; topMargin: ui.du(1); bottomMargin: ui.du(0.5) }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "输入电流"; value: backend.live.inputText || "--" }
                        Metric { label: "系统充电电流"; value: backend.live.systemChargeText || "--" }
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "输入限值"; value: backend.live.inputLimitText || "--" }
                        Metric { label: "充电限值"; value: backend.live.chargeLimitText || "--" }
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "预计耗尽"; value: backend.live.timeToEmptyText || "--" }
                        Metric { label: "预计充满"; value: backend.live.timeToFullText || "--" }
                    }
                    Button {
                        text: root.showQualityDetails ? "收起采集详情" : "采集详情"
                        horizontalAlignment: HorizontalAlignment.Fill
                        onClicked: root.showQualityDetails = !root.showQualityDetails
                    }
                    Container {
                        visible: root.showQualityDetails
                        Label { text: backend.quality.historyText || ""; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Label { text: "有效读数  电流 " + (backend.quality.currentCoverage || 0) + "% · 电压 " + (backend.quality.voltageCoverage || 0) + "% · 温度 " + (backend.quality.temperatureCoverage || 0) + "%"; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2") }
                        Label { text: backend.quality.missingText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2") }
                        Label { text: backend.status; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Label { text: "最近采样  " + (backend.live.sampleAge || "--") + "前 · " + (backend.quality.storageText || "--"); textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2") }
                        Label { text: backend.quality.period || ""; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#748896") }
                    }
                }
            }
        }
    }
    attachedObjects: [
        SystemToast { id: toast },
        Sheet {
            id: settings
            Page {
                ScrollView {
                scrollViewProperties.scrollMode: ScrollMode.Vertical
                    Container {
                        background: Color.create("#101820"); leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(2)
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Label { text: "持续采集"; verticalAlignment: VerticalAlignment.Center; layoutProperties: StackLayoutProperties { spaceQuota: 1 } }
                            ToggleButton { checked: !backend.paused; onCheckedChanged: if (root.interfaceReady && checked === backend.paused) backend.configure(backend.interval, !checked) }
                        }
                        DropDown {
                            title: "采样间隔"
                            options: [
                                Option { text: "5 秒"; value: 5; selected: backend.interval === 5 },
                                Option { text: "10 秒"; value: 10; selected: backend.interval === 10 },
                                Option { text: "30 秒"; value: 30; selected: backend.interval === 30 },
                                Option { text: "60 秒"; value: 60; selected: backend.interval === 60 }
                            ]
                            onSelectedValueChanged: if (root.interfaceReady && selectedValue && Number(selectedValue) !== backend.interval) backend.configure(Number(selectedValue), backend.paused)
                        }
                        Divider {}
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Label { text: "电池提醒"; verticalAlignment: VerticalAlignment.Center; layoutProperties: StackLayoutProperties { spaceQuota: 1 } }
                            ToggleButton {
                                checked: backend.alerts.enabled === true
                                onCheckedChanged: if (root.interfaceReady && checked !== backend.alerts.enabled) backend.configureAlerts(checked, Number(backend.alerts.low), Number(backend.alerts.high), Number(backend.alerts.temperature))
                            }
                        }
                        Label { text: "低电量  " + Math.round(lowSlider.immediateValue) + "%"; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Slider {
                            id: lowSlider
                            fromValue: 5; toValue: 40; value: Number(backend.alerts.low || 20); enabled: backend.alerts.enabled === true
                            onValueChanged: if (root.interfaceReady && Math.round(value) !== Number(backend.alerts.low)) backend.configureAlerts(backend.alerts.enabled, Math.round(value), Number(backend.alerts.high), Number(backend.alerts.temperature))
                        }
                        Label { text: "充电电量  " + Math.round(highSlider.immediateValue) + "%"; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Slider {
                            id: highSlider
                            fromValue: 60; toValue: 100; value: Number(backend.alerts.high || 90); enabled: backend.alerts.enabled === true
                            onValueChanged: if (root.interfaceReady && Math.round(value) !== Number(backend.alerts.high)) backend.configureAlerts(backend.alerts.enabled, Number(backend.alerts.low), Math.round(value), Number(backend.alerts.temperature))
                        }
                        Label { text: "温度  " + Math.round(tempSlider.immediateValue) + "°C"; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Slider {
                            id: tempSlider
                            fromValue: 35; toValue: 55; value: Number(backend.alerts.temperature || 45); enabled: backend.alerts.enabled === true
                            onValueChanged: if (root.interfaceReady && Math.round(value) !== Number(backend.alerts.temperature)) backend.configureAlerts(backend.alerts.enabled, Number(backend.alerts.low), Number(backend.alerts.high), Math.round(value))
                        }
                        Label { text: backend.status; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2") }
                    }
                }
                actions: [ ActionItem { title: "完成"; imageSource: "asset:///icons/close.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: settings.close() } ]
            }
        },
        Sheet {
            id: displaySettings
            Page {
                Container {
                    background: Color.create("#101820"); leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1.5)
                    CheckBox { text: "电量曲线"; checked: root.showSoc; onCheckedChanged: root.showSoc = checked }
                    DropDown {
                        title: "参数曲线"
                        options: [
                            Option { text: "电池平均电流"; value: "current"; selected: backend.parameter === "current" },
                            Option { text: "电压"; value: "voltage"; selected: backend.parameter === "voltage" },
                            Option { text: "温度"; value: "temperature"; selected: backend.parameter === "temperature" },
                            Option { text: "隐藏"; value: "none"; selected: backend.parameter === "none" }
                        ]
                        onSelectedValueChanged: if (selectedValue) backend.setParameter(selectedValue)
                    }
                }
                actions: [ ActionItem { title: "完成"; imageSource: "asset:///icons/close.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: displaySettings.close() } ]
            }
        },
        Sheet {
            id: calendar
            Page {
                Container {
                    background: Color.create("#101820"); leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1.5)
                    DateTimePicker { id: historyDate; title: "截止日期"; mode: DateTimePickerMode.Date; value: backend.historyDate; maximum: new Date(); expanded: true }
                }
                actions: [
                    ActionItem { title: "查看"; imageSource: "asset:///icons/history.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: { backend.browseDate(historyDate.value); calendar.close(); } },
                    ActionItem { title: "取消"; imageSource: "asset:///icons/close.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: calendar.close() }
                ]
            }
        },
        Sheet {
            id: sessionSheet
            Page {
                ScrollView {
                scrollViewProperties.scrollMode: ScrollMode.Vertical
                    Container {
                        background: Color.create("#101820"); leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(2)
                        Label { text: backend.detail.timeText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#93a4b2") }
                        Label { text: backend.detail.socText || ""; textStyle.fontSize: FontSize.Large }
                        Label { text: backend.detail.durationText || ""; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Metric { label: "累计电量"; value: backend.detail.chargeText || "--" }
                            Metric { label: backend.detail.estimateKind || "容量外推"; value: backend.detail.estimateValue || "--"; unit: "mAh"; accent: "#78c9ee" }
                        }
                        Chart { title: "电量 · %"; plot: backend.detailSocImage; axes: backend.detailSocAxes }
                        Chart { title: "电池平均电流 · mA"; plot: backend.detailCurrentImage; axes: backend.detailCurrentAxes }
                    }
                }
                actions: [
                    ActionItem { title: "关闭"; imageSource: "asset:///icons/close.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: sessionSheet.close() },
                    ActionItem { title: "诊断"; imageSource: "asset:///icons/info.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: detailDiagnostics.open() },
                    ActionItem { title: "导出"; imageSource: "asset:///icons/save.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: backend.exportData(true) }
                ]
            }
        },
        Sheet {
            id: detailDiagnostics
            Page {
                Container {
                    background: Color.create("#101820"); leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1.5)
                    Metric { label: backend.detail.estimateKind || "容量外推"; value: backend.detail.estimateValue || "--"; unit: "mAh"; accent: "#78c9ee" }
                    Label { text: backend.detail.evaluation || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#efc56a") }
                    Divider {}
                    Label { text: "累计能量  " + (backend.detail.energyText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "电流时间均值  " + (backend.detail.currentMeanText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "最高温度  " + (backend.detail.temperatureMaxText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "采样数  " + (backend.detail.samplesText || "0"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "积分覆盖  " + (backend.detail.coverageText || "--"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                }
                actions: [ ActionItem { title: "关闭"; imageSource: "asset:///icons/close.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: detailDiagnostics.close() } ]
            }
        },
        Connections { target: backend; onExported: { toast.body = message; toast.show(); } onAlerted: { toast.body = message; toast.show(); } }
    ]
}
