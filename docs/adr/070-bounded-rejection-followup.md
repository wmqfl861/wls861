# ADR-070 — 被拒提交的有限返工与用户出口

日期：2026-10-25。状态：采纳。目标蓝图 v0.4.1。基线 19ea603（R1.3）。

## 问题与范围

否定 Dispatcher 验收和 TeamLead 返工耗尽都保留 BLOCKED 提交，但当前 pump 没有管理后继。
本切片 R2.1 增加 Dispatcher `resolve_blocked_submission`，不同时改按请求恢复、模型 AMEND、
通用 change_task、真实模型或数据库结构。原拒绝决定本身仍只拒绝，不隐式批准返工。

## 决策

宿主在预约管理调用前捕获一个当前 BLOCKED 提交、任务 CAS、完整原提交/检查与拒绝证据、
冻结团队返工上限及累计次数。模型只能返回 `action: rework|ask_user` 和非空 `reason`，
不能提供目标 ID、版本、团队、额度或替代验收标准。Store 在提交事务重查身份、当前代次、
请求/任务/说明语义版本、政策、CAS和等待；返工另需操作/用量结算。上报用户作为控制仍可
保留UNKNOWN，但绝不解除它，调用结果不是执行授权。

`rework` 是保持原 TaskBrief 的受限 change_task：累计计数与既有 TeamLead 返工共享，
只有次数小于冻结 team_internal_rework_max 才可执行。事务增加 task_revision，将原提交标记
RETURNED_FOR_REWORK、暂停团队并保留原说明。TeamLead 接到原始拒绝及理由后发布新说明；
新 instruction_revision 和 Worker ack 完成前禁止新业务。需求/计划/原提交字节及所有消耗不改。

`ask_user` 每份被拒提交最多创建一个原生 Dispatcher 问题，直接交用户并持久绑定精确等待。
它可用于澄清或返工额度耗尽，不假装 Worker 提问，也不让模型代签用户答案。回答仍使用原
Root/answer 路径，只解除所属等待；有剩余额度时 Dispatcher 可重新捕获目标后选择返工。
耗尽时必须由所有者另行 AMEND、CANCEL 或 NEW，不通过回答、重启、普通重发说明或检查点
重置累计次数。问过的问题不重复创建；未答或耗尽已答时不自动再预约 Dispatcher。

新事件及其规范化 command/result 保存来源与处置，保持命令重放、CAS和原反馈；不修改已发布
SQL/vendor/原审核。业务返工计业务池，提问计控制池但不享有无限结算特权；受问题数/调用预算
和已有存储容量保护。等待、UNKNOWN、未处置 AMEND、终态及已停止团队均不可被后继绕过。

## 验收

先在旧代码上复现否定验收后不再调用模型；新增回归要求拒绝→显式返工→新说明/ack→再次验收，
TL耗尽→一个用户问题，重开不重复提问/不清零，答案不解锁耗尽或UNKNOWN，兄弟任务可继续。
覆盖重放/不同意图冲突、旧CAS/迟到响应/旧政策/AMEND/取消/外来目标、数据库提交故障原子性，
并按 AGENTS.md 执行普通用户 Release/UBSan/ASan/Make 与同一提交 CI。成绩单独记录。
