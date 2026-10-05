import bb.cascades 1.4
Container {
    property variant summary: ({available: false})
    Container {
        visible: summary.available === true
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        topMargin: ui.du(0.3)
        Label { text: "约 " + (summary.valueText || "--"); textStyle.fontSize: FontSize.XXLarge; textStyle.fontWeight: FontWeight.W500; textStyle.color: Color.create("#f1f3f4") }
        Label { text: "mAh"; verticalAlignment: VerticalAlignment.Bottom; bottomMargin: ui.du(0.6); textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
    }
    Label { visible: summary.available !== true; text: summary.unavailableReason === "loading" ? "正在读取容量…" : summary.unavailableReason === "database-read-failed" ? "容量读取失败" : "暂无可靠估计"; textStyle.fontSize: FontSize.Large; textStyle.color: Color.create("#f1f3f4") }
    Label { text: summary.available === true ? summary.sourceText + " · " + summary.timeText : "查看电池详情，开始容量测试"; multiline: true; autoSize.maxLineCount: 2; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
}
