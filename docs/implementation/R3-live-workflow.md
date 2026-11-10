# R3.3c 受控模型工作流

日期：2026-10-26（America/Los_Angeles）。设计决定见
[ADR-085](../adr/085-controlled-live-workflow.md)，前置为
[原 Store live 契约](R3-original-store-live.md)。本模块提供可受控调用的工作流库和宿主入口；
真实供应商、能力/计费和质量对照继续单独验收。

## 1. 接口和顺序

`OfflineRuntime::capture_call` 返回原 `CapturedModelCall`，包括编译上下文、原身份、
捕获的状态版本和单个工作流目标。此阶段不创建 attempt。
`CapturedCallLimits` 只选择宿主费用预留和调用 timeout；不改模型、端点、工具或输出能力。

`observe_live_attempt` 消费原 capture、原 actor、显式宿主授权和临时凭据：

1. 再读原冻结配置并编码原请求。
2. 原 `Store::reserve_attempt` 同事务核全部版本、权限、五维账目和原预算。
3. 领取唯一发送权；精确重放不返回可再次执行的响应。
4. 在数据库事务之外运行原 HTTPS 组件。
5. 保存私有原观察后返回本次响应，不结算，也不派发工具。

`accept_live_response` 再核原 envelope、workflow target 和捕获版本，首次结算原响应。
随后可运行受信核对 hook，再执行已有响应应用分支。Dispatcher 的独立计划/AMEND、
TeamLead 说明/问题/候选检查、Worker 文件工具/提交和经验报告都沿用原捕获守卫。
旧 `execute_live_attempt` 仍是 observe 后立即结算的完整库入口；它不会自行应用业务动作。

## 2. 宿主配置

`EventRuntime::pump_live(request_id, LiveWorkflowOptions, stop_token)` 使用以下显式参数：

| 字段 | 契约 |
| --- | --- |
| `limits.max_calls` | 1～64，每个 batch 的总调用数上限 |
| `limits.max_worker_calls_per_task` | 1～16，每任务 Worker 调用上限 |
| `limits.amendment_dispositions` | 必须为空对象；live AMEND 决定来自原模型调用 |
| `authorization.network_enabled` | 必须显式为 true |
| `authorization.provider_enabled` | provider 类型另需 true；local_tls 仍限制 loopback |
| `reserved_cost_micro_usd` | 每 attempt 的正数费用预留，始终受原请求总预算限制 |
| `max_batch_duration` | 1 毫秒～1 小时的单 batch 墙钟限制 |
| `credentials` | 受信函数，只接收冻结 `api_key_env` 引用，返回临时值 |
| `usage_evidence` | 可选受信用量核对函数，返回 `VerifiedUsageEvidence` 或无证据 |

`validate_live_workflow_config` 只检查配置和宿主参数，不开网络、不读凭据、不修改 Store。
CLI 在取得写者前还校验授权资料与原请求/配置/profile 的绑定；配置有效本身不授予网络权限。

每次调用先捕获当前事实，核原 event claim 和协议，再核账目与剩余原预算，然后解析凭据。
取得凭据后重核 batch 截止，再把剩余时间与 profile 上限较小者写入尚未预约的原请求 envelope。
原 Store 在预约及首次发送时继续复核，防止预检与实际执行之间的状态变化。

deadline watcher 随 `pump_live` 的作用域退出；stop token 进入原 TLS 轮询。
已开始的原文件操作仍由其已有有界执行/证据规则结算。外部受信 callback 不能由库强行中断，
宿主必须自行限制它们；callback 返回后过期 batch 不会再预约或发起新的网络调用。

## 3. 费用核对和停止语义

协议原 usage 仍可为 null，费用不由 token 价格表推算为实际值。
核对 hook 在首次原结算后收到原 request、response 和 settlement；其证据必须另经宿主 verifier
认证和解析，再由原 `reconcile_usage` 检查 attempt/settlement、来源、CAS、幂等及预算差额。
工作流先要求 hook 的 attempt ID 和结算 hash 精确等于本次原结算；同请求另一 attempt
的有效证据也不能误用于此次 callback。
本地测试可注册只接受 local_tls 的独立夹具核验器，生产 provider 核验实现由宿主单独提供。

hook 位于应用之前，以满足原验收和收尾的完整账目门禁，但不会放宽普通动作原有守卫。
没有 verifier 时，原本可采用的计划/说明/文件动作可以依原规则完成，随后 batch 显式停止。
原本要求完整账目的验收等动作仍会拒绝，原错误反馈持久化；后来核对不会再次应用这个响应。

`Store::live_accounting_status` 有界查询五维未知、无原结算、争议、超预留及总预算状态。
任一阻塞都会在下一凭据读取和模型预约前退出，避免 0 预留掩盖 cache-only 未知。
取消、deadline、真实错误、没有新发送权和不完整操作都会停止 batch；不存在隐式付费重试。
原 pending events、AMEND 未决节点和未处理管理目标保持可恢复。

## 4. 输出与恢复

报告模式为 `event_live`。`calls`、`blocked`、`pending`、任务、问题及提交记录提供安全标识、
状态、版本和原记录 hash；原 provider 响应和原文件正文只在私有 Store 中保留。
`stop_reason` 标明 `quiescent`、`call_limit`、`cancelled`、`deadline`、`needs_accounting` 或 `error`。
`quiescent` 只表示本 batch 没有更多可执行工作，不能解释为请求已经交付。

`network_started_calls`/`provider_calls` 统计有观察依据的网络开始；它们不是供应商接收或计费回执。
`network_count_complete=false`、`unobserved_send_claims` 或调用级 `network_started=null` 明确保留
丢失观察，不能从已观察计数为零推出没有请求。报告会另列原账目、batch 限制和实际宿主耗时。

共享路径不改变旧响应的反馈重放规则：

- 已结算且有相同原反馈：只返回原反馈，不调用 hook、不重做工具。
- 已结算但没有原反馈：保持 NotReady，不能重新领取应用权限。
- 观察后尚未结算的崩溃：按原 writer 接管与 UNKNOWN 规则处理，不重发旧 attempt。
- 应用中断：保留原 operation/管理命令与反馈缺失事实，不能把取消当作效果回滚。

## 5. 验收范围

有意义的回归包括：关闭或错误授权时零凭据读取/零发送；旧 Fake 兼容；原上下文和目标贯通；
首次计划/说明/原生工具往返；显式真实观察与独立用量核对；未知/争议/超预留退出；
原 AMEND 捕获、拒绝/返工/耗尽和经验目标；错误停止整 batch；取消和墙钟限制；
重复发送/已结算无反馈拒绝；公开结果无凭据和原正文。实际执行项与失败修正单独记录。

真实供应商调用、实际单 Agent 品质对照、强沙箱部署正向、并发服务及私有行 purge
不由该库接口或 loopback 成功推出完成。

## 6. CLI 授权文件

`live-status --db FILE --request ID` 以只读模式返回原冻结身份、五维账目、原预算及
`authorization_template`。该模板的网络/供应商开关均为 false、费用预留为0；读取状态
不解析凭据、不接管请求、不授予执行权。对Fake profile请求不声称已有live配置。

`pump-live --db FILE --request ID --authorization-file FILE` 接受严格v1授权对象：

| 字段 | 要求 |
| --- | --- |
| `schema_version` / `kind` | `1` / `wls-live-workflow-authorization-v1` |
| `request_id` | 精确等于当前请求ID |
| `config_snapshot_sha256` | 精确等于原冻结配置hash |
| `execution_kind` | `local_tls` 或 `provider`，精确匹配所有已选角色profile |
| `network_enabled` / `provider_enabled` | 布尔值；网络必须true，provider另需true；local_tls不得授权provider |
| `models` | 去重后的全部已选model_ref到完整原profile规范JSON hash的映射；不得遗漏或额外增加 |
| `max_calls` | 1～64 |
| `max_worker_calls_per_task` | 1～16 |
| `max_batch_duration_ms` | 1～3600000 |
| `reserved_cost_micro_usd` | 正整数，不得超过原请求费用限额；实际预约前还核剩余额度 |

不接受缺失/多余字段、凭据值、临时profile、当前配置覆盖或宿主AMEND处置文件。
授权文件有界读取为一次捕获的字节，公开 `host_authorization_sha256` 也来自这些原字节，
执行期间文件变更不会改变本次授权身份。普通文件类型检查先于读打开，FIFO/特殊文件/
软链接拒绝。0.11.0起在读取内容之前，文件须通过整个Store已绑定工作区检查，
也须位于本次显式工作区之外；同库兄弟请求的业务路径不能提供宿主信任输入。

全部CLI参数、授权格式、原冻结身份及sandbox配置在写者接管前预检；取得原独占写者后
只恢复所选请求，再核绑定。预检不把现有未知账目当成禁止恢复的理由：恢复后的pump会
在凭据解析和新预约前返回 `needs_accounting`。原请求和兄弟请求的权限边界保持。

凭据由原profile的 `api_key_env` 引用解析，只接受1～8192字节的可打印非空白ASCII。
缺失/格式错误均用固定有界消息报告，值不进入公开输出。CLI没有 `usage_evidence` 导入选项；
首次费用未知之后的继续执行需要另行受信核验，无法用重复命令或任意费用JSON绕过。

0.11.0按[签名计费契约](R3-signed-billing.md)增加明确的受信中转适配：
`--billing-policy-file` 配合严格v2授权（原字段加 `billing_policy_sha256`）绑定政策原字节，
核验原TLS收据后追加账目。`reconcile-billing` 只导入通过签名及原调用身份验证的迟到收据，
精确重放无需写者接管。v1继续适用于没有该适配的原调用；未知费用仍保持停止。
`pump-single-live` 使用同一原工作流与计费门禁，另要求
[单Agent冻结契约](R3-single-agent-baseline.md)和真实完整跨角色输入。

`--workspaces-file` 沿原节点工作区映射与原 `WorkerTools` 守卫；只有显式工作区时才接受
`--sandbox-manifest`。命令不自动发布或签收交付；这些仍使用原独立命令和精确回执。

## 冻结价格与估算门禁（G1-1）

profile 可携带冻结 `pricing`（见 [原 Store live 契约](R3-original-store-live.md)）。
带价表的请求把 `needs_accounting` 判定改为预算账目完成：已结算、无争议、全部适用
token 已知且有效费用已知（估算或已确认实际）。估算完成的 attempt 释放 settlement
控制预留并允许后续调用；被 N/A 冻结的 cache 维度不计入未知，`not_applicable_dimensions`
单独列出。EventRuntime 在捕获后、encode/凭据前复核最低费用预留（输入侧按适用 rate
最大值 + output rate 向上取整），不足即 Quota，无新预约/claim/发送；Store reserve 与
claim 首发前重复同一复核。无 pricing 的请求保持原五维 UNKNOWN 停止语义逐字不变。

`live-status`/`pump` 的 `accounting` 对价表请求给出 estimated/reported 费用分项、
仍未知预留和 blocker 原因（含 `policy_mismatch`）；`budget` 追加
`accounting_format=wls-estimated-budget-v1` 与 cost breakdown。估算金额永远标注
basis，不冒充 actual；不含 `billing_challenge` 的 profile 才能用估算完成账目，
带 challenge 的签名路径见 [签名计费契约](R3-signed-billing.md)。
## 0.11.1-r3.3c 有界纠错（G1-3）

`OfflineRuntime` 共享 accept 与两类 `EventRuntime` 泵（event-only 404 与 workflow/live）
现在把四类明确的普通Worker失败转换为有界反馈并在同一次显式调用内续行：
完成且非空text但0个工具调用（`model_schema/missing_tool_call`）、
本次已声明且已注册Worker工具的纯参数Schema校验失败（`model_schema/invalid_arguments`，
无任何dispatch/operation）、fs_read在全部权限/路径/祖先检查后仅缺最终叶节点
（`tool_result/not_found`，私有 `MissingWorkspaceLeaf` 子类型对外仍是InvalidInput）、
以及仅由workspace_posix写入前creation/CAS/patch-content确定拒绝点标记的
`failure_kind=content_precondition`（须同时满足operation FAILED、observed_outcome FAILED、
rename_attempted/renamed/temporary_remaining=false且temporary_cleanup_complete=true）。

反馈为固定格式 `wls-correction-feedback-v1`（status=FAILED、correction_eligible=true、
observation_complete=true、effect_status=NONE、effect_count=0、固定有界message）；
已结算工具失败的原operation/execution_result全保留并另加 `correction_feedback` 子对象。
分类只在受信wrapper推导的资格内生效：durable-feedback Store、Worker角色、
无共享单Agent transcript、且为Fake预约或原live尝试的冻结 `execution_kind=local_tls`
（provider类型不获本阶段续行）。分类前仍先完成原settle、after_settlement hook与
账目核验；不合法envelope不强造settlement、未知预留不释放。

续行受冻结 `system.context.schema_retry_max`（新 `FrozenRequestConfig::schema_retry_max()`
访问器；该设置离开runtime_support的无条件validated_only列表，改由
`same_call_schema_retry` 明确列出fake_worker/local_tls_worker=true、provider/legacy=false）
与既有 per-task Worker/call/预算/容量/claim/deadline 门共同限制；每次纠正消耗一个
admission并占原名额，成功之间不重置。workflow/live 入口的续行保持在原Worker claim内
（不重新claim、不提前finish）；耗尽或守卫停止时claim以completed_ids={}释放，
最后失败反馈保留、pending可见。显式pump返回即关闭链；无自动repump、无跨泵持久计数表。

STOP 语义不变：2+工具调用、未声明/未知工具、空Completed、refusal、传输/协议错误、
UNKNOWN/CANCELLED、提交期竞态Conflict、父目录缺失、越界/symlink/权限/Quota、
Store/feedback/invariant失败、管理角色业务语义错误、text+单个合法工具的既有单次执行。
wire侧仅冻结 `tool_calling=true且parallel_tool_calls=false` 的profile增加
`tool_choice:"required"`（logical hash不变）；true-profile原样。

实测与NOT_RUN项见 [docs/reports/g1-correction-validation.md](../reports/g1-correction-validation.md)。
