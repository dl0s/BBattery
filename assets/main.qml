import bb.cascades 1.4
import bb.system 1.2
import QtQuick 1.0

TabbedPane {
    id: root
    objectName: "batteryTests"
    showTabsOnActionBar: true
    property bool rebuildingBatteries: false
    property string batteryOptionsState: ""
    function syncBatteries() {
        rebuildingBatteries = true;
        var items = backend.batteries;
        var signature = JSON.stringify(items);
        if (signature !== batteryOptionsState) {
            batteryChoice.removeAll();
            for (var i = 0; i < items.length; ++i) {
                var option = batteryOption.createObject();
                option.text = items[i].label;
                option.value = items[i].key;
                batteryChoice.add(option);
            }
            batteryOptionsState = signature;
        }
        for (var j = 0; j < items.length; ++j) {
            if (items[j].key === backend.battery.viewedKey) batteryChoice.selectedIndex = j;
        }
        rebuildingBatteries = false;
    }
    function testView(args) {
        if (args.tab !== undefined) activeTab = Number(args.tab) === 1 ? resultsTab : testTab;
        if (args.batteries === true) { syncBatteries(); batterySheet.open(); }
        if (args.result === true) resultSheet.open();
        if (args.close === true) { batterySheet.close(); resultSheet.close(); }
        return { tab: activeTab === resultsTab ? 1 : 0, records: backend.resultCount };
    }
    Menu.definition: MenuDefinition {
        actions: [
            ActionItem { title: "电池标记"; imageSource: "asset:///icons/battery.png"; onTriggered: { root.syncBatteries(); batterySheet.open(); } },
            ActionItem { title: "导出全部原始数据"; imageSource: "asset:///icons/save.png"; onTriggered: backend.exportData(true) }
        ]
    }
    Tab {
        id: testTab
        title: "测试"
        imageSource: "asset:///icons/battery.png"
        Page {
            ScrollView {
                scrollViewProperties.scrollMode: ScrollMode.Vertical
                Container {
                    leftPadding: ui.du(1.5); rightPadding: ui.du(1.5)
                    topPadding: ui.du(1.5); bottomPadding: ui.du(1)
                    Label { text: "电池容量测试"; textStyle.fontSize: FontSize.Large; textStyle.fontWeight: FontWeight.W500 }
                    Label {
                        text: "当前电池 · " + (backend.battery.activeLabel || "电池 1")
                        textStyle.color: Color.create("#93a4b2")
                        textStyle.fontSize: FontSize.Small
                    }
                    Container {
                        visible: !backend.running
                        Label { text: "测试时长（分钟）"; textStyle.fontSize: FontSize.Small }
                        TextField {
                            id: minutes
                            hintText: "测试时长（分钟，1–1440）"
                            text: "60"
                            inputMode: TextFieldInputMode.NumbersAndPunctuation
                            enabled: !backend.pending
                        }
                        DropDown {
                            id: intervalChoice
                            title: "采样间隔"
                            enabled: !backend.pending
                            Option { text: "30 秒 · 省电"; value: 30; selected: true }
                            Option { text: "10 秒 · 较细"; value: 10 }
                            Option { text: "60 秒 · 较低负荷"; value: 60 }
                        }
                        Button {
                            text: backend.pending ? "正在确认…" : "开始测试"
                            horizontalAlignment: HorizontalAlignment.Fill
                            enabled: backend.ready && !backend.pending && /^[0-9]+$/.test(minutes.text) && Number(minutes.text) >= 1 && Number(minutes.text) <= 1440
                            onClicked: backend.startTest(Number(minutes.text), Number(intervalChoice.selectedValue))
                        }
                        Label {
                            text: "开始时自动识别充电或放电。到时自动结束，关闭界面仍继续测试。"
                            multiline: true
                            textStyle.fontSize: FontSize.Small
                            textStyle.color: Color.create("#93a4b2")
                        }
                    }
                    Container {
                        visible: backend.running
                        Label {
                            text: (backend.test.modeText || "测试中") + " · 剩余 " + (backend.test.remainingText || "--")
                            textStyle.color: Color.create("#78c9ee")
                        }
                        Label {
                            text: backend.test.mahText || "--"
                            textStyle.fontSize: FontSize.XXLarge
                            textStyle.fontWeight: FontWeight.W500
                        }
                        Label {
                            text: "电量 " + (backend.test.socText || "--") + " · 已测 " + (backend.test.elapsedText || "--")
                            textStyle.fontSize: FontSize.Small
                        }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Metric { label: "电池平均电流"; value: backend.live.currentText || "--" }
                            Metric { label: "电池温度"; value: backend.live.temperatureText || "--" }
                        }
                        Label {
                            text: "采样更新 " + (backend.live.lastText || "--")
                            textStyle.fontSize: FontSize.Small
                            textStyle.color: Color.create("#93a4b2")
                        }
                        Button {
                            text: backend.pending ? "正在确认…" : "结束并保存"
                            horizontalAlignment: HorizontalAlignment.Fill
                            enabled: backend.ready && !backend.pending
                            onClicked: backend.stopTest()
                        }
                    }
                    Label {
                        text: backend.status
                        multiline: true
                        textStyle.fontSize: FontSize.Small
                        textStyle.color: Color.create("#93a4b2")
                    }
                }
            }
            actions: [ ActionItem { title: "电池标记"; imageSource: "asset:///icons/battery.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: { root.syncBatteries(); batterySheet.open(); } } ]
        }
    }
    Tab {
        id: resultsTab
        title: "结果"
        imageSource: "asset:///icons/save.png"
        Page {
            Container {
                leftPadding: ui.du(1); rightPadding: ui.du(1); topPadding: ui.du(1)
                Label { text: (backend.battery.viewedLabel || "电池 1") + " · 测试结果"; textStyle.fontSize: FontSize.Large }
                Label {
                    visible: backend.resultCount === 0
                    text: "暂无测试结果。选择电池后开始一次定时测试；旧原始记录可从菜单导出。"
                    multiline: true
                    textStyle.fontSize: FontSize.Small
                    textStyle.color: Color.create("#93a4b2")
                }
                ListView {
                    id: resultList
                    dataModel: backend.results
                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    listItemComponents: [ ListItemComponent {
                        type: ""
                        StandardListItem { title: ListItemData.title; description: ListItemData.subtitle; status: ListItemData.socText }
                    } ]
                    onTriggered: { backend.selectTest(dataModel.data(indexPath).id); resultSheet.open(); }
                }
                Button { text: "加载更早结果"; visible: backend.hasMore; onClicked: backend.loadMore() }
            }
            actions: [ ActionItem { title: "选择电池"; imageSource: "asset:///icons/battery.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: { root.syncBatteries(); batterySheet.open(); } } ]
        }
    }
    attachedObjects: [
        SystemToast { id: toast },
        ComponentDefinition { id: batteryOption; Option {} },
        Connections {
            target: backend
            onBatteriesChanged: root.syncBatteries()
            onNotified: { toast.body = message; toast.show(); }
        },
        Sheet {
            id: batterySheet
            Page {
                titleBar: TitleBar { title: "电池标记"; dismissAction: ActionItem { title: "完成"; onTriggered: batterySheet.close() } }
                ScrollView {
                    scrollViewProperties.scrollMode: ScrollMode.Vertical
                    Container {
                        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1)
                        Label { text: "当前 · " + (backend.battery.activeLabel || "电池 1") }
                        DropDown {
                            id: batteryChoice
                            title: "选择已有电池"
                            onSelectedValueChanged: if (!root.rebuildingBatteries && selectedValue) backend.viewBattery(String(selectedValue))
                        }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Button { text: "设为当前电池"; enabled: !backend.running && !backend.pending; layoutProperties: StackLayoutProperties { spaceQuota: 1 } onClicked: backend.useBattery(String(batteryChoice.selectedValue)) }
                            Button { text: "查看结果"; layoutProperties: StackLayoutProperties { spaceQuota: 1 } onClicked: { backend.viewBattery(String(batteryChoice.selectedValue)); root.activeTab = resultsTab; batterySheet.close(); } }
                        }
                        Label {
                            text: "每块电池使用一个标记。重新装回电池时，选择原有标记即可保留同一电池的测试记录。"
                            multiline: true
                            textStyle.fontSize: FontSize.Small
                            textStyle.color: Color.create("#93a4b2")
                        }
                        TextField { id: batteryName; hintText: "新标记或新名称（1–40 字）"; maximumLength: 40 }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Button { text: "新增标记"; layoutProperties: StackLayoutProperties { spaceQuota: 1 } onClicked: { var key = backend.createBattery(batteryName.text); if (key) batteryName.text = ""; } }
                            Button { text: "重命名"; layoutProperties: StackLayoutProperties { spaceQuota: 1 } onClicked: { if (backend.renameBattery(String(batteryChoice.selectedValue), batteryName.text)) batteryName.text = ""; } }
                        }
                    }
                }
            }
        },
        Sheet {
            id: resultSheet
            Page {
                titleBar: TitleBar { title: "测试结果"; dismissAction: ActionItem { title: "完成"; onTriggered: resultSheet.close() } }
                ScrollView {
                    scrollViewProperties.scrollMode: ScrollMode.Vertical
                    Container {
                        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(1)
                        Label { text: (backend.detail.battery_label || "") + " · " + (backend.detail.stateText || "读取中"); textStyle.fontSize: FontSize.Large }
                        Label { text: backend.detail.mahText || "--"; textStyle.fontSize: FontSize.XLarge }
                        Label { text: backend.detail.modeText || ""; textStyle.color: Color.create("#93a4b2") }
                        Label { text: "开始  " + (backend.detail.startText || "--"); textStyle.fontSize: FontSize.Small }
                        Label { text: "结束  " + (backend.detail.endText || "--"); textStyle.fontSize: FontSize.Small }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Metric { label: "测试时长"; value: backend.detail.elapsedText || "--" }
                            Metric { label: "电量变化"; value: backend.detail.socText || "--" }
                        }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            Metric { label: "电能"; value: backend.detail.mwhText || "--" }
                            Metric { label: "电流积分覆盖"; value: backend.detail.coverageText || "--" }
                        }
                        Label { text: "整电池容量估计  " + (backend.detail.capacityText || "--"); textStyle.fontSize: FontSize.Small }
                        Label { text: backend.detail.estimateNote || ""; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#93a4b2") }
                        Label { text: backend.detail.reasonText || ""; textStyle.fontSize: FontSize.Small }
                        Button { text: "导出本次数据"; enabled: !!backend.detail.id && backend.detail.status !== "running"; onClicked: backend.exportData(false) }
                    }
                }
            }
        }
    ]
}
