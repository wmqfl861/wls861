# ADR-085：受控 live 调用贯通原事件工作流

日期：2026-10-26（America/Los_Angeles）。基线：
`1c572d3aa9121e85cf22aa1e4bb330b25a34a143`，Store 410。

## 背景

R3.3b 已实现原预约、唯一发送权、传输观察、不可变结算和追加对账。
原 `OfflineRuntime::reserve_call` 仍限定 FakeModel，并把费用预留固定为零。
`execute_live_attempt` 在返回前完成结算，而原响应处理器对“已结算但没有原反馈”的调用
保持阻塞。因此，将一个 live adapter 直接替换 FakeModel 不能正确连接现有工作流。

Responses 协议本身不提供可直接采用的实际费用；codec 保留 null。
把未知费用补零、从价格估算出实际账目、再次应用原已结算响应，都会破坏既有契约。

## 决定

1. 提取 `capture_call`：构造原版本、角色、权限、完整当前上下文和单个管理目标，
   但不预约、不解析凭据、不发网络。旧 Fake `reserve_call` 仍调用原预约，保持兼容。
2. 提取 `observe_live_attempt`：原 Store 预约 → 原唯一发送权 → TLS → 私有原观察。
   只有新取得发送权且原观察已持久化时才返回本次 ModelResponse；此函数不结算。
   旧 `execute_live_attempt` 包装该路径及原结算，保持其已有完整调用语义。
3. live 事件泵把新观察交给原响应处理器，执行首次原结算、可选受信用量核对和原业务应用。
   原 Schema、角色、任务/请求 CAS、政策和业务门禁继续适用，不增加管理工具权限。
   经验报告的 job ID、版本和 source hash 跟随原预约，终态调用仍只允许原经验目标。
4. 核对 hook 位于原结算之后、业务应用之前，可使原本要求完整账目的验收路径采用独立证据。
   它只接收宿主构造的 `VerifiedUsageEvidence`；普通 JSON、自报 verified 或模型文字不具该能力。
   hook 缺失时，原本允许的业务动作仍遵守原守卫；任何五维未知、争议或超预留都会停止后续调用。
   原本要求完整账目的动作依旧拒绝，其错误反馈保留，核对后也不会重放这个响应来恢复权限。
5. `pump_live` 使用原事件 claim、原工作流循环和原工具反馈；live AMEND 不接受宿主 dispositions。
   管理目标在 live 预约和凭据读取前校验是否属于原 claim。所有实际错误停止整个 batch。

## 授权、边界和输出

- 原请求必须显式冻结 `real_calls_enabled=true`、完整外部 profile、structured-v2、当前工具能力
  及无自动付费重试策略。模型、端点、CA、认证引用与传输限制都来自冻结配置。
- 运行宿主必须另行授予 network 权限；provider 还需独立 provider 权限。默认均关闭。
  新 CLI 授权输入绑定请求、配置和模型 profile；该输入是受信本地主机授权资料，不是签名证明。
- 每个 attempt 的费用预留必须显式为正数，仍受原请求总预算限制；该数是预留，不是实际费用。
  输入/输出预留继续来自原上下文容量与冻结模型能力。
- 凭据 resolver 只收到冻结的环境变量引用名，在配置、claim、协议、账目与预算预检后调用。
  异常使用固定诊断；凭据值不进入快照或公开结果。受信 resolver 和核验 hook 必须自行有界。
- 每 batch 最多 64 次调用、每 Worker 最多 16 次；冻结管理调用限额继续适用。
  显式 batch 墙钟上限为 1 毫秒至 1 小时。一个随调用销毁的 deadline watcher 向原 TLS
  等待传播 stop，不创建 Agent、自动重试、并发服务或常驻后台任务。
- 公开报告只显示标识、状态、版本、计数、账目和 hash。原响应正文、工具参数、文件原字节
  保留在私有 Store。缺少传输观察时显示未知，不能用已观察发送计数零证明没有发送。

## 崩溃和兼容

原 SQL 和 Store 410 数据结构不因该工作流接入而改写。发送权重放仍为零发送；
原结算缺反馈仍 NotReady；原响应有反馈时只返回原结果，不重做工具。
预约、claim、观察、结算及反馈之间的崩溃使用现有恢复/UNKNOWN/附加核对规则。
一次核对不会修改原响应或原 accepted，也不会恢复取消、AMEND 或过期代次的业务权。

旧 run-fake、pump-fake、OfflineRuntime 和 execute_live_attempt 的限定语义保留。
本节点不实现实际供应商账单核验、不执行付费验收，也不把本地 TLS 脚本当成模型质量证明。
单 Agent 全任务对照涉及独立持久身份/权限与实验契约，不能把多个 Worker 改名为单 Agent。

实现接口及复现范围见 [受控工作流契约](../implementation/R3-live-workflow.md)。
实际结果由本轮验证报告及对应最终 SHA 的 CI 记录，本文不是执行成绩。
