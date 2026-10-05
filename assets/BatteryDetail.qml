import bb.cascades 1.4
import bb.system 1.2
Page {
    id: page
    objectName: "batteryDetail"
    property variant navigation
    property bool readingsExpanded: false
    function open(definition) { navigation.openPage(definition); }
    titleBar: TitleBar { title: backend.battery.viewedLabel || "电池详情" }
    actions: [
        ActionItem { title: "重命名"; imageSource: "asset:///icons/settings.png"; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: { renameDialog.inputField.defaultText = backend.battery.viewedLabel; renameDialog.show(); } },
        ActionItem { title: "设为当前"; enabled: backend.ready && !backend.running && !backend.pending && backend.battery.viewedKey !== backend.battery.activeKey; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: backend.useBattery(backend.battery.viewedKey) }
    ]
    ScrollView {
        scrollViewProperties.scrollMode: ScrollMode.Vertical
        Container {
            leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(2)
            Label { text: backend.battery.viewedKey === backend.battery.activeKey ? "当前使用" : "历史电池 · 查看不会改变采集归属"; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
            Label { visible: !backend.ready || backend.pending; text: backend.status; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#f0d39b") }
            Label { text: "整电池预估容量"; textStyle.fontSize: FontSize.Small }
            CapacityDisplay { summary: backend.capacity }
            Label { text: backend.capacity.note || ""; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
            Button { visible: backend.capacity.available && backend.capacity.sourceType !== "system-reference"; text: "查看容量依据"; horizontalAlignment: HorizontalAlignment.Fill; onClicked: { backend.selectTest(backend.capacity.sourceId); page.open(resultDefinition); } }
            Button { text: "测试记录"; horizontalAlignment: HorizontalAlignment.Fill; onClicked: page.open(recordsDefinition) }
            Button { text: backend.running ? "查看当前测试" : "开始测试当前电池"; horizontalAlignment: HorizontalAlignment.Fill; enabled: backend.running || backend.battery.viewedKey === backend.battery.activeKey; onClicked: page.open(testDefinition) }
            Label { visible: backend.battery.viewedKey !== backend.battery.activeKey; text: "测试此电池前，请先装入并设为当前。测试或操作待确认时不能切换。"; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
            Button { text: page.readingsExpanded ? "收起充放电读数" : "充放电读数"; horizontalAlignment: HorizontalAlignment.Fill; onClicked: page.readingsExpanded = !page.readingsExpanded }
            Container {
                visible: page.readingsExpanded
                Label { text: backend.reading.live ? "当前测试实时读数" : "历史测试末次读数 · 不代表当前电量"; multiline: true; textStyle.fontSize: FontSize.Small }
                Label { text: "采样时间 · " + (backend.reading.lastText || "--"); textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
                Metric { label: "剩余电量百分比"; value: backend.reading.socText || "--" }
                Metric { label: "采样平均电流"; value: backend.reading.currentText || "--" }
                Metric { label: "电压"; value: backend.reading.voltageText || "--" }
                Metric { label: "温度"; value: backend.reading.temperatureText || "--" }
            }
        }
    }
    attachedObjects: [
        ComponentDefinition { id: resultDefinition; source: "TestResult.qml" },
        ComponentDefinition { id: recordsDefinition; source: "TestRecords.qml" },
        ComponentDefinition { id: testDefinition; source: "TestPage.qml" },
        SystemPrompt { id: renameDialog; title: "重命名电池"; body: "名称 1–40 字，身份和历史保持"; confirmButton.label: "保存"; cancelButton.label: "取消"; onFinished: { if (value === SystemUiResult.ConfirmButtonSelection) backend.renameBattery(backend.battery.viewedKey, inputFieldTextEntry()); } }
    ]
}
