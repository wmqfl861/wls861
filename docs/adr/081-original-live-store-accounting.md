# ADR-081 — 原 Store live attempt、唯一发送权与追加用量核对

日期：2026-10-27。基线：`56185a48303deb1c72b2ec53d896e8c0b32055f0`。
接续 [ADR-080](080-bounded-https-transport.md) 和 [R3.3 计划](../implementation/R3-controlled-live-plan.md)。
本决定覆盖本轮五节点中的前两项；实施契约见 [R3 原 Store 集成](../implementation/R3-original-store-live.md)。
它不授予供应商调用、凭据或付费测试权限。

## 问题

组件的本地上传观察不能证明供应商计费，也不能替代原请求预算、当前执行绑定和一次性发送权。
在崩溃或取消后重新发送旧 attempt，会把未决调用当成免费重试；在原结算行补写费用，又会改变
原响应的事实及精确重放结果。需要保留原事实，同时让后来有来源的用量证据完成账目。

另外，崩溃可能发生在原预约之后、领取发送权之前。仅给已有 claim 的调用提供恢复结算，会
留下永远无法对账的预约。因此必须分别表达“未取得发送权”和“已取得发送权但没有原响应”，
不能将这两种状态都命名为没有发送。

## 决定

1. 使用原 `call_attempts`、`request_budgets`、Store 单写者锁、请求范围及事务。
   新增迁移 011/012，分别产生 Store 409/410；完整 live 准入要求 410。
   409 可以识别结构和只读检查，不能绕过完整账本前提发起新的 live attempt。
   已发布 SQL 不改；旧 FakeModel 预约、快照和结算保持原字节及解释。
2. 外层模型注册可以显式选择 `openai-responses`，完整 profile、传输 CA/路由/容量、stream、
   execution kind 和能力依据随请求冻结。真实凭据仍仅以引用名称存在，值由受信宿主作为
   调用期间的独立内存输入传递。默认模型调用开关关闭；CLI/OfflineRuntime 仍只运行 Fake。
   `execute_live_attempt` 另外要求宿主 `network_enabled`，provider kind 还要求
   `provider_enabled`。本地测试使用显式 loopback 和合成 bearer。
3. 原预约事务验证完整模型信封、冻结角色/profile、政策、版本、允许工具、正数 token/费用
   预留及原预算，保存 live 身份和 writer instance。发送前取得不可变、每 attempt 唯一的
   durable claim，再核原编码/配置/绑定与当前准入。只有第一次提交返回
   `new_send_authority=true`；精确重复只能读回既有 claim，不能取得第二次发送权。
4. 新live注册在发送前核对持久化容量：原头体累计3 MiB、HTTP body 1 MiB、输出文本和
   聚合工具参数各256 KiB，并限制SSE条目/行/event等。原hex最多6 MiB，为decoded response
   与protocol evidence保留空间；超出此live包络的profile在准入前拒绝，不事后削去原观察。
   独立离线codec/transport上限保持。live原结算event payload跟随原fingerprint的4 MiB
   上限，覆盖合法文本JSON转义膨胀；旧Fake/其他event仍1 MiB。网络执行发生在 Store 事务之外。原有 R3.3a HTTPS 组件承担严格 TLS、单 deadline、
   取消和原 HTTP/SSE 解码。429、5xx、deadline、连接中断均不自动重试。
   完整 transport、原响应及原应用层字节进入私有观察行；event 使用安全摘要。
   本地写入量、完整 HTTP 响应和供应商用量证明分别表达，不能互相冒充。
5. live 原结算只能消费已保存的完全相同原 transport 响应，或者受控恢复事务生成的宿主
   中断观察；任意宿主拼出的另一份 response/usage 不能结算该 live attempt。
   原结算仍按完整指纹精确重放；后续核对不修改 `call_attempts` 的原响应、usage/null、
   状态、accepted 或 settlement hash。迟到响应保留原 tuple，当前绑定不成立时 accepted=0。
6. 重开 Store 先通过原恢复机制把 pending attempt 标为 UNKNOWN。受信当前 Root 可用原
   attempt CAS 为前一 writer 的调用创建不可变宿主恢复记录及原 Interrupted 结算：

   - 有原 claim、没有 transport：`unobserved_after_recovery`。发送/远端结果仍未知。
   - 没有原 claim、没有 transport：`not_dispatched_after_recovery`。只证明原 Store
     从未授予发送权，不伪造 HTTP、供应商响应或计费凭证。

   两种原宿主响应的五项用量都为 null、正文和工具为空、accepted=0。标记和结算在同一
   事务中提交。当前 writer 尚在途的调用不可强制走此恢复分支。后来的原 transport 可以
   追加在有 claim 的宿主观察之后，但不能替换该原结算或重新领取发送权。
7. 后到证据使用私有 `VerifiedUsageEvidence` 和宿主注册的 verifier。模型不能调用该
   verifier，也不能通过自报 `verified=true` 取得可信证据。来源分为：

   | 来源 | Store 接受条件 | 不代表什么 |
   | --- | --- | --- |
   | `loopback_fixture` | 原 execution kind 必须为 local_tls | 不证明真实供应商计费 |
   | `host_verified_provider` | 宿主 verifier 认证来源、单位及原 attempt/settlement | Store 不内置通用供应商签名、账单抓取或价格估算 |
   | `host_no_dispatch` | 存在无 claim 恢复标记、仍无 claim、五项明确为 0 | 不能用于有发送权或已发送后无响应的调用 |

   no-dispatch 是追加的宿主证明，而不是在原 null 结算内自动填 0。外部供应商证据的
   真实性由具体受信 verifier 负责；SHA256 是字节一致性检查，不是来源签名。
8. 每次核对携带独立 ID、幂等键、原 settlement hash 和投影 CAS，保存证据源/原字节/hash、
   原值、新值及逐维差额。已知值只可保持，未知值只可追加确认；矛盾证据追加为 CONFLICT，
   置 disputed，不能覆盖前一值。只对本次新确认的 input/output/cost 释放相应预留并记实支。
   cache read/write 独立可空，不将未知当 0，不重复加到总 token 上。
9. 所有核对行不可变；投影与原预算在同一事务中 CAS 更新。新 paid attempt/首次发送遇到
   任一 live 维度未知、dispute 或 over-reserved 即拒绝。真实支出超过原预留仍如实保存并
   保留告警，不能截短以满足预算。完整且无争议的超支账目与未知账目不同：前者停止新付费，
   但在原交付/退出/所有权条件成立时不阻止必要收尾。
10. `verify_usage_ledger` 从原预约和原结算重建初始投影，逐条校验附加证据 hash、CAS、
    已知值、差额与投影链，再重建原预算。`usage_projection` 同时呈现原结算与当前核对
    状态。核对不恢复业务资格、不清工具 UNKNOWN、不重发工具、不改包或用户回执；
    OneRequest、终态、只读、Root 身份和原清理条件各自继续生效。

## 验收和范围

验证使用临时 SQLite、普通用户、合成凭据和本地 TLS；包括失败事务整体回滚、唯一 claim
竞争与重放、实际 SIGKILL 后恢复、四类 in-flight binding fence、逐维核对、矛盾/超预留、
无 claim 原证据、终态和兄弟请求隔离、旧 408 迁移及原 Fake 字节兼容。
实际结果见 [R3 Store 验证](../reports/r3-original-store-validation.md) 和本轮总体报告；
本 ADR 的完成条件本身不等于门禁通过。

真实供应商执行、真实 provider usage verifier、端点能力验证、单 Agent 质量对照、
强沙箱部署正向和服务化仍分别验收。没有新增凭据环境解析、通用 live CLI、自动付费重试
或生产资料操作。R3.3c 本轮只提供固定任务及原证据报告框架。
