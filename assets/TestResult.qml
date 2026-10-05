import bb.cascades 1.4
Page {
    id: page
    objectName: "testResult"
    property variant navigation
    property bool metricsExpanded: false
    titleBar: TitleBar { title: "测试详情" }
    ScrollView {
        scrollViewProperties.scrollMode: ScrollMode.Vertical
        Container {
            leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(2)
            Label { text: (backend.detail.battery_label || "") + " · " + (backend.detail.stateText || "正在读取…"); multiline: true }
            Label { text: "本次整电池估计 · " + (backend.detail.capacityText || "--"); multiline: true; textStyle.fontSize: FontSize.XLarge }
            Label { text: backend.detail.estimateNote || ""; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
            Label { text: backend.detail.modeText || ""; textStyle.fontSize: FontSize.Small }
            Label { text: "估计/结束时间 · " + (backend.detail.endText || "--"); multiline: true; textStyle.fontSize: FontSize.Small }
            Label { text: "来源记录 · " + (backend.detail.id || "--"); multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
            Label { text: "充放电明细"; textStyle.fontWeight: FontWeight.W500 }
            Metric { label: "本次区间电量（不是整电池容量）"; value: backend.detail.mahText || "--" }
            Metric { label: "SOC 起止"; value: backend.detail.socText || "--" }
            Metric { label: "测试时长"; value: backend.detail.elapsedText || "--" }
            Metric { label: "电流积分覆盖"; value: backend.detail.coverageText || "--" }
            Label { text: backend.detail.reasonText || ""; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#f0d39b") }
            Button { text: page.metricsExpanded ? "收起更多数据" : "更多采样数据"; horizontalAlignment: HorizontalAlignment.Fill; onClicked: page.metricsExpanded = !page.metricsExpanded }
            Container {
                visible: page.metricsExpanded
                Label { text: "开始 · " + (backend.detail.startText || "--"); textStyle.fontSize: FontSize.Small }
                Metric { label: "本次区间电能"; value: backend.detail.mwhText || "--" }
                Metric { label: "有效积分区间平均电流"; value: backend.detail.meanCurrentText || "--" }
                Metric { label: "数据缺口次数"; value: backend.detail.gapsText || "--" }
                Metric { label: "末次电压 · 历史采样"; value: backend.reading.voltageText || "--" }
                Metric { label: "末次温度 · 历史采样"; value: backend.reading.temperatureText || "--" }
            }
            Button { text: "导出本次 CSV / JSON"; horizontalAlignment: HorizontalAlignment.Fill; enabled: !!backend.detail.id && backend.detail.status !== "running"; onClicked: backend.exportData(false) }
        }
    }
}
