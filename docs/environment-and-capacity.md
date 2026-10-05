# 运行环境、容量口径与数据恢复

本轮日期：2026-10-05（Asia/Shanghai）。固件基线为 **1.1.6-M**，现场标准文档与发布管理器/Core/CLI 为 **1.1.6.5**，BBattery 修订独立编号。用户最初指定的 1.1.6.2 管理器已被当前标准文档替代；不能将管理器版本写成固件版本。

操作唯一依据为 `C:/Users/dove/Documents/BBmanager/Documentation/标准操作文档.md`，发布 CLI 为 `C:/Users/dove/Documents/BBmanager/dist/Q10Manager/cli/Q10Manager.Cli.exe`。Autoloader 原 `docs/1.1.6-M开发与迁移.md` 已归档，当前入口为 `docs/1.1.6-M开发指南.md`。保留原固件十文件、root、SSH、主机信任、凭据、PathTrust 和系统启动链。

## 实际 SDK 和构建发现

| 项目 | 本轮核验值 |
| --- | --- |
| SDK | `C:/bbdevtools` |
| QNX_HOST | `host_10_3_1_12/win32/x86` |
| QNX_TARGET | `target_10_3_1_995/qnx6` |
| 编译变体 | `-V4.6.3,gcc_ntoarmv7le_cpp` |
| 运行库 ABI | ELF32 little endian ARM，`libcpp.so.4` |
| libcpp 所在位置 | `armle-v7/lib/libcpp.so.4`，不能假设在 `usr/lib` |
| Qt / SQLite | Qt4、Cascades 1.4；SDK SQLite 3.7.14.1 |
| BAR 打包 Java | 本机 BBNDK 自带 JRE 1.7.0.51；可用 BBATTERY_JAVA_HOME 明确指定 |
| 主机验证 Python | >=3.8，在加入 SDK PATH 前解析完整可执行路径 |
| 设备端脚本 | BBmanager 固定 Python 3.2/ksh 流程，UTF-8/LF，唯一 `/var/tmp` 暂存 |

构建脚本发现唯一 SDK host/target，检查 Qt、SQLite、moc、readelf、打包器和运行库。每个外部程序检查退出码，记录阶段与错误码。`build/environment.json` 保存实际路径和编译器摘要。图标来自本项目 `assets/source-icons`，不再依赖 BBFile 的资源目录。

主机 Python 必须在 SDK PATH 修改前定位：SDK 自带旧 Python 无法执行主机审计。PowerShell 子进程不能假设具有 Get-FileHash，构建使用 .NET SHA256。Java 顺序为显式参数、BBATTERY_JAVA_HOME、唯一自带 BBNDK JRE、JAVA_HOME。

构建同时用 SDK `qmlpreview.exe` 加载全部五个页面。Windows SDK 不含设备端 bb.system 插件，仅在 build 下生成提示框桩和假后端。源页面使用原生 Cascades 渲染；设备端提示框、键盘、焦点仍需真机验证。预览的示例容量仅存在于隔离目录，不进入 BAR、正式数据库或真实业务结果。

## 设备入口迁移

`tools/device.py` 只调用发布 CLI 的 submit 和只读查询。`--host` 从已验证设备登记中取得唯一端点和该 PIN 自己的凭据，不读取旧 Q10Deploy，不使用默认固定 IP、不持锁、不启动 worker、不执行 SSH/SCP。

提交前持久保存任务 ID、完整参数和意图。响应不明或进程重启只查原 ID，不自动重发。部署使用不可变 SHA256 包副本；deploymentObservation 绑定原连接、包摘要、原任务和原 PPS。队列 succeeded 表示观察任务完成时，仍须查看业务 outcome。未确认任务不自动 acknowledge/abandon/delete。

旧 `provision`、`boot-hook`、直接传输、私有 GUI 通道和采集器维护已关闭。原字节存于 `docs/history/0.1.0.9/`，不是执行入口。原六个设备测试脚本明确显示 RETIRED，不能作为当前验收。

0.1.0.11 的同应用 QProcess 子进程在 Q10 上被证实随 GUI 卡片关闭而退出，不能作为后台方案。后续修订复用同一 batteryd，注册 BB10 `application.headless` 入口，通过系统 InvokeManager 定向启动。采集线程仍只有一个、使用现有进程锁，系统事件循环独立于采集线程。仅声明应用后台权限；无系统 STARTED/安装/开机过滤器、无新固件 API、无修改 btool 或额外监护进程。实际支持和关闭后持续采集必须以本轮真机结果判断。

就绪依据仍为持有锁的真实实例、PID/euid/run_id 一致和新鲜业务心跳。调用系统启动成功不能代替就绪。GUI 启动仅自动请求一次；手动重连沿用同样守卫。已有测试在采集器重新启动时标为中断，不重新拼接或继续过去的截止时间。

## 统一容量摘要

Store 在后台生成摘要：value、unit、sourceType、sourceId、estimatedAt、available、unavailableReason，并保留独立 charge/discharge 摘要。QML 只展示格式化后的内容。

顺序是同 battery_key 最近的合格放电测试，随后最近合格充电测试，再考虑有明确归属的系统满充参考。最近按估计自身 end_ms 排序，不能使用刷新时间。放电优先，即使充电记录时间更晚；二者不混合平均。

测试资格必须同时满足：完成、结束原因为 timer/manual、稳定方向和 SOC、起止 SOC 均有效且变化至少 30 个百分点、有效电流积分覆盖至少 95%、至少两个样本、时间/积分/外推容量通过现有数值约束。区间 mAh 乘 100/SOC 变化才是整电池外推容量；原区间 mAh、mWh 和当前 SOC 分别保留。

系统参考的 gate 要求同一当前实体电池、确认归属、真实就绪、明确 BPS 满充来源、记录 ID/时间和有效容量。当前采集读数未提供足够实体归属证据，所以本轮不启用此来源。标称设计容量不作为实际容量；未装入电池不借用当前读数。无可靠来源显示“暂无可靠估计”。新的短测试、失败、中断或 NULL/零容量不会覆盖旧合格结果。合法 0% SOC 正常显示。

容量查询只读取测试聚合表，不扫描原始 samples。按 battery_key/mode/status/end_ms/id 建普通索引，查询 LIMIT 1；历史记录分页，读数 LIMIT 1。SQLite 3.7.14.1 不支持 partial index，故 3.8 以上才创建可选优化索引。

## 数据保留与恢复

本轮对已有数据的改动仅为增加容量摘要索引，不修改旧行或迁移版本。battery_key 保持稳定，重命名只更新名称；查看其他电池不改变 active key。旧监测 sessions 不进入新定时测试的估计来源。

迁移前保留原 BAR、settings、导出及一致 SQLite 副本。在正在写入的库上不能将普通文件拷贝称作一致备份；本轮设备文件下载经过对象身份、哈希和前后稳定性核验，并在主机用 integrity_check 验证。原生既有 sqlite3_backup 路径可制作一致副本，但现行主机协议没有通用 BBattery 私有 GUI 入口，不绕过边界调用。

主机隔离测试用 SQLite backup 保留迁移前副本，重复执行增量索引、对照全部原行，并验证完整性和恢复。恢复应先确认没有进行中的测试和未知操作，关闭应用/采集服务，由受支持入口恢复所保留的数据库和设置；现行协议写入范围不含应用沙盒，所以不能通过 deviceFiles 冒充沙盒恢复 API。旧库无需删除新索引即可供旧代码读取。不要删除目录或卸载应用来恢复数据。

实机 192.168.1.61 在本轮升级前没有样本、会话或测试历史，因此本机没有证明大量旧历史库迁移；该项主机隔离测试通过，实机范围必须单独记录。
