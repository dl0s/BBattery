import bb.cascades 1.4

Container {
    id: chart
    property string title: ""
    property variant plot
    property variant axes
    property variant cursor
    property bool compact: false
    property bool inspectable: false
    horizontalAlignment: HorizontalAlignment.Fill
    Label {
        text: title
        textStyle.base: SystemDefaults.TextStyles.SmallText
        textStyle.color: Color.create("#d9dfe3")
        bottomMargin: 0
    }
    Container {
        id: surface
        horizontalAlignment: HorizontalAlignment.Fill
        preferredHeight: compact ? ui.du(13) : ui.du(19)
        minHeight: preferredHeight
        maxHeight: preferredHeight
        layout: AbsoluteLayout {}
        property real w: chartLayout.layoutFrame.width
        property real h: chartLayout.layoutFrame.height
        property real sourceHeight: axes && axes.height ? axes.height : 228
        property real labelHeight: ui.du(2.2)
        attachedObjects: [ LayoutUpdateHandler { id: chartLayout } ]
        ImageView {
            image: plot
            preferredWidth: surface.w
            preferredHeight: surface.h
            scalingMethod: ScalingMethod.Fill
            onTouch: {
                if (inspectable && (event.isDown() || event.isMove() || event.isUp()))
                    backend.inspect(event.localX, surface.w, event.isDown() || event.isUp());
                else if (inspectable && event.isCancel()) backend.clearInspection();
            }
        }
        Container {
            visible: chart.inspectable && cursor && cursor.visible === true
            preferredWidth: 2; minWidth: 2; maxWidth: 2
            preferredHeight: surface.h * (surface.sourceHeight - 66) / surface.sourceHeight
            background: Color.create("#f1f3f4")
            layoutProperties: AbsoluteLayoutProperties {
                positionX: surface.w * (76 + 624 * (cursor && cursor.ratio ? cursor.ratio : 0)) / 720
                positionY: surface.h * 28 / surface.sourceHeight
            }
        }
        Label {
            text: axes ? axes.high : ""
            preferredWidth: surface.w * 64 / 720
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#b1b6bc")
            textStyle.textAlign: TextAlign.Right
            layoutProperties: AbsoluteLayoutProperties { positionX: 0; positionY: surface.h * 28 / surface.sourceHeight - surface.labelHeight / 2 }
        }
        Label {
            text: axes ? axes.middle : ""
            preferredWidth: surface.w * 64 / 720
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#b1b6bc")
            textStyle.textAlign: TextAlign.Right
            layoutProperties: AbsoluteLayoutProperties { positionX: 0; positionY: surface.h * (surface.sourceHeight - 10) / (2 * surface.sourceHeight) - surface.labelHeight / 2 }
        }
        Label {
            text: axes ? axes.low : ""
            preferredWidth: surface.w * 64 / 720
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#b1b6bc")
            textStyle.textAlign: TextAlign.Right
            layoutProperties: AbsoluteLayoutProperties { positionX: 0; positionY: surface.h * (surface.sourceHeight - 38) / surface.sourceHeight - surface.labelHeight / 2 }
        }
        Label {
            text: axes ? axes.start : ""
            preferredWidth: surface.w * 130 / 720
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#b1b6bc")
            layoutProperties: AbsoluteLayoutProperties { positionX: surface.w * 76 / 720; positionY: surface.h * (surface.sourceHeight - 31) / surface.sourceHeight }
        }
        Label {
            text: axes ? axes.center : ""
            preferredWidth: surface.w * 130 / 720
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#b1b6bc")
            textStyle.textAlign: TextAlign.Center
            layoutProperties: AbsoluteLayoutProperties { positionX: surface.w * 323 / 720; positionY: surface.h * (surface.sourceHeight - 31) / surface.sourceHeight }
        }
        Label {
            text: axes ? axes.end : ""
            preferredWidth: surface.w * 130 / 720
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#b1b6bc")
            textStyle.textAlign: TextAlign.Right
            layoutProperties: AbsoluteLayoutProperties { positionX: surface.w * 570 / 720; positionY: surface.h * (surface.sourceHeight - 31) / surface.sourceHeight }
        }
        Label {
            text: axes ? axes.empty : ""
            visible: text !== ""
            preferredWidth: surface.w * 624 / 720
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#b1b6bc")
            textStyle.textAlign: TextAlign.Center
            layoutProperties: AbsoluteLayoutProperties { positionX: surface.w * 76 / 720; positionY: surface.h / 2 - surface.labelHeight / 2 }
        }
    }
}
