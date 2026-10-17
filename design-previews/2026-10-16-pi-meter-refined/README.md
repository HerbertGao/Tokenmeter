# Pi Meter：A / D 像素约束版

> 历史设计原型：以下状态与交互说明保留当时记录，不代表当前固件。最新操作规则见[项目 README](../../README.md)，最新界面渲染见[docs/images](../../docs/images/README.md)。

状态：**仅设计样图，等待用户选择**。本轮没有实现统计 daemon、BLE、固件或刷机，也没有提交任何设计选择。

预览：**http://127.0.0.1:8766/**。第一轮仍在 http://127.0.0.1:8765/，未覆盖或停止。

## 本轮范围

根据“点阵屏字体必须可实现，A 和 D 都不错”的反馈，只收敛这两个方向，不把 A+D 混搭当成已批准决定。

- A：费用、token 和明细沿同一左边界排列，黑底、单色层级。
- D：保留错位布局和 Pi 珊瑚／蓝／金色，不再依赖另一套装饰字体。
- 首页：今日费用、总 token、输入、输出、缓存读取，以及示例请求数、更新时间。
- 第二页：Provider / Model 两种统计视角，每行名称、费用、token、示例请求数。
- 网页顶部按钮同步切换两套方案，便于比较；它只演示切页，不代表已经决定实机的触摸或滑动手势。

视觉冒险度 4/10，动效强度 1/10，信息密度 5/10。页面拨盘仅记录后续偏好，不实时重排已栅格化样图。选择按钮通过原预览桥打开确认弹层；必须用户确认回传，不能把测试点击或取消视作选择。

## 文件

| 方向 | 首页 | Provider | Model |
|---|---|---|---|
| A | `a-home.svg` / `a-home.png` | `a-providers.svg` / `a-providers.png` | `a-models.svg` / `a-models.png` |
| D | `d-home.svg` / `d-home.png` | `d-providers.svg` / `d-providers.png` | `d-models.svg` / `d-models.png` |

每张单页均为 480×480。`overview.png` 按 A、D 两行排列，每行依次为首页、Provider、Model。`index.html` 内嵌全部六张 SVG，离线可切换页面；PNG 下载需与 HTML 放在同一目录，离线不支持自动回传选择。

## 字体与像素边界

字体源为旧项目已有的 `firmware/fonts/DejaVuSansMono.ttf`（仓库根目录相对路径）；本目录不复制 TTF，也不复用 Styrene / Tiempos。旧项目的 `font_mono_32.c` 已证明同一字体可转换为 LVGL 4bpp 位图字库，但**这不代表本轮所有字号已经转换、编译或通过实机验证**。

样图使用固定 16、20、24、32、48、96 px 字号，在透明背景上栅格化，再把字形透明度量化为 16 级。SVG 中所有文字使用内嵌 PNG，宽高与 PNG 原始尺寸一致，不做文字位图缩放，也不依赖浏览器字体。单页 PNG 由同一 Ego 浏览器的 SVG → Image.decode → 480×480 Canvas 导出，不是硬件截图。

点阵屏并不要求粗糙的像素风；这里展示的是固定像素、有限灰阶覆盖率的可行排版方向，而不是对 LVGL 渲染结果的保证。未来固件仍应使用动态文本与转换后的字库，不是把这些静态金额截图推送到屏幕。

长名称以固定 20px 字号中间省略、保留两端；只做了名称列边界自检，尚未实现实机完整名称查看。大金额的字号回退、缺字覆盖、中文字库、字库体积、亮度、圆角安全区与长期残影均未验证。屏内先保留拉丁字符，网页说明用中文。

## 示例数据不是实际分布

所有屏幕统一使用 $0.79、3.16M token、34 条记录。日期、18s ago 均为静态示例。

Provider 人工分配：openai-codex = $0.57 / 1.94M / 21；qoder = $0.22 / 1.22M / 13。Model 人工分配：gpt-5.4 与 Qwen3.8-Max 使用同样两份数字。两种分组合计均等于首页，但这**不是从用户日志计算出的真实 Provider / Model 占比**，也不是断言 provider 与模型一一对应。

输入 258.8K、输出 24.2K、缓存读取约 2.88M 为舍入展示；用于自检的整数为 258800 + 24200 + 2877000 = 3160000。USD 暂沿用首轮示例，币种仍待确认；RECORDED 表示日志金额或折算，不是供应商账单。Pi 的用量记录数量不能未经核对就宣称等于供应商最终请求数。

## CC Switch 参考与本项目差异

只读核对版本 `farion1231/cc-switch@e8880e6`：

- [`resolveUsageRange`](https://github.com/farion1231/cc-switch/blob/e8880e6/src/lib/usageRange.ts#L26)：today 为本地零点至现在，非滚动 24 小时。
- [`get_provider_stats`](https://github.com/farion1231/cc-switch/blob/e8880e6/src-tauri/src/services/usage_stats.rs#L1708)：按 provider_id + app_type 分组，累计费用、token、请求数，按费用降序。
- [`get_model_stats`](https://github.com/farion1231/cc-switch/blob/e8880e6/src-tauri/src/services/usage_stats.rs#L1901)：优先非空 pricing_model，否则 model；只按该模型键分组，跨 provider 合并，按费用降序。
- CC Switch 的 Provider 名称可来自它的配置，数据来自请求日志与日汇总；不能把这些数据库／配置前提照搬到独立 Pi 仪表。

Tokenmeter 拟直接使用 Pi 日志的 provider 与 model，不读 CC Switch 数据库，不猜测 pricing_model、路由模型或自定义显示名。同一数据的两种视角不能相加。缺失归属的记录后续必须进入明确的未知组，不能丢弃导致首页与明细不相等；完整统计口径待实现与对账。

当前样图只演示每个视角两条。更多条目的滚动／分页未实现、未确认，不照搬桌面每页 20 条。分组增加后的 BLE 数据量、更新一致性和固件内存也需要在实现前重新检查。

## 已检查

- 六张 SVG 均可解析，viewBox 为 0 0 480 480；六张单页 PNG 签名及尺寸正确。
- 内嵌文字位图按原始像素尺寸放置；字形边界保持 x ≥ 28、y ≥ 20、right ≤ 452、bottom ≤ 454。
- 检查字体样本透明度级数不超过 16，原尺寸价格区域有有效文字像素；已目视检查六图总览。
- 两种示例分组的费用、token、记录数均与首页相等；超长 Provider 名称保持名称列宽。
- Ego 浏览器 1200px 视口：两列、每个屏幕 480×480，无横向溢出。
- 390px 视口：单列、每个屏幕 354×354，无横向溢出；外部切页按钮高度至少 44px。
- Provider / Model 切换同步改变两卡片，激活状态和 PNG 下载链接正确。
- D 的选择弹层显示正确方向与 4/1/5 参数；点击取消后关闭，两个预览目录均没有 selection.json。
- 两个预览 URL 与新版全部 SVG / PNG 可访问；内联脚本通过 `node --check`。主动诊断仅提示“回看第一轮”的 localhost HTTP 链接，保留本机服务原地址，不伪造不存在的 HTTPS。

没有安装依赖、修改旧 daemon、读取 auth.json 或会话正文。未编译 LVGL 字库／固件，未运行 BLE、ccusage 对账或连接硬件。
