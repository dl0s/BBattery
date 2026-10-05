import bb.cascades 1.4
Page {
    objectName: "testRecords"
    property variant navigation
    titleBar: TitleBar { title: (backend.battery.viewedLabel || "电池") + " · 测试记录" }
    actions: [ ActionItem { title: "更早记录"; enabled: backend.hasMore; ActionBar.placement: ActionBarPlacement.OnBar; onTriggered: backend.loadMore() } ]
    Container {
        leftPadding: ui.du(1.5); rightPadding: ui.du(1.5)
        Label { visible: !backend.ready || backend.pending; text: backend.status; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#f0d39b") }
        Label { visible: backend.resultCount === 0; text: "暂无定时测试记录。历史监测片段仍保留，可导出全部原始数据。"; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
        ListView {
            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
            dataModel: backend.results
            listItemComponents: [ ListItemComponent { type: ""; StandardListItem { title: ListItemData.title; description: ListItemData.subtitle } } ]
            onTriggered: { backend.selectTest(dataModel.data(indexPath).id); navigation.openPage(resultDefinition); }
        }
    }
    attachedObjects: [ ComponentDefinition { id: resultDefinition; source: "TestResult.qml" } ]
}
