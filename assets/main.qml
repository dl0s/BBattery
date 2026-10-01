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
            titleBar: TitleBar { title: "BBattery" }
            ScrollView {
                Container {
                    objectName: "overviewContent"
                    leftPadding: ui.du(2); rightPadding: ui.du(2); topPadding: ui.du(1); bottomPadding: ui.du(14)
                    attachedObjects: [ LayoutUpdateHandler { id: overviewLayout } ]
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Label {
                            objectName: "collectorStatus"
                            text: backend.live.collectorText || "等待采集"
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create(backend.live.fresh && !backend.live.paused ? "#63d6a0" : "#efc56a")
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        }
                        Label { text: backend.live.lastText || ""; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Label { text: (backend.live.socText || "--") + "%"; textStyle.fontSize: FontSize.XXLarge; textStyle.fontWeight: FontWeight.W500; rightMargin: ui.du(2.5) }
                        Container {
                            verticalAlignment: VerticalAlignment.Center
                            Label { text: backend.live.modeText || "状态未知"; textStyle.fontSize: FontSize.Medium; bottomMargin: 0 }
                            Label { text: backend.live.chargerText || ""; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc"); topMargin: 0 }
                        }
                    }
                    ProgressIndicator {
                        value: backend.live.socText !== "--" ? Number(backend.live.socText || 0) / 100 : 0
                        horizontalAlignment: HorizontalAlignment.Fill
                        topMargin: 0; bottomMargin: ui.du(1)
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
                    Divider { topMargin: ui.du(0.5); bottomMargin: 0 }
                    Chart { title: "电量 · 近 1 小时"; plot: backend.overviewImage; axes: backend.overviewAxes; compact: true }
                    Divider { topMargin: 0; bottomMargin: 0 }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Label {
                            text: backend.live.sessionActive ? "当前过程" : "当前无进行中的记录"
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: Color.create("#b1b6bc")
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        }
                        Label { text: backend.live.sessionActive ? backend.live.sessionDuration : ""; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    }
                    Label {
                        text: backend.live.sessionActive ? backend.live.sessionText : backend.status
                        multiline: true; autoSize.maxLineCount: 2; textStyle.base: SystemDefaults.TextStyles.SmallText
                        topMargin: 0; bottomMargin: 0
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
            titleBar: TitleBar { title: backend.following ? "电池历史 · 实时" : "电池历史" }
            ScrollView {
                id: historyScroll
                Container {
                    leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(0.5); bottomPadding: ui.du(14)
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
                        textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc")
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
                    Label { text: backend.history.summaryText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                }
            }
            actions: [
                ActionItem { title: "上一段"; imageSource: "asset:///icons/previous.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: backend.moveWindow(-1) },
                ActionItem { title: "实时"; imageSource: "asset:///icons/refresh.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: backend.followLatest() },
                ActionItem { title: "下一段"; imageSource: "asset:///icons/next.png"; enabled: !backend.following; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: backend.moveWindow(1) },
                ActionItem { title: "选择日期"; imageSource: "asset:///icons/history.png"; onTriggered: calendar.open() },
                ActionItem { title: "曲线"; imageSource: "asset:///icons/settings.png"; onTriggered: displaySettings.open() }
            ]
        }
    }
    Tab {
        id: recordTab
        title: "记录"
        imageSource: "asset:///icons/history.png"
        Page {
            titleBar: TitleBar { title: root.showEvents ? "电池事件与提醒" : "充放电记录" }
            Container {
                leftPadding: ui.du(1); rightPadding: ui.du(1); topPadding: ui.du(1); bottomPadding: ui.du(12)
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
                    textStyle.color: Color.create("#b1b6bc")
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
            titleBar: TitleBar { title: "数据诊断" }
            ScrollView {
                id: diagnosisScroll
                Container {
                    leftPadding: ui.du(2); rightPadding: ui.du(2); topPadding: ui.du(1); bottomPadding: ui.du(14)
                    Label { text: backend.quality.summary || "等待采集"; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#efc56a") }
                    Label { text: "实时参数"; textStyle.fontSize: FontSize.Medium }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "电池电压"; value: backend.live.voltageText || "--"; unit: "V" }
                        Metric { label: "剩余容量"; value: backend.live.remainingText || "未提供" }
                    }
                    Label { text: "电源状态  " + (backend.live.chargerRaw || "--") + "    充电阶段  " + (backend.live.phase || "--"); multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "实际输入电流  " + (backend.live.inputText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "系统充电电流  " + (backend.live.systemChargeText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "系统预估耗尽  " + (backend.live.timeToEmptyText || "未提供") + "\n系统预估充满  " + (backend.live.timeToFullText || "不适用"); multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    Label { text: "输入限值  " + (backend.live.inputLimitText || "未提供") + "    充电限值  " + (backend.live.chargeLimitText || "未提供"); multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    Divider {}
                    Label { text: "容量与健康"; textStyle.fontSize: FontSize.Medium }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "设计容量"; value: backend.live.designText || "未提供" }
                        Metric { label: "电量计满充估计"; value: backend.live.fullText || "未提供" }
                    }
                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Metric { label: "系统自报健康度"; value: backend.live.healthText || "未提供" }
                        Metric { label: "系统自报循环次数"; value: backend.live.cyclesText || "未提供" }
                    }
                    Label { text: "独立容量实测  尚无结果"; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    Divider {}
                    Label { text: "当前历史区间"; textStyle.fontSize: FontSize.Medium }
                    Label { text: backend.history.rangeText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    Label { text: backend.history.summaryText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    ProgressIndicator { value: (backend.history.coverage || 0) / 100; horizontalAlignment: HorizontalAlignment.Fill }
                    Label { text: "电流时间均值  " + (backend.history.currentMeanText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "电流最小 / 最大  " + (backend.history.currentRangeText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "电压时间均值  " + (backend.history.voltageMeanText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "温度时间均值  " + (backend.history.temperatureMeanText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "温度最小 / 最大  " + (backend.history.temperatureRangeText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "充入能量  " + (backend.history.chargeMwh === undefined || backend.history.chargeMwh === null ? "未提供" : Number(backend.history.chargeMwh).toFixed(1) + " mWh") + "\n放出能量  " + (backend.history.dischargeMwh === undefined || backend.history.dischargeMwh === null ? "未提供" : Number(backend.history.dischargeMwh).toFixed(1) + " mWh"); multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Divider {}
                    Label { text: "全历史数据质量"; textStyle.fontSize: FontSize.Medium }
                    Label { text: backend.quality.historyText || ""; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "有效读数比例  电流 " + (backend.quality.currentCoverage || 0) + "% · 电压 " + (backend.quality.voltageCoverage || 0) + "% · 温度 " + (backend.quality.temperatureCoverage || 0) + "%"; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: backend.quality.directionText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    Label { text: "缺失字段  " + (backend.quality.missingText || ""); multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Divider {}
                    Label { text: "采集器"; textStyle.fontSize: FontSize.Medium }
                    Label { text: backend.status; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "最近采样  " + (backend.live.sampleAge || "未知") + "前"; textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "系统数值未变  " + (backend.quality.sourceAge || "未知"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "记录存储  " + (backend.quality.storageText || "0 MB"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: backend.quality.period || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                }
            }
        }
    }
    attachedObjects: [
        SystemToast { id: toast },
        Sheet {
            id: settings
            Page {
                titleBar: TitleBar { title: "采集与提醒" }
                ScrollView {
                    Container {
                        leftPadding: ui.du(2); rightPadding: ui.du(2); topPadding: ui.du(1); bottomPadding: ui.du(14)
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Label { text: "持续采集"; verticalAlignment: VerticalAlignment.Center; layoutProperties: StackLayoutProperties { spaceQuota: 1 } }
                            ToggleButton { checked: !backend.paused; onCheckedChanged: if (checked === backend.paused) backend.configure(backend.interval, !checked) }
                        }
                        DropDown {
                            title: "采样间隔"
                            options: [
                                Option { text: "5 秒"; value: 5; selected: backend.interval === 5 },
                                Option { text: "10 秒"; value: 10; selected: backend.interval === 10 },
                                Option { text: "30 秒"; value: 30; selected: backend.interval === 30 },
                                Option { text: "60 秒"; value: 60; selected: backend.interval === 60 }
                            ]
                            onSelectedValueChanged: if (selectedValue && Number(selectedValue) !== backend.interval) backend.configure(Number(selectedValue), backend.paused)
                        }
                        Divider {}
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Label { text: "电池提醒"; verticalAlignment: VerticalAlignment.Center; layoutProperties: StackLayoutProperties { spaceQuota: 1 } }
                            ToggleButton {
                                checked: backend.alerts.enabled === true
                                onCheckedChanged: if (checked !== backend.alerts.enabled) backend.configureAlerts(checked, Number(backend.alerts.low), Number(backend.alerts.high), Number(backend.alerts.temperature))
                            }
                        }
                        Label { text: "低电量  " + Math.round(lowSlider.immediateValue) + "%"; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Slider {
                            id: lowSlider
                            fromValue: 5; toValue: 40; value: Number(backend.alerts.low || 20); enabled: backend.alerts.enabled === true
                            onValueChanged: if (Math.round(value) !== Number(backend.alerts.low)) backend.configureAlerts(backend.alerts.enabled, Math.round(value), Number(backend.alerts.high), Number(backend.alerts.temperature))
                        }
                        Label { text: "充电电量  " + Math.round(highSlider.immediateValue) + "%"; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Slider {
                            id: highSlider
                            fromValue: 60; toValue: 100; value: Number(backend.alerts.high || 90); enabled: backend.alerts.enabled === true
                            onValueChanged: if (Math.round(value) !== Number(backend.alerts.high)) backend.configureAlerts(backend.alerts.enabled, Number(backend.alerts.low), Math.round(value), Number(backend.alerts.temperature))
                        }
                        Label { text: "温度  " + Math.round(tempSlider.immediateValue) + "°C"; textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Slider {
                            id: tempSlider
                            fromValue: 35; toValue: 55; value: Number(backend.alerts.temperature || 45); enabled: backend.alerts.enabled === true
                            onValueChanged: if (Math.round(value) !== Number(backend.alerts.temperature)) backend.configureAlerts(backend.alerts.enabled, Number(backend.alerts.low), Number(backend.alerts.high), Math.round(value))
                        }
                        Label { text: backend.status; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                    }
                }
                actions: [ ActionItem { title: "完成"; imageSource: "asset:///icons/close.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: settings.close() } ]
            }
        },
        Sheet {
            id: displaySettings
            Page {
                titleBar: TitleBar { title: "历史曲线" }
                Container {
                    leftPadding: ui.du(2); rightPadding: ui.du(2); topPadding: ui.du(2)
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
                titleBar: TitleBar { title: "历史日期" }
                Container {
                    leftPadding: ui.du(2); rightPadding: ui.du(2); topPadding: ui.du(2)
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
                titleBar: TitleBar { title: backend.detail.title || "过程详情" }
                ScrollView {
                    Container {
                        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(14)
                        Label { text: backend.detail.timeText || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#b1b6bc") }
                        Label { text: backend.detail.socText || ""; textStyle.fontSize: FontSize.Large }
                        Label { text: (backend.detail.durationText || "") + " · " + (backend.detail.status || ""); textStyle.base: SystemDefaults.TextStyles.SmallText }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Metric { label: "累计电量"; value: backend.detail.chargeText || "--" }
                            Metric { label: "有效积分覆盖"; value: backend.detail.coverageText || "--" }
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
                titleBar: TitleBar { title: "过程诊断" }
                Container {
                    leftPadding: ui.du(2); rightPadding: ui.du(2); topPadding: ui.du(2)
                    Label { text: "区间外推容量  " + (backend.detail.estimateText || "条件不足"); multiline: true; textStyle.fontSize: FontSize.Medium }
                    Label { text: backend.detail.evaluation || ""; multiline: true; textStyle.base: SystemDefaults.TextStyles.SmallText; textStyle.color: Color.create("#efc56a") }
                    Divider {}
                    Label { text: "累计能量  " + (backend.detail.energyText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "电流时间均值  " + (backend.detail.currentMeanText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "最高温度  " + (backend.detail.temperatureMaxText || "未提供"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "采样数  " + (backend.detail.samplesText || "0"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                    Label { text: "结束原因  " + (backend.detail.reason || "进行中"); textStyle.base: SystemDefaults.TextStyles.SmallText }
                }
                actions: [ ActionItem { title: "关闭"; imageSource: "asset:///icons/close.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: detailDiagnostics.close() } ]
            }
        },
        Connections { target: backend; onExported: { toast.body = message; toast.show(); } onAlerted: { toast.body = message; toast.show(); } }
    ]
}
