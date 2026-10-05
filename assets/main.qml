import bb.cascades 1.4
import bb.system 1.2
import QtQuick 1.0

NavigationPane {
    id: root
    objectName: "batteryOverview"
    function openPage(definition) { var page = definition.createObject(); page.navigation = root; push(page); }
    function testView(args) {
        if (args.batteries === true) { nameSheet.renaming = false; editName.text = ""; nameSheet.open(); }
        if (args.result === true) openPage(resultDefinition);
        if (args.close === true) { nameSheet.close(); pop(); }
        return { page: top.objectName, records: backend.resultCount };
    }
    onPopTransitionEnded: page.destroy()
    Menu.definition: MenuDefinition {
        actions: [
            ActionItem { title: "刷新"; imageSource: "asset:///icons/refresh.png"; onTriggered: backend.refresh() },
            ActionItem { title: "导出全部原始数据"; imageSource: "asset:///icons/save.png"; onTriggered: backend.exportData(true) },
            ActionItem { title: "保存当前页面截图"; imageSource: "asset:///icons/save.png"; onTriggered: backend.captureScreen() }
        ]
    }
    Page {
        objectName: "batteryList"
        titleBar: TitleBar { title: "电池 · 容量总览" }
        actions: [
            ActionItem { title: "新增电池"; imageSource: "asset:///icons/battery.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: { nameSheet.renaming = false; editName.text = ""; nameSheet.open(); } },
            ActionItem { title: backend.running ? "查看测试" : "开始测试"; imageSource: "asset:///icons/charge.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: root.openPage(testDefinition) }
        ]
        Container {
            leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(0.7); bottomPadding: ui.du(0.5)
            Label { text: "整电池预估容量 · mAh"; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
            Label {
                visible: backend.pending || !backend.ready
                text: backend.status; multiline: true; autoSize.maxLineCount: 3
                textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#f0d39b")
            }
            Button {
                visible: backend.running
                text: backend.pending ? "测试操作待确认 · 查看" : (backend.test.modeText || "测试中") + " · 剩余 " + (backend.test.remainingText || "--")
                horizontalAlignment: HorizontalAlignment.Fill; onClicked: root.openPage(testDefinition)
            }
            Label { visible: backend.batteries.length === 0; text: "尚未建立电池档案，请点击“新增电池”。"; multiline: true }
            ListView {
                id: batteryList
                objectName: "batteryListView"
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                dataModel: backend.batteryModel
                function itemType(data, indexPath) { return "battery"; }
                listItemComponents: [
                    ListItemComponent {
                        type: "battery"
                        Container {
                            id: row
                            minHeight: ui.du(17); horizontalAlignment: HorizontalAlignment.Fill
                            background: Color.create(row.ListItem.active ? "#294657" : "#19232c")
                            leftPadding: ui.du(1.2); rightPadding: ui.du(1.2); topPadding: ui.du(1); bottomPadding: ui.du(1); bottomMargin: ui.du(0.8)
                            Container {
                                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                                Label { text: ListItemData.label; multiline: true; autoSize.maxLineCount: 2; layoutProperties: StackLayoutProperties { spaceQuota: 1 } textStyle.fontWeight: FontWeight.W500 }
                                Label { text: ListItemData.active ? "当前使用" : "未装入"; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create(ListItemData.active ? "#86debb" : "#b9c7d2") }
                            }
                            CapacityDisplay { summary: ListItemData.capacity }
                            Label { visible: ListItemData.testing; text: "测试中 · 保留上次有效估计"; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#8dd5f4") }
                        }
                    }
                ]
                onTriggered: { backend.viewBattery(String(dataModel.data(indexPath).key)); backend.selectTest(""); root.openPage(detailDefinition); }
            }
        }
    }
    attachedObjects: [
        ComponentDefinition { id: detailDefinition; source: "BatteryDetail.qml" },
        ComponentDefinition { id: testDefinition; source: "TestPage.qml" },
        ComponentDefinition { id: resultDefinition; source: "TestResult.qml" },
        SystemToast { id: toast },
        Connections { target: backend; onNotified: { toast.body = message; toast.show(); } },
        Sheet {
            id: nameSheet
            property bool renaming: false
            Page {
                titleBar: TitleBar {
                    title: nameSheet.renaming ? "重命名电池" : "新增电池"
                    dismissAction: ActionItem { title: "取消"; onTriggered: nameSheet.close() }
                    acceptAction: ActionItem { title: "保存"; enabled: editName.text.trim().length > 0; onTriggered: { var ok = nameSheet.renaming ? backend.renameBattery(backend.battery.viewedKey, editName.text) : !!backend.createBattery(editName.text); if (ok) nameSheet.close(); } }
                }
                ScrollView {
                    scrollViewProperties.scrollMode: ScrollMode.Vertical
                    Container {
                        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1)
                        TextField { id: editName; hintText: "电池名称（1–40 字）"; maximumLength: 40 }
                        Label { text: nameSheet.renaming ? "名称改变后，电池身份与全部历史记录继续保留。" : "每块实体电池建立一次档案。重新装回时，选择已有电池并设为当前。"; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
                    }
                }
            }
        }
    ]
}
