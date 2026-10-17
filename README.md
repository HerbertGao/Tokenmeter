# Tokenmeter

用 Waveshare **ESP32-S3-Touch-AMOLED-2.16（480×480）**展示 Pi 的今日／上一日 token 用量和日志费用，通过本地 daemon 经 BLE 更新。

支持两日汇总、今日 `provider/model` 明细、四向自动旋转与触摸翻页。不需要 Claude 订阅、用量 API、Wi-Fi、数据库或桌面 App。

## 界面预览

![Tokenmeter 界面预览：上排昨日与今日汇总，下排今日模型明细](docs/images/overview.png)

四个页面统一展示：上排为昨日、今日汇总，下排为今日模型明细的两页。图片由真实 LVGL 界面代码配合合成数据离线渲染，**不是实机照片，也不代表真实用量、账单或模型定价**。原尺寸图片和渲染说明见 [docs/images](docs/images/README.md)。

## 快速开始（macOS）

需要 Git 和 Python 3.10+，推荐 Python 3.14。目前一键脚本和实机连接均在 macOS 验证。

### 1. 获取代码

```sh
git clone https://github.com/HerbertGao/Tokenmeter.git
cd Tokenmeter
```

两个脚本会在项目 `.venv` 中准备所需 Python 包，并复用已有环境。首次运行需要联网；固件构建还会使用 PlatformIO 用户缓存。脚本不会安装系统 Python、使用 sudo、安装开机服务或修改系统蓝牙配置；发现损坏的 `.venv` 会报错，不自动删除重建。

### 2. 按需刷入固件

**已有 Tokenmeter 固件时，换电脑连接不需要重刷，可直接跳到下一步。** 首次使用或需要更新固件时，确认板型是 **ESP32-S3-Touch-AMOLED-2.16，16MB Flash / 8MB PSRAM**，使用支持数据传输的 USB 线连接：

```sh
./flash.sh
# 也可指定设备列表中的准确串口
./flash.sh /dev/cu.usbmodemXXXX
```

脚本在项目虚拟环境安装 PlatformIO 6.2.0。单个 ESP32 原生 USB 候选会显示端口，多台候选必须选择，输入 `FLASH` 后才构建并上传。**USB 标识不能证明板型；刷机会覆盖现有固件，脚本不备份、不整片擦除，也不提供跳过确认的参数。** 请先核对实物、端口和备份需要，不要向其他板型上传。

### 3. 启动本地 daemon

新电脑开启蓝牙，然后运行：

```sh
./start-daemon.sh
```

脚本自动安装 BLE 依赖，前台运行并默认每 60 秒推送，按 `Ctrl+C` 停止。macOS 首次询问时允许终端／Python 使用蓝牙；通常不需要在系统设置中手动配对。

设备只允许一个连接，**先停止旧电脑或旧终端的 daemon**，避免多个进程争抢。首次启动会自动寻找 Tokenmeter；多台设备可用 `--address` 指定，macOS 外设 UUID 不应直接从另一台 Mac 照搬。

默认从 `PI_AGENT_DIR/sessions` 读取；未设置时使用 `~/.pi/agent/sessions`。可先只检查统计，或指定其他目录：

```sh
./start-daemon.sh --once
./start-daemon.sh --pi-dir /path/to/pi-agent
./start-daemon.sh --pi-dir /path/to/agent-one --pi-dir /path/to/agent-two
./start-daemon.sh --help
```

`--once` 只用 Python 标准库，不安装 BLE 依赖、不连接设备。`--pi-dir` 指**包含 `sessions/` 的 agent 根目录**，不是会话文件或 `auth.json`；多目录会按原有去重规则合并。设备显示的是运行 daemon 的电脑所能读取的数据，不自动同步其他电脑的日志。

若脚本没有执行权限，可用 `bash ./flash.sh` 或 `bash ./start-daemon.sh`。需要手动安装时，使用项目虚拟环境安装 `daemon/requirements.txt`，再运行 `.venv/bin/python -m daemon.tokenmeter`；手动构建见[固件说明](firmware/README.md)。

## 更新与换机

在旧 daemon 终端按 `Ctrl+C` 停止，再更新并重新启动：

```sh
git pull
./start-daemon.sh
```

已运行的 Python 进程不会自动加载新代码。涉及固件功能的更新还需运行 `./flash.sh`，并让 daemon 与固件配套升级；仅换电脑无需重刷。旧 daemon 不发送上一日摘要时，新固件会显示 `NO DATA`，不会伪造为零；更新后重启 daemon，再等待首次推送。

## 屏幕操作

| 当前页面 | 操作 | 结果 |
| --- | --- | --- |
| 今日汇总 | 右滑 | 上一日汇总 |
| 上一日汇总 | 左滑或点 `HOME >` | 今日汇总 |
| 今日汇总 | 左滑或点 `MODELS >` | 今日模型明细 |
| 模型明细 | 右滑或点 `< HOME` | 今日汇总 |
| 模型明细 | 上／下滑或点 `NEXT`／`PREV` | 下一／上一页；第一页下滑不离开明细 |
| 任意页面 | 按 GPIO18 自定义实体 HOME 键 | 今日汇总 |

汇总页上下滑不翻页，上一日页继续右滑没有更早日期，也不提供昨日模型明细。实体 HOME 不是 BOOT、PWR 或 RESET，不做双击／长按模式。

手势和按钮都相对当前画面方向。沿有效方向达到 50px 即响应，不等松手；同一笔触摸最多切一次，滑动不会误点按钮，也可从灰色按钮区域起手。

方向稳定 400ms 后自动切换四个正交朝向；平放、接近 45°、明显晃动或触摸期间保持原方向。用户切页采用约 120ms 内容淡入，分钟数据更新不播放动画。

## 数据、隐私与边界

- **按本地自然日统计**：仅今日与上一日，不是滚动 24 小时，也没有多日历史选择。两日独立汇总，只有今日提供模型明细；同名模型跨 provider 不合并。
- **按日志记费用**：USD 是日志记录或折算值，不是供应商最终账单；缺价不按免费处理，也不从 token 猜价格。空日志目录可表示零用量，源缺失、坏记录和缺价会明确标记。
- **只保留用量元数据**：JSONL 容器在本机解析，仅缓存、输出和发送允许的 usage 字段；不保留／发送正文，不读取认证文件。缓存只在内存中保留两日元数据，不建立数据库或持久索引。
- **保持更新时间可见**：常规刷新每 60 秒；新数据、连接变化和失效状态立即更新。120 秒没有有效快照或跨本地午夜时标记 `STALE`／`OLD DAY`，保留源日期而不把旧数据称为今日。重启后等待 daemon 重发，不在设备上持久保存历史。
- **BLE 仅适合可信近场**：当前未认证、未加密，附近客户端可能注入数据，不应作为可信计费终端。
- **有明确容量和字库限制**：整包上限 64KiB、今日 256 个分组、每个身份 192 UTF-8 字节；超限不静默截断。字库主要覆盖 ASCII 和常用标点，非 ASCII 身份在屏幕上以 `?` 替代，原始身份仍保留。

完整规则见[用量口径](docs/usage.md)、[BLE 协议与状态](docs/protocol.md)。

## 开发与检查

Python 与脚本测试不读取真实日志，也不访问蓝牙或硬件：

```sh
python3 -m unittest discover -s daemon/tests -v
python3 -m unittest discover -s scripts/tests -v
```

获取 PlatformIO 依赖后，可运行固件主机检查；原生 LVGL 检查还需要系统 C/C++ 编译器：

```sh
sh firmware/checks/run.sh
python3 firmware/checks/minute.py
python3 firmware/checks/rotation.py
python3 firmware/checks/ui.py
```

覆盖内容包括两日隔离、DST／跨日、去重、缺价、协议边界、输入仲裁、旋转、分钟调度和原生 LVGL 页面／动画。目标板已验证启动、显示／触摸、方向、页面切换及真实 BLE 快照提交，但这些检查不构成长期可靠性认证；满负载并发、峰值／长期内存、I²C 故障注入和功耗尚未完整测量。更多构建、内存及字体细节见[固件说明](firmware/README.md)。

## 参考与许可

板级引脚、硬件 API 和初始化顺序参考 [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) 的同款设备支持，没有引入旧桌面 App、HAL 或其他板型。字体许可见 [LICENSE-DejaVu.txt](firmware/fonts/LICENSE-DejaVu.txt)，PlatformIO 依赖遵循各自上游许可。

本仓库暂未声明项目整体开源许可证。公开源码不改变第三方组件的许可，也不代表对参考工程的源码另行授予再分发许可。
