# R3.3b — 原 Store 预约、发送、结算与追加核对

实现版本 0.9.0-r3.3b；Store 410；蓝图 v0.4.1。
决定见 [ADR-081](../adr/081-original-live-store-accounting.md)，本地实际验证见
[节点报告](../reports/r3-original-store-validation.md)。本页定义受信宿主库接口，CLI 仍只运行 Fake。

## 配置和授权

`system.models[model_ref]` 可显式注册 `adapter=openai-responses`、`adapter_version=1`、
`protocol=openai-responses-v1`。已有 model/capabilities 必须与新增 `responses` 中的
严格 provider profile 一致；不允许在外层声明 supported 而内层另有含义。

`responses` 要求完整字段：`provider_profile`、`transport`、`stream`、`execution_kind`、
`capability_evidence_source`、`capability_evidence_sha256`。transport 保存明确 CA PEM、
IPv4 路由、头/行/累计响应容量及 1–25 ms poll interval。证书验证仍使用原 URL 主机名。
`local_tls` 仅允许 127.0.0.1，或 localhost 精确映射到 127.0.0.1；其他端点必须为 provider。
能力依据引用/hash 不是运行时供应商证明，真实验收另做。

外层 `system.model_calls.real_calls_enabled` 默认 false；fake-only 注册即使把它设 true
仍拒绝。允许 live 的原快照还必须使用 structured-v2，正数 input/output/cost 预留，以及
原每请求 budget。离线 codec profile 内部继续 `real_calls_enabled=false`、paid retries=0，
它本身不授予网络权。

`execute_live_attempt(Store&, actor, request, bearer, authorization, stop_token)` 的
`authorization` 默认全部 false。network_enabled 是宿主独立开关，provider_enabled
是实际 provider kind 的额外门禁。bearer 仅以内存参数传入，不从环境查找、不保存 outgoing
认证头；任何响应回显的内容只能进入私有原观察。底层 Store 和 transport API 均是受信宿主
接口，不注册成模型工具，也不构成不受这些门禁约束的生产入口。

## 可持久化容量

新 live 注册在准入前核对比独立离线组件更窄的一组上限；不默默修改已冻结 profile。
HTTP/SSE 的实际接收仍取各自限制与累计限制的交集，响应超限保留已观察到的精确前缀及截断标志。

| 项目 | live 上限 |
| --- | --- |
| 原响应头体累计 | 3 MiB |
| 原响应头 / 单行头 | 64 KiB / 8 KiB |
| HTTP JSON body / SSE profile stream | 1 MiB / 4 MiB，仍受3 MiB头体累计限制 |
| 输出文本 / 全部工具 arguments | 各256 KiB |
| SSE line / event | 256 KiB / 1 MiB |
| output items / tool calls | 128 / 32 |
| SSE events / callback chunks | 4096 / 16384 |
| Store private raw hex + decoded response + protocol evidence | 12 MiB；每份JSON另有4 MiB限制 |
| 新live原结算event payload | 4 MiB，与原settlement fingerprint相同；旧Fake/其他event保持1 MiB |

3 MiB原头体最多产生6 MiB十六进制；输出聚合、SSE观察和terminal元数据另受上表限制，
为原响应和协议证据保留空间。live profile不接受可预知无法持久化的更宽配置。
独立 R3.1/R3.2/R3.3a 的旧上限和离线接口保持。有效输出的JSON转义可能大于原文本，
因此新增live原结算的event与原结算同为4 MiB，其他旧event不放宽。
接收后绝不为了落库再截断原观察。

## 原事务和证据路径

| 步骤 | 原持久事实 | 失败/重放 |
| --- | --- | --- |
| reserve_attempt | 原 call_attempts、原 request_budgets、完整模型信封、live frozen identity/writer | 任一步失败整体回滚；无网络效果 |
| claim_live_send | 每 attempt 唯一 claim、原预约 CAS/tuple、编码/配置 hash、writer | 精确重复返回原 claim 和 false；首发重核政策/绑定/预算；不得第二次发送 |
| perform_responses_https | 在事务外进行；原组件的本地连接/上传/响应观察 | 不重试，不把本地写入量称计费回执 |
| record_live_transport | 私有原 HTTP callback 字节、原响应、协议证据；另存安全摘要 | 校验原编码/配置/tuple/全部观察 hash；同 attempt 只能保存同一份观察 |
| settle_attempt | 原响应、原 nullable usage、结算hash、当时accepted；独立初始usage projection | 只能消费已保存原观察；原完整指纹重放保持不变 |
| reconcile_usage | 追加证据行、独立 projection CAS、原预算逐维差额 | 同键同证据读回；异证据冲突；更新和event失败一起回滚 |

原 original attempt ID、call ID、角色、role call index、request/plan/task/instruction
版本、dispatcher/execution/policy epoch、冻结配置 hash 全程不换标。AMEND、取消、
恢复、政策变化发生在网络期间时，完整响应仍可作为原账目保存，业务 accepted 保持 false。
Schemas、HTTP 成功或 decoded Completed 不授予工具或交付权限。
`LiveAttemptResult.sent` 当前直接映射 `transport.network_started`，只表示网络执行已启动；
完整上传、远端接收或计费须看各自原证据，不能从此布尔值推导。

原调用事件和响应行属于私有 Store 资料。`live_attempt(id)` 只返回安全身份/传输摘要和
用量；`live_attempt(id, true)` 是受信宿主显式原证据读取，不能直接用于共享日志。
`record_live_transport` 的 event 不展开 raw response、headers/body 或 provider diagnostic。

## 崩溃恢复

重开 Store 时仍先获得原单写者锁，再按 AllRequests/OneRequest 范围恢复。
恢复前一 writer 的 pending attempt 变为 UNKNOWN；读取旧预约或旧 claim 从来不触发网络。

`settle_unobserved_live(root, attempt_id, expected_attempt_version, reason, idempotency_key)`
要求当前 Root、原请求范围和 attempt CAS。当前 writer 的在途调用、已有 transport、
无关原结算、错误 reason 或重用异意图幂等键均拒绝。

| 原持久事实 | 原宿主结算 | 后到核对 |
| --- | --- | --- |
| 原预约，无 claim | reason=not_dispatched_after_recovery，Interrupted，五维null，accepted=0 | 有无claim原标记且仍无claim时，host_no_dispatch 可确认五维0 |
| 原预约及 claim，无 transport | reason=unobserved_after_recovery，Interrupted，五维null，accepted=0 | 只有可信来源才能补实际用量；未知不能转成免费重试 |
| 原 transport 已保存，尚未结算 | 消费完全相同原响应/usage；恢复后的陈旧绑定 accepted=0 | 可继续追加实际用量证据 |
| 已有原结算 | 只读或原完整指纹重放 | 独立投影对账；不改原响应及资格 |

有 claim 的宿主中断结算之后，原 transport 迟到可以追加原观察，但原中断结算不会被替换。
无 claim 的已恢复 attempt 永远不能新领发送权；发送必须是经过新准入和新预留的新 attempt。

## 逐维账本

用量维度为 input_tokens、output_tokens、cache_read_tokens、cache_write_tokens、cost_micro_usd。
这些字段独立可空。cache 不是另外一份可加到总 input/output 的 token；缺失 cache 不能填0。

新确认某原预算维度时，`reserved -= original_reservation`、`unknown -= original_reservation`、
`spent += confirmed_actual`，其他维度不动。已确认维度的后到相同值不再调整预算。
原预留为0也不使未知变已知；是否完整同时检查逐维 null。

后到矛盾值保留原已知值、追加 conflict 和 disputed。超出原预留的实际数额仍记全，
`over_reserved` 是持久告警；新付费预约/首次发送均阻塞。全部五维已确认且无争议时账目
可以完整，即使实际超支；必要收尾仍另外检查原 process/operation、交付及 ownership 条件。

`VerifiedUsageEvidence` 只能由宿主 verifier factory 构造有效内容。verifier 负责来源认证、
解析和单位；Store 再校验原 attempt/settlement、来源种类、证据 hash 和投影 CAS。
host_no_dispatch 只用于无发送权的原恢复标记，且必须五维明确为0；不能清除已发送未决。
当前没有通用供应商账单抓取、数字签名实现或自动价格估算。

`usage_projection` 同时返回原结算身份/usage、独立 projection、unknown dimensions 和核对链。
`verify_usage_ledger(request_id)` 按原预约/结算和每条核对记录重建预算；对记录/hash/链/差额/
当前投影不一致直接报 Corrupt。此一致性检查不是供应商审计证明。

## 迁移与旧请求

新增 011 和 012，不改发布过的 base/004–010 SQL。CLI 构造 Store 使用完整资产，默认410；
显式只提供旧资产的旧库/API测试仍按原版本执行。已有数据的结构升级要求显式 AllRequests
维护，OneRequest/AdmissionOnly 不偷偷迁移兄弟请求。ReadOnly 不能迁移或核对。

旧 Fake attempts 不插入 live identity，不补造 transport，不重写 usage 或原结算。
原 acceptance/owned cleanup 在410只对 live 行消费独立投影；Fake 行保留原未知判断。
工具 UNKNOWN、原进程退出证明、成功回执、经验作业和精确清理库存依然各自是独立门禁。

## 未完成范围

本实现与验证只到原 Store + 临时 SQLite + loopback TLS。CLI/OfflineRuntime 的通用真实
模型调度、真实端点能力及凭据配置、可信 provider verifier、实际小任务/单 Agent 质量对照、
并发服务化和强沙箱部署正向均不由本节点宣称完成。R3.3c 本地框架见
[R3 评测框架](R3-evaluation-framework.md)。
