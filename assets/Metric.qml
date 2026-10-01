import bb.cascades 1.4

Container {
    property string label: ""
    property string value: "--"
    property string unit: ""
    property string accent: "#f1f3f4"
    minHeight: ui.du(6)
    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
    Label {
        text: label
        textStyle.base: SystemDefaults.TextStyles.SmallText
        textStyle.color: Color.create("#b1b6bc")
        bottomMargin: 0
        autoSize.maxLineCount: 1
    }
    Label {
        text: value + (unit !== "" && value !== "未提供" && value !== "--" ? " " + unit : "")
        textStyle.fontSize: FontSize.Large
        textStyle.fontWeight: FontWeight.W500
        textStyle.color: Color.create(accent)
        topMargin: 0
        multiline: true
        autoSize.maxLineCount: 2
    }
}
