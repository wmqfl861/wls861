# R1.4：冻结管理历史窗口与原证据回读

实现版本 `0.9.0-r3.3b`，目标蓝图 v0.4.1，依据
[ADR-083](../adr/083-management-history-window.md)。本节点接入 Dispatcher/TeamLead 的实际
OfflineRuntime 和 EventRuntime；原业务目标捕获、管理 CAS、预算和反馈路径继续使用。

## 配置选择

新请求可在现有系统配置中显式加入：

```yaml
context:
  request_format: structured-v2
  management_history:
    mode: window-v1
    recent_attempts: 2
    inline_bytes: 32768
    max_context_bytes: 131072
```

这是 `system.context` 中相应字段的示例；原 `context` 其他必填字段保留。`recent_attempts` 为
1..4，`inline_bytes` 为 32768..65536，乘积不超过 131072；`max_context_bytes` 为
131072..1048576。类型、额外键、模式或 framed-v1 组合在配置验证时拒绝。

缺失 `management_history` 不隐式启用策略，也不改变旧请求完整历史的容量失败。磁盘 YAML 更新
只影响以后准入的请求。策略进入冻结快照、编译依赖哈希和当前任务契约；Store 预约再次核对冻结策略
及两个控制声明。`check`/`explain` 的 `runtime_support.management_window_integrated` 与
`management_history_policy` 披露实际选择。

## 当前视图和历史目录

`Store::management_view(actor, current_binding)` 返回 `wls-management-view-v1`：

| 字段 | 内容与边界 |
| --- | --- |
| `scope`、`current_binding` | 宿主确认的管理角色、请求、主体、任务/团队及当前语义/代次 |
| `questions` | 当前作用域全部 OPEN 问题，包含原内容、当前路由/版本与完整原绑定 |
| `unresolved_operations` | PREPARED、RUNNING、CANCEL_REQUESTED、UNKNOWN 的完整持久事实 |
| `unresolved_attempts` | PENDING/UNKNOWN、用量仍未知或缺失反馈的尝试；live 另附独立五维用量投影 |
| `recent_attempts` | 当前管理角色/主体的最后 N 个完整原响应及反馈，保留其原绑定 |
| `history_catalogs` | question/submission/attempt 的高水位、成员哈希、总量、已完成量和省略量 |

OPEN 问题、未决操作和未决尝试每类超过 64 项会在创建新预约前明确拒绝，不保留前 64 项后继续执行。
Store 410 的 live 判断使用 `live_usage_projection` 的 input/output/cache_read/cache_write/cost 五个独立
可空字段及争议状态；原 `call_attempts.usage` 和原响应保持不变。已知超预留也在管理视图保持可见。
已完整核对且无争议/超预留的原中断尝试可退出未决集合，但原 UNKNOWN 状态仍保持其独立围栏语义。
原 Worker `attempt_history_tail` 对 Store 410 live 使用同一投影判定和事实附件，避免追加对账后仍永久
受原 null 标志误导；旧 schema 与 fake 调用保持原三项 unknown 的解释。

目录以同一次 SQLite 读取快照取得最大 rowid、成员范围和当前事实，近期查询受该高水位约束。
`catalog_sha256` 对作用域、kind、高水位和每项不可变原始指纹构建哈希链；`completed_count` 只是这次
观察到的状态计数。`snapshot_kind=insertion_membership_only` 不保证以后问题仍处于同一状态。
后续取原记录需要独立校验 `record_version` 和 `record_sha256`。

近期轮次按编译后的整轮消息大小选择：可内联则保留完整 assistant/tool 配对，超限则使用明确原记录引用。
未决事实同时在完整当前契约中保留，不会因近期轮次被引用而消失。不会递归加载以前的整个请求上下文。
历史目录扫描仅使用元数据，原记录每条仍受 8 MiB 上限约束；这是有界读入，不是 SQLite 大对象底层流式读取。

## 完整当前契约

管理输入继续包含 request_snapshot 的原始文本、全部 AMEND、有效约束、当前 task/instruction/binding、
预算、待办和等待。Dispatcher 另含当前任务列表和计划；一次工作流动作仍捕获其完整问题、候选、拒绝、
AMEND 目标或经验原源。提交检查和验收继续带原不可变文件字节，引用不替代本次必须检查的内容。

管理窗口不改原决策入口，也不从最新行刷新已捕获的目标。管理响应等待期间 AMEND、目标 state_version
或执行代次改变，原响应照常结算，最终业务变更仍被原 Store 门禁拒绝并保存错误反馈。

`compile_management_context` 先计算当前契约各字段大小及完整结构化输入。超过冻结总上限时返回 Quota，
诊断含 `code=management_context_capacity`、`section_bytes`、`history_message_bytes`、
`required_context_bytes`、`stable_prefix_bytes`、历史目录和 `reservation_created=false`。
若连临时测量上限也无法容纳，报告 `measurement_complete=false` 而不虚报完整总量。当前契约不会被剪裁。
诊断建议处理具体未决事实或显式拆分/重规划；该节点不自动摘要要求、调整预算或循环重试同一个超限输入。

## 模型控制接口

| 控制 | 参数 |
| --- | --- |
| `management_history_page` | `kind` 为 question/submission/attempt；`after_sequence`、`through_sequence` 非负；`limit` 为 1..4 |
| `management_history_read` | `kind`、`record_id`、64 位小写 `record_sha256`、正整数 `record_version`、非负 `offset`、1..4096 的 `max_bytes` |

参数集合封闭。身份和绑定来自宿主捕获，不从模型输出取得。Dispatcher 的问题/提交只能读本请求；
TeamLead 只能读本任务/团队。attempt 必须属于当前管理角色、主体及作用域。非零分页游标必须确实属于该
作用域，不能借兄弟请求 rowid 定位；旧读取引用跨管理角色使用也会因作用域哈希不同而失败。

原记录读回返回 `base64-canonical-json-utf8`、整个 section SHA、偏移、实际字节数和下一偏移。拼接全部
片段后先验证 SHA，再解析 JSON；片边界可位于 UTF-8 字符中间。读回的 question/submission 包含原记录及
其当前捕获版本；attempt 返回已持久的不可变反馈及原响应。缺失反馈返回 NotReady，不能补造旧记录。

读取前后复核当前角色/绑定和禁用政策。SQLite 只读快照结束后再进行一次当前政策检查，避免读取期间的
撤销被旧快照隐藏。任何读取都不授予业务权限。正常管理窗口要求两项回读控制都可用；已捕获调用在执行前
失去控制能力时拒绝。终态经验报告继续只声明 `report_experience`。

每次模型选择控制仍耗用原模型 attempt/用量账本，结果写入原不可变反馈。精确响应重放返回相同反馈，
不会重新查询或追加预算。最近 60 秒的已结算查询包含失败，按请求/角色/主体/任务/团队从持久账本计数并
遵守冻结的 `system.control.max_queries_per_minute`；重开不能重置。时间窗口依赖可信宿主 UTC 时钟。

EventRuntime 读历史后只完成确实处理过的事件；未回答的问题仍 OPEN、待办仍保留。跨重启继续时沿当前
绑定重新捕获目标，不能用回读结果直接消费问题、通过候选或更新计划。

## 验证和保留边界

`r1_management_tests` 覆盖 130 次实际 Fake 管理问答和中途重开、窗口外原字节分片回读、旧完整历史触顶、
scope/hash/version/epoch、原反馈缺失、OPEN/UNKNOWN 固定保留、预约前零新增、政策撤销及读取中变化、
失败查询频率、AMEND/CAS、事件待办和完整候选文件，以及管理角色和 Worker live 用量投影的联合回归。
实际结果和执行输入见 [节点报告](../reports/r1-management-window-validation.md)；最终 SHA 的全部
门禁以 [五节点报告](../reports/five-node-validation-20261027.md) 为准。

本节点没有启用真实供应商调用。Fake 能验证引用、权限和状态行为，不能证明真实模型会正确选择回读、
理解长历史或提高质量。LLM 语义压缩、旧活动请求迁移、无限历史存储/查询、远程鉴权和并发租约仍未实现。
