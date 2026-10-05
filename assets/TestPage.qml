import bb.cascades 1.4
Page {
    objectName: "timedTest"
    property variant navigation
    titleBar: TitleBar { title: "定时容量测试" }
    ScrollView {
        scrollViewProperties.scrollMode: ScrollMode.Vertical
        Container {
            leftPadding: ui.du(1.5); rightPadding: ui.du(1.5); topPadding: ui.du(1); bottomPadding: ui.du(2)
            Label { text: "当前电池 · " + (backend.battery.activeLabel || "--"); multiline: true }
            Label { text: backend.status; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#f0d39b") }
            Button { visible: !backend.ready && !backend.pending; text: "重新连接采集器"; horizontalAlignment: HorizontalAlignment.Fill; onClicked: backend.connectCollector() }
            Container {
                visible: !backend.running
                TextField { id: minutes; hintText: "测试时长（分钟，1–1440）"; text: "60"; inputMode: TextFieldInputMode.NumbersAndPunctuation; enabled: !backend.pending }
                DropDown {
                    id: intervalChoice; title: "采样间隔"; enabled: !backend.pending
                    Option { text: "30 秒 · 省电"; value: 30; selected: true }
                    Option { text: "10 秒 · 较细"; value: 10 }
                    Option { text: "60 秒 · 较低负荷"; value: 60 }
                }
                Button {
                    text: backend.pending ? "操作待确认…" : "开始测试"
                    horizontalAlignment: HorizontalAlignment.Fill
                    enabled: backend.ready && !backend.pending && /^[0-9]+$/.test(minutes.text) && Number(minutes.text) >= 1 && Number(minutes.text) <= 1440
                    onClicked: backend.startTest(Number(minutes.text), Number(intervalChoice.selectedValue))
                }
                Label { text: "开始时识别充放电方向，到时自动结束。已建立的后台采集器继续执行测试；待机不采样。容量估计需要电量变化至少 30 个百分点。"; multiline: true; textStyle.fontSize: FontSize.Small; textStyle.color: Color.create("#b9c7d2") }
            }
            Container {
                visible: backend.running
                Label { text: (backend.test.modeText || "测试中") + " · 剩余 " + (backend.test.remainingText || "--"); multiline: true; textStyle.color: Color.create("#8dd5f4") }
                Metric { label: "本次区间电量（不是整电池容量）"; value: backend.test.mahText || "--" }
                Metric { label: "SOC 起止"; value: backend.test.socText || "--" }
                Metric { label: "已测时长"; value: backend.test.elapsedText || "--" }
                Metric { label: "电流积分覆盖"; value: backend.test.coverageText || "--" }
                Button { text: backend.pending ? "正在确认结束…" : "结束并保存"; horizontalAlignment: HorizontalAlignment.Fill; enabled: backend.ready && !backend.pending; onClicked: backend.stopTest() }
            }
        }
    }
}
