import bb.cascades 1.4

Container {
    id: gauge
    property string soc: "--"
    property bool available: false
    property string mode: "unknown"
    property string subtitle: ""
    property real ratio: available ? Math.max(0, Math.min(1, Number(soc) / 100)) : 0
    property string accent: available && Number(soc) <= 20 ? "#f5a582" : mode === "charge" ? "#63d6a0" : "#78c9ee"
    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
    minHeight: ui.du(11)
    topMargin: ui.du(0.5); bottomMargin: ui.du(0.8)
    Container {
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
        verticalAlignment: VerticalAlignment.Center
        rightMargin: ui.du(2)
        Container {
            preferredWidth: ui.du(16); minWidth: preferredWidth; maxWidth: preferredWidth
            preferredHeight: ui.du(7); minHeight: preferredHeight; maxHeight: preferredHeight
            background: Color.create(available ? accent : "#40505c")
            leftPadding: ui.du(0.4); rightPadding: ui.du(0.4); topPadding: ui.du(0.4); bottomPadding: ui.du(0.4)
            Container {
                background: Color.create("#101820")
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Fill
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                layout: AbsoluteLayout {}
                attachedObjects: [ LayoutUpdateHandler { id: batteryLayout } ]
                Container {
                    visible: gauge.available && gauge.ratio > 0
                    preferredWidth: batteryLayout.layoutFrame.width * gauge.ratio
                    preferredHeight: batteryLayout.layoutFrame.height
                    background: Color.create(gauge.accent)
                }
            }
        }
        Container {
            preferredWidth: ui.du(0.8); minWidth: preferredWidth; maxWidth: preferredWidth
            preferredHeight: ui.du(2.6); minHeight: preferredHeight; maxHeight: preferredHeight
            verticalAlignment: VerticalAlignment.Center
            background: Color.create(available ? accent : "#40505c")
        }
    }
    Container {
        verticalAlignment: VerticalAlignment.Center
        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
        Container {
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            Label {
                text: gauge.available ? gauge.soc : "--"
                textStyle.fontSize: FontSize.PercentageValue
                textStyle.fontSizeValue: 240
                textStyle.fontWeight: FontWeight.W300
                textStyle.color: Color.create("#f1f5f7")
                topMargin: 0; bottomMargin: 0; rightMargin: ui.du(0.5)
            }
            Label {
                text: "%"; visible: gauge.available
                verticalAlignment: VerticalAlignment.Center
                textStyle.fontSize: FontSize.PercentageValue
                textStyle.fontSizeValue: 90
                textStyle.color: Color.create("#93a4b2")
                topMargin: 0; bottomMargin: 0
            }
        }
        Label {
            text: subtitle
            textStyle.fontSize: FontSize.PercentageValue
            textStyle.fontSizeValue: 65
            textStyle.color: Color.create(available ? accent : "#748896")
            topMargin: 0; bottomMargin: 0
        }
    }
}
