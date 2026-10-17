# 界面示例（离线渲染，非实机照片）

**图片来自当前真实 LVGL 界面代码的主机离线渲染，不是实机截图或照片；全部数据为合成示例，不代表真实用量、账单或模型定价。**

来源：`firmware/src/ui.cpp`、`firmware/src/core.cpp` 和 `firmware/src/fonts/font_{16,20,24,32,48,96}.c`。使用原有 `firmware/include/lv_conf.h`、LVGL 9.2.2，原生编译后调用实际 UI API 和滑动手势，捕获 RGB565 flush 帧缓冲再转换为 PNG，并非另行绘制 UI。

- [home.png](home.png)：今日首页，480 × 480。
- [yesterday.png](yesterday.png)：上一日汇总，480 × 480；同布局、`HOME >`回今日，无昨日模型明细。
- [models-1.png](models-1.png)、[models-2.png](models-2.png)：模型明细两页，各 480 × 480；同名模型跨 provider 不合并。
- [overview.png](overview.png)：三页无缩放横向拼图，1472 × 480，深灰间隙各 16 像素。

今日示例日期为 2026-10-17（Asia/Shanghai），采样时间12:00，日结束为次日00:00；已连接、1m ago、状态正常。今日合计$0.79、3,160,000 tokens、34 records。上一日示例为2026-10-16，$1.24、2,400,000 tokens、27 records，输入180,000、输出20,000、cache read2,200,000。两日issues/unpriced均为0，全部为合成示例。

昨日图由 `python3 firmware/checks/ui.py --render-dir docs/images` 的真实LVGL回归渲染导出；截图在原生淡入完成后捕获，不用于声称实机动画帧率。

未读取设备、USB、BLE 或私人会话数据。PNG 不含文本元数据或本机路径；离线渲染不验证屏幕面板、触摸硬件或实机显示效果。
