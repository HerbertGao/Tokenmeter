# Pi 今日与上一日用量口径

## 来源与隐私

只扫描指定 Pi agent 目录的 `sessions/**/*.jsonl`，默认 `PI_AGENT_DIR` 或 `~/.pi/agent`。多个 `--pi-dir` 合并统计，相同真实文件路径只读一次。不打开 `auth.json`、模型配置、供应商账单 API 或 API keys。

JSONL 同时包含正文和元数据，因此需要在进程内解析日志容器；只保留 ID、时间、provider/model 和 usage 数字。消息正文不进入缓存、stdout、日志、测试产物或 BLE。`--once` 仅输出允许的统计字段，不输出会话 ID 或路径。

## 时间与缓存

- 用同一主机本地日期构造昨日、今日、明日三个午夜；今日区间 `[今日00:00, 明日00:00)`，昨日区间 `[昨日00:00, 今日00:00)`，排除生成快照时仍属未来的记录。上一日按日历减一天，不减86400秒；兼容DST23/25小时及跳日。不是滚动24小时，也不按文件创建日或mtime选一天。
- entry.timestamp 优先，message.timestamp 兜底。ISO 时区与 epoch 毫秒均兼容。
- 每次检查文件 inode、mtime_ns、ctime_ns、大小；仅重读变化文件，缓存只留今日与上一日使用元数据；同一次扫描/去重后分开加总，仅今日生成模型分组，昨日只有摘要。跨日或本地时区窗口变化时重建。不写数据库或索引文件。
- 文件被截断、替换、删除会在下次扫描反映；fork 中复制的历史不能再加一次。完整坏行、读失败或部分目录缺失保守影响两日issues/partial，不凭坏数据推断某日完整或为零；正在写入的不完整末行留待下次处理。

## 哪些用量计入

按当前安装的 Pi `dist/core/usage-totals.js:getUsageCostBreakdown` 核对，计入：

- assistant 的 message.usage。
- toolResult 的可选 message.usage。
- compaction / branch_summary 的顶层 usage。
- 独立 `type: "usage"` 事件。

error / aborted 只要包含有效用量也计入；全零默认 usage 不算一条有消耗的记录。记录数是用量记录数，不是供应商最终 API 请求数。

assistant 有非空 responseModel 时优先于 model，与 Pi 原生明细一致。其他类型只使用记录本身明确给出的 provider/model；缺失进入 `unknown`，不从前一个模型、定价模型或路由名猜测。组合键是元组 `(provider, model)`；不能只按 model 合并。

去重键包含 kind、entry ID、时间、provider、model。Pi fork 会保留复制项的 ID 与时间。副本元数据冲突时保留有价格／更完整的副本并标 partial，不把两个版本累计。没有稳定 ID 的旧记录只能按文件与行保留，不能保证跨复制文件去重；不通过读取正文猜测身份。

## Token 与费用

- 总 token 按 Pi 原生明细：`input + output + cacheRead + cacheWrite`。`reasoning` 已包含在 output，`cacheWrite1h` 已包含在 cacheWrite，不再相加，也不把 totalTokens 再加上去。
- 分项缺失时使用已知分项；有效 totalTokens 可作为下界补充，取两者较大值，同时标 partial，避免把未知分项假称完整。无此问题的原生日志严格使用四项合计，即使它与 totalTokens 不一致。
- 费用使用有限、非负的 usage.cost.total。Qoder 在其缺失或为0时回退有效 usd_equivalent，包括明确的0；非零 cost.total 保持优先。不调用在线价格表，不从 token 猜费用。
- 无有效价格是 null，不是免费。部分缺价时保留已知小计与 unpriced 数量，设备明确标记；所有记录均缺价则金额显示占位。
- 金额按日志 USD，尚不是最终账单。已有的空 sessions 目录是真实无用量日；配置目录缺失或完全不可读是 unavailable，不能伪装成零。

源代码证据：当前安装的 `@earendil-works/pi-coding-agent/dist/core/{usage-totals,session-manager}.js`；Qoder 的 `pi-provider-qoder/dist/index.js:applyQoderCredits`。这些是核对时版本，不是未来版本的兼容承诺。

## 与 ccusage 的差别

本机未安装 ccusage。研究时上游 Pi adapter 主要计 assistant，并不覆盖这里列出的全部 usage 类型，也不保证识别 Qoder 等值金额。故不同总额不自动说明其中一方算错，应先比较共同记录、日期、fork去重与费用来源。本工程先用合成边界测试及已安装 Pi 原生汇总函数作可复现核对，不声称已通过 ccusage 全账本对账。

昨日与今日使用同一 token/费用口径。昨日累计超设备数值边界时，只有昨日摘要降为 unavailable；今日自身超分组/身份/整包边界时，保留仍能安全编码的昨日摘要。fallback沿用原生成epoch，避免跨午夜拼出不同日期的两日快照。不增加多日查询参数、持久索引或历史选择器。

BLE 容量、整包确认、未知状态和屏幕规则见 [protocol.md](protocol.md)。
