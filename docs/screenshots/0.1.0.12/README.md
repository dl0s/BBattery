# 界面图来源

`sdk-*.png` 是 BB10 SDK 原生 Cascades qmlpreview.exe 生成的 720×720、Nevada（329 PPI）、深色界面预览。页面来自正式 QML，容量等数据来自独立假后端；bb.system 使用 Windows 预览桩。示例容量未进入 BAR 或正式数据库。

- `sdk-home.png`：容量总览，多电池和不同来源。
- `sdk-detail.png`：二级详情优先展示容量依据。
- `sdk-home-long-name.png`：长名称换行。
- `sdk-home-empty.png`：无档案且采集未就绪。
- `sdk-home-running.png`：紧凑测试状态。
- `sdk-home-pending.png`：操作待确认。

`preview-evidence.json` 记录源文件 SHA256、渲染器、隔离桩范围和生成日志路径。0.1.0.12 与这些 0.1.0.11 生成的页面源文件字节一致，随后变更仅为采集启动方式；交付前再次按 BAR 中页面核对摘要。

这些图片不是 Q10 实机截图，不能证明真机系统提示框、键盘遮挡、大字号、焦点及返回行为。实机图片另以 `device-` 命名，必须伴随本轮下载任务与应用截图证据。
