import bb.cascades 1.4

Container {
    property string label: ""
    property string value: "--"
    property string unit: ""
    property string accent: "#f1f3f4"
    property string note: ""
    property string displayValue: value === "未提供" || value === "不适用" || value === "条件不足" || value === "尚无有效积分" ? "--" : value
    background: Color.create("#19232c")
    leftPadding: ui.du(1); rightPadding: ui.du(1); topPadding: ui.du(0.8); bottomPadding: ui.du(0.8)
    rightMargin: ui.du(0.5); bottomMargin: ui.du(0.7)
    minHeight: ui.du(note !== "" ? 9.5 : 7.5)
    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
    Label {
        text: label
        textStyle.fontSize: FontSize.PercentageValue
        textStyle.fontSizeValue: 65
        textStyle.color: Color.create("#b9c7d2")
        topMargin: 0; bottomMargin: 0
        multiline: true; autoSize.maxLineCount: 2
    }
    Container {
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        topMargin: ui.du(0.2); bottomMargin: 0
        Label {
            text: displayValue
            textStyle.fontSize: FontSize.PercentageValue
            textStyle.fontSizeValue: 105
            textStyle.fontWeight: FontWeight.W500
            textStyle.color: Color.create(displayValue === "--" ? "#b9c7d2" : accent)
            topMargin: 0; bottomMargin: 0; rightMargin: ui.du(0.5)
            autoSize.maxLineCount: 1
        }
        Label {
            text: unit
            visible: unit !== "" && displayValue !== "--"
            verticalAlignment: VerticalAlignment.Bottom
            textStyle.fontSize: FontSize.PercentageValue
            textStyle.fontSizeValue: 60
            textStyle.color: Color.create("#b9c7d2")
            topMargin: 0; bottomMargin: ui.du(0.3)
        }
    }
    Label {
        text: note; visible: note !== ""
        textStyle.fontSize: FontSize.PercentageValue
        textStyle.fontSizeValue: 60
        textStyle.color: Color.create("#b9c7d2")
        topMargin: ui.du(0.2); bottomMargin: 0
        autoSize.maxLineCount: 1
    }
}
