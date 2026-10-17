# Tokenmeter firmware

仅支持 Waveshare **ESP32-S3-Touch-AMOLED-2.16**：480×480 CO5300 QSPI、CST9220、AXP2101。独立 Arduino 工程，无旧 App/HAL/HID/音频/splash、持久化或服务安装；仅启用 QMI8658 加速度计做四向自动方向。

共享契约：`../docs/protocol.md`；视觉依据：`../design-previews/2026-10-16-pi-meter-mixed/{home,models-1,models-2}.svg/.png` 与 README，按用户后续确认的 A 布局 + D 品牌色实现动态 LVGL 标签，不传截图。

## 手动构建 / host 验证

在本目录执行（不上传）：

```sh
pio run
sh checks/run.sh
python3 checks/minute.py
python3 checks/rotation.py
sh checks/run.sh /tmp/tokenmeter-live.json
```

首次 `pio run` 会下载 PlatformIO 托管依赖；不运行 upload、monitor、服务安装或 git 命令。host 自检使用系统 C++17 编译器，复用 `.pio/libdeps/waveshare_amoled_216/ArduinoJson/src`，启用 ASan/UBSan。额外 argv 是用于开发交叉验证的 JSON 文件，不是设备功能。编译产物：`.pio/build/waveshare_amoled_216/firmware.{elf,bin}`；host exe：`.pio/host/check`。

依赖固定在 `platformio.ini`：Arduino_GFX 1.6.9、SensorLib 0.2.6、XPowersLib 0.2.7、LVGL 9.2.2、NimBLE 2.3.9、ArduinoJson 7.0.4。1.6.4 的 SPI API 与当前 Arduino ESP32 3.3.8 不兼容，因此用 1.6.9；较新 ArduinoJson 的短十进制输入可能走 float32 路径，实际快照对账发现精度损失，因此固定仍属 v7 的 7.0.4，并保留金额精度断言。未修改依赖源码。

## 文件与行为

- `src/core.{h,cpp}`：主机可编译的有界 LF 分帧、严格类型/日期/时间/费用/计数校验、配对去重、排序、格式化、显示省略及分页。只检查 tokens/records/unpriced 与行合计，不强制 tokens 等于四个分项和。保留完整 UTF-8 身份；拒绝非法 UTF-8、嵌入 NUL、C0/DEL/C1 控制字符。计数上限 2^53−1，求和先防溢出。金额比较误差上限低于一美分。
- `src/ble.{h,cpp}`：Tokenmeter 专用 UUID，RX WRITE、ACK READ/NOTIFY。最大 180-byte write、65536-byte 帧、256 pairs、192-byte 身份。超长丢弃至 LF，断连清空未完输入；接收回调只交接字节。主线程完整校验、原子换快照后才 ACK `ok`；失败 ACK `error`，保留原快照/时间。generation 防止跨连接提交；仅一个未完成帧/连接。芯片地址派生稳定 static-random 身份，在 RAM 中设置并回读自检，避免复用旧 HID 的 factory/public 身份；不新增 NVS/eFuse 配置写入。无加密，无配对流程。
- `src/board.{h,cpp}`：保留参考板 display→PMU→touch 顺序、共享 reset GPIO2、触摸使用实机测得与面板对齐的CST原生坐标（swapXY=false/mirrorXY=(false,false)）；沿用参考板的交换/镜像会额外偏转90°，造成旋转后按钮和手势同时错位、200亮度、8秒硬件关机及偶数flush。QMI8658以低功耗21Hz模式提供加速度，主循环约100ms采样；稳定400ms后切换LVGL四向。平放、45°附近、明显移动/不合理重力大小及触摸时保持原方向。IMU失败只在串口报告并保留固定方向，不中断账单。使用现有LVGL的 `lv_display_rotate_area` / `lv_draw_sw_rotate` 和38400-byte PSRAM条带，保护容量边界；LVGL统一逆变换触摸。GPIO18上拉，30ms消抖，只在按下沿发一次回首页事件，不占BOOT/PWR/RESET。
- `src/main.cpp`：先检查 PSRAM，再分配 current/staging 和 draw 双缓冲，检查大块内存及初始化失败。BLE RX 双缓冲、JSON 文档也指定 PSRAM；不让大型快照临时对象进入 loop 栈。NimBLE 保留内部 RAM，关闭 central/observer，单连接。运行时打印初始化后的内部/PSRAM可用内存。手势在LVGL处理指针坐标后读取，避免重复旋转；手势切页后屏蔽该笔后续原生点击直到松手，不阻塞主循环。实体HOME会清理手势并屏蔽到松手，方向改变也会取消未完手势。
- `src/ui.{h,cpp}`：首页左对齐、珊瑚费用、蓝色输入/cache、金色输出、白色 tokens；详情 provider/ 与 model 分两行，每页两条，全部记录可达。HOME 返回首页；第一页 PREV/末页 NEXT 禁用。首页左滑进入明细，明细右滑回首页；明细上下滑仅翻明细页，第一页下滑保持。从普通/禁用按钮区域起手也可滑动；主轴达到50px且满足方向条件即触发，不等松手，每笔触摸只导航一次。屏内按钮维持原尺寸；点击必须从同一按钮起按且未形成拖动，快速松手也复核最终坐标，滑入HOME等按钮不会误点。文字、字体及隐藏状态只在变化时设置，避免重复整页重绘。金额量宽 96→48→32；详情金额也量宽；tokens 量宽 48→32，避免叠到单位。非 ASCII 身份显示 `?`，过长身份显示中间 `...`，不改状态原值。
- `checks/`：一个 assert 自检入口，含 64KiB 接收边界/恢复、解析/身份/金额/溢出/空日边界及分页/时间/格式化检查，也接受实际daemon JSON；另含四向判向、400ms稳定、平放/斜角/震动/触摸保持、64位时间、GPIO18消抖与不连发、手势分类和第一页边界、按住越阈即触发/每笔一次、按钮区域起滑、快速松手取消点击、正常轻点、斜向拖动不恢复点击及取消后重新起按。
- `fonts/`、`src/fonts/`：原始 TTF、许可、生成脚本及六个生成字体，详见 `fonts/README.md`。

## 时间 / 空数据语义

常规UI定时刷新每60秒，年龄标签按整数分钟显示；新数据、连接变化、STALE或跨日状态变化仍立即刷新，不降低LVGL/触摸/IMU节奏。`checks/minute.py`编译实际调度代码与年龄格式分支，以模拟单调时钟覆盖60秒周期、事件提前刷新、120秒失效、午夜边界及millis回绕，无需等待真实分钟。

`checks/rotation.py`组合板级实际配置、SensorLib实际坐标更新、LVGL实际面积旋转/指针逆变换/get_point及核心手势，覆盖四向边界、按钮中心和实机90°/180°点击样本。旧swap/mirror配置可复现断言失败，原生坐标配置通过；这比只验证LVGL自身正逆变换多覆盖了板级基准。临时USB坐标日志已移除。

新鲜度用 `esp_timer_get_time()` 的 64 位单调微秒计算，不使用会在 49 天回绕的 millis 差计算快照年龄。120 秒没有有效帧即 STALE；当 `elapsed >= day_end - epoch`，标 OLD DAY · STALE 并保留源日期，不伪称 TODAY。断连不清空现有快照、不重置新鲜度。

无首帧：NO DATA YET + 占位符。unavailable：SOURCE UNAVAILABLE + 占位符。ok 空日：$0.00 / 0 tokens。partial 零记录且 cost=null：PARTIAL SOURCE / INCOMPLETE / NO READABLE RECORDS + 占位符，不当作完整零账单。其余 null 费用标 UNPRICED；有已知费用且含未定价记录，费用标 PARTIAL PRICE；partial 来源单独标 PARTIAL SOURCE。

## 参考来源、验证及限制

板级引脚、硬件API和初始化顺序参考 [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) 的 `firmware/src/boards/waveshare_amoled_216/` 及其 flush/tick 设置。没有引入旧 HAL、应用或其他板型，依赖源码由 PlatformIO 从各自上游获取。参考工程根目录未找到 LICENSE，不因此推断或授予其源码再分发许可。字体许可单独保存在 `fonts/LICENSE-DejaVu.txt`；本仓库未为整个项目声明开源许可证。

本机实际验证：交叉编译、ASan/UBSan host自检及实际daemon JSON解析通过。实机启动Tokenmeter ready，增加方向与导航后，QMI8658实际初始化成功并取得重力样本，初始化后空闲内部RAM169056、PSRAM7906804 bytes。MTU185的真实快照收到通知和读取ACK ok，正式daemon临时5秒间隔连续4次成功更新；重启后身份稳定、再次连接推送成功。测试进程已退出，未安装自启动，未修改旧Clawdmeter服务或代码。

后续手势修复版再次刷入同一目标并逐段Hash校验，初始化空闲内部RAM169048、PSRAM7906804 bytes；真实今日快照（MTU185）通知/读取ACK仍均为ok。直接编译实际UI输入/click/刷新helper的临时回放与core ASan/UBSan自检通过，但不是实际触摸时序测量。此次启动出现一次I²C `ESP_ERR_INVALID_STATE` 告警，随后初始化与BLE提交成功；告警来源未定位，板级驱动未改。

已验证板级初始化返回成功、实机启动、真实BLE分片/ACK及数次连接。最新触摸基准校准及左滑进入/右滑返回已在设备上确认正常。仍待验证：触摸中断/I²C故障注入、满负载与长期BLE并发、峰值运行时内存余量、功耗及长期残影。编译RAM和一次启动空闲值都不能代替长期内存检查。PSRAM 缺失或关键初始化失败会停机并写 Serial，不继续用空指针绘制。初版字体仅 ASCII 32–126 与 U+00B7（96px 为数值/标点子集）；缺字用 `?` 显示，不支持在屏幕恢复完整长身份。GATT 不认证，只适合可信近场，不是可信计费来源。
