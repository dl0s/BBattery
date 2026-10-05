# BBattery

BlackBerry Q10 的电池档案与整电池预估容量工具。默认首页是“电池 · 容量总览”，每项直接显示容量、来源和估计时间；充放电数据、定时测试及记录位于二级页面。应用 **0.1.0.12**、固件 **1.1.6-M**、本轮发布管理器/Core/CLI **1.1.6.5** 分别管理版本号。

## 使用

1. 为每块实体电池新增档案。装入电池后打开详情，选择“设为当前”；重新装回时使用原档案。查看其他电池只改变浏览对象，重命名保留 battery_key 和历史。
2. 首页比较整电池容量；“当前使用”和“未装入”用文字标明。无合格来源显示“暂无可靠估计”，不会填入标称容量或示例值。
3. “开始测试”进入二级页面，输入 1–1440 分钟，选择 10/30/60 秒采样。开始时识别充放电方向，正常到时结束；也可“结束并保存”。测试中或操作待确认时禁止切换采集归属。
4. 打开电池详情先看容量依据，展开充放电读数；测试记录每次读取 50 条，可加载更早记录。旧历史读数与当前测试实时读数明确区分，0% 是合法读数，缺失显示 `--`。
5. 测试详情显示 SOC、区间 mAh/mWh、时长、覆盖率、缺口和中断原因，可导出本次 CSV/JSON 到 Documents/BBattery。顶部菜单保留全部原始数据导出和当前页面截图。

整电池预估容量优先使用该 battery_key 的最近合格放电测试，没有时使用合格充电测试并标“充电估计”。测试必须完成、SOC 稳定且变化至少 30 个百分点、有效电流积分覆盖至少 95%，并通过数值检查。区间电量和当前剩余百分比不能直接作为整电池容量。新短测试或中断不会抹掉旧合格值，显示的是估计本身的结束时间。

系统满充参考必须有明确来源与实体归属；当前没有足够归属证据，故不启用系统参考。充电和放电估计独立保留，不混合平均。历史监测 sessions 保留，不能作为新定时测试结果。

待机不采样，首页只做后台有限索引查询。0.1.0.12 通过 BB10 的独立 headless 入口启动原有 batteryd，保留单实例和真实心跳就绪判断。0.1.0.11 的 GUI 子进程方式在关闭卡片实测中失败，已保留失败记录。当前实机验证状态和边界以 [本轮验证记录](docs/verification-0.1.0.12.md) 为准，不继承旧版验收。

## 构建与设备操作

先运行发布的 BBmanager 桌面程序作为共享执行者，并在其设备登记中选定目标。所有主机设备业务只提交到共享 v1 队列，按原 ID 查询。

```powershell
$env:INTROOP_SDK_ROOT = 'C:/bbdevtools'
# intent 必须是本次新构建的文件；重用同一文件只查原任务。
python -B tools/device.py build --intent build/protocol/build-my-revision.json
python -B tools/device.py status <构建ID>
python -B tools/record_operation.py <构建ID>

# --host 从已验证的登记中解析 PIN 与独立连接，不使用旧配置回退。
python -B tools/device.py install --host 192.168.1.61
python -B tools/device.py status <部署ID>
python -B tools/device.py continue-deployment <原部署ID> --intent build/protocol/my-observation.json
python -B tools/device.py status <观察ID>
```

当前流程依据 `C:/Users/dove/Documents/BBmanager/Documentation/标准操作文档.md`，CLI 为 `C:/Users/dove/Documents/BBmanager/dist/Q10Manager/cli/Q10Manager.Cli.exe`。提交返回 ID 只表示入队；部署成功还需原 PPS success/100、实际版本/身份/目录共同确认。原事务观察不能改成重新安装，也不自动清理未知任务。

当前协议没有通用 BBattery 前台启动、私有 GUI 操作或固件采集器维护 API。手动打开应用进行界面与测试操作；主机使用固定 applicationLog、processes、deviceFiles 读取证据。旧 SSH/SCP、with_device_lock、Q10Deploy、boot-hook、btool 修改和维护流程均不作为回退。

保留 C++98、Qt4/Cascades/QML、SQLite 与 BB10 ARM 工具链。SDK、Java、运行库 ABI、打包与迁移恢复说明见 [环境与容量说明](docs/environment-and-capacity.md)。每次构建记录实际环境、检查所有退出码、加载五个 QML 页面并审计 BAR。

## 主机验证

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test-capacity.ps1
python -B tests/device_tools_test.py
python -B tests/collector_release_test.py
python -B tests/package_test.py
python -B tests/qml_load_test.py
python -B tools/audit_bar.py build/BBattery.bar
```

QML 回归会重现并拒绝 0.1.0.10 真机遇到的两个语法错误。SDK 页面预览用 `python -B tools/qml_preview.py --all`；`--scenario long-name/empty/running/pending` 检查隔离界面。bb.system 的 Windows 桩不证明真机提示框正确。

旧六个设备测试入口显示 RETIRED，原字节位于 `docs/history/0.1.0.9/`。它们依赖的直接传输和私有维护已关闭，不能把历史通过数算入本轮。主机、SDK 预览、共享队列实测、用户确认和未验证项目分别记录于验证文档。
