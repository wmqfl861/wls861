# N5-1 / N5-2 持久关联存储选择包

日期：2026-11-02 UTC。状态：**DESIGN_PROPOSED；生产实现未开始；全部新增运行验收 PLANNED / NOT_RUN**。
当前阅读输入为 `aa2c592b8aed6509189710cc6f6d69e0959098a8`，父提交
`b58f7b0e63ad68f67f124eddd303825ade817791`；运行版本仍 `0.11.1-r3.3c-audit`，Store 410。
本文提出新设计，不宣布存储采纳、迁移批准、实现完成、PR 正式批准或合并。

## 协调者可以直接作出的选择

建议采纳 **B：新增附加事实表和 410→411 的显式迁移**，首版仅支持两个请求、每请求两个
独立节点的 FakeModel `plan → instruction → ack → note`。使用同一个 Store owner、原 A3/A4
调度和预算；模型 callback 不访问 Store。新模式只显式启用在没有旧调用／操作的新请求上。

需要协调者记录的决定是：

1. 选择下表 A 或 B 的持久权威；是否允许后继单独实现新的 013 迁移／411 版本门禁。
2. 是否采纳 [事务契约](storage-contract.md) 的 T0–T4，尤其数据库内业务效果、原反馈和逐 ID
   完成凭据同事务提交。它要求提取现有内部事务步骤，不能嵌套调用现有会自行开启事务的公开方法。
3. 是否采纳新模式对旧同步路径的显式拒绝、只读旧库解释、无原地降级的回滚政策，以及新请求的
   `max_links≤64`、`max_claims≤128`、每 claim≤64 IDs 和有限收尾预留。原冻结上限只能收紧。

**本包尚未获采纳。** 如不批准迁移，允许交付的保守管理子集见下文；不能据此宣称 Worker
持久闭环完成。选择 B 也不启动 N5-2 生产实现：仍须 N5-1 方案稳定、实施门禁和协调者明确排程。

| 比较项 | A：不可变关联事实追加到现有 `events` | B：新增附加表／迁移（推荐） |
| --- | --- | --- |
| 权威表示 | 版本化 `n5_event_*` 事件；从 LINK/DISPATCH/SETTLEMENT/CLOSE 等事实折叠 | 独立 claim/link/member/receipt 不可变事实；开放 ID 索引只作可核对投影 |
| 真实可重用点 | `Impl::event/replay` 已同事务写原字节、命令 hash 与 result；原事件只读可追溯 | 复用原 events、attempt、feedback、inbox 及事务／原 guard；新表只存关联和证明 |
| 不可回避的代码变更 | 仍须新 typed Store 路径、事务内关联／确认校验、容量分类和恢复门禁；没有通用公开 append API | 同样需内部事务组合和新 typed 路径，另新增表、约束、索引与迁移资产注册 |
| 单 ID 唯一未决关联 | 同一写事务有界折叠请求全部新协议事实后检查；不能用未来队列当索引 | `open_items.inbox_id` 唯一约束，再核原 request/role/task；历史 member 允许明确的新回合 |
| 崩溃定位 | 每次恢复按版本严格折叠，检测缺步／重复／冲突，保留原 claim snapshot | 按 link/attempt/ID 索引查不可变 receipt；缺项显式 HOLD，投影不一致拒绝 |
| 容量／查询 | 每次新事实消耗现有事件池；累计 checkpoint 不限制终身扫描，需新的终身限额或附加索引 | 新事实行／字节单列有界；原事件和预算照常计数，收尾空间在准入时预留 |
| 旧二进制误消费 | **纯 410 追加不能阻止旧 EventRuntime 忽略新标记后消费 ID**；完整 Worker 目标至少还需 411 版本门禁（元数据迁移） | 411 在旧程序接受范围之外；旧程序在业务恢复及 journal-mode 改写前拒绝 |
| 旧 SQL／旧快照 | 原文件／hash 不变；不能把未知字段塞进旧模型 Schema 或改旧指纹 | 原文件／hash 不变；追加 013 和注册资产，旧请求默认不启用新模式 |
| 审核／回滚成本 | schema 少，但关联唯一性、日志折叠、故障定位和容量证明复杂；“无迁移”不满足完整兼容门禁 | schema 明确，能逐行审计；旧程序不能直接打开 411，须保留新版本读取／收尾或恢复升级前快照 |

A 不是通过现有公开 API 就能安全完成的完整方案。若协调者选择 A，建议采用 **A+411**：
新增最小元数据／版本资产并在新程序实现相同 T0–T4、完整 binding、终身计数和故障 oracle。
纯 410 形式只可用于明确隔离的设计实验，不能交付完整 Worker 目标。

## 当前源码核对出的具体缺口

| 当前来源 | 已存在且可复用 | 当前缺口与新设计要求 |
| --- | --- | --- |
| `src/store.cpp:1872` `claim_events`；`db/migrations/006_event_runtime.sql` | token/generation、actor 范围、需求／epoch、具体 IDs；原事务逐 ID claim | `event_actor_claims` 对 actor 主键 UPSERT；历史 claim 会被覆写。T0 保存独立不可变 snapshot |
| `src/store.cpp:1906` `finish_events` | 只完成捕获子集，遗漏回 PENDING；旧 claim/owner 拒绝；CLOSED 重放集合一致性 | 没有 attempt／settlement／业务／feedback 证明参数。新模式只能由 T4 证明路径完成，不能直接信任 completed IDs |
| `src/store.cpp:3722` `reserve_attempt`；`:3647` `claim_offline_call` | 完整 envelope、原 binding、预算和唯一 `offline-dispatch:<attempt>`；重放不再派发 | 关联与预约目前分开；T1 同事务写 link，T2 同事务记录原首次派发及 link receipt |
| `src/offline_runtime.cpp:731` `accept_captured_response` | 原 envelope/action/CAS 核对；先结算，再业务，再反馈；已结算缺反馈时拒绝重执行 | 原路径跨事务，note 又有 prepare/start/finish 三步。新 opt-in 数据库内子集需要 T4 原子组合 |
| `src/store.cpp:2002` `record_tool_feedback` | 原 settlement/response/binding 匹配；不可变反馈；只保存原事实 | 不能把缺反馈当成功，也不能靠再次调用 adapter 重建。T4 失败回滚业务尾部并保留原响应 |
| `src/async_coordinator.cpp:130,318,407` | 先持结果槽再预约、owner 首次 claim、Ready/Retained、close join | 当前不消费 inbox；仅把 future 接上 `finish_events` 仍有崩溃缺口 |
| `src/event_runtime.cpp:510–565,841–978` | `action_in_claim`、question/rejection 判定、逐 subject AMEND、原完成语义 | 局部判断不能直接复制成异步“整批成功”；需共享 typed proof predicate 并保留原同步语义 |
| `src/store.cpp:503`；`src/migrations.cpp` | 401–410 版本拒绝边界、原资产指纹、显式 all-request 升级、WAL/FULL | 013/411 不存在；本包没有修改这些生产路径 |

上述行号均针对 aa2 输入。完整的 [事务／binding 设计](storage-contract.md)、
[崩溃窗口和验收 oracle](crash-and-acceptance.md)、[N5-2 后继事实依赖](n5-2-dependencies.md)
共同构成提案，不能只批准一张比较表而遗漏恢复和容量条件。

## 无迁移时可以诚实交付的范围

可以准备只读管理待办分类器：用 `inbox/plan/question/submission/experience` 的现有完整对象
事实解释某个原捕获 ID 是已处理、已失效还是仍未决；在当前原 claim 下使用原 `finish_events`
子集规则。它不新增自动模型调用、不建立跨重启去重，缺原对象事实／纠正脚本／额度则保留待办。

可解释的例子包括：同一 question 已 ANSWERED 或持有人已转走、原 experience job 已 DONE、
原 AMEND subject 已由同需求／计划下的显式处置覆盖。问题回答不能代替 rejection 纠正完成；
已有 plan/instruction 不能证明 Worker note 已发生。旧 Worker 历史无法由现有 inbox API 唯一关联
原 attempt 时返回 `UNLINKED_HISTORY`。这项最多是管理辅助，**不满足 N5-1 Worker 纵向目标**。

## 输入、方法和交付边界

两份冻结交接来自 `283d23070a1f36f2eea598bcb96b2a312cd71307`：

- `NEXT_AGENT_PROMPT.md`：8,314 bytes，SHA-256 `14b3d003633951684d2c33c5fac8dab5f7c5e7d24e65136b278a45745583fec1`。
- `REVIEW_AND_HANDOFF.md`：8,615 bytes，SHA-256 `c1274c17acd627679df924d2c211f2a37ab66a2bf278422deedd77415fb010b7`。

已完整读取两份交接、当前 AGENTS/STATUS/ROADMAP、N4 三卡与相关 A3/A4/S1 契约，并按本版本
源码核对上表行为。以前审核过的最终报告／索引继续保留原本地失败和未验收范围，不从设计工作
推导任何门禁通过。结构检索遵循已读 `codebase-memory` 技能；父协调者提供图
`wls861-next-cycle`、generation `2026-10-31T06:55:42Z`、旧 772 root，Store/Event 覆盖 partial、
Async 缺失。本作者工具目录无 MCP，因此**本轮没有图调用／coverage 验证**，结论来自当前源码
读／检索，不把旧图未命中当缺少接口的证明；这不是全仓审计。

本批仅新增设计文档和设计验收材料，没有改 SQL、Store 公共头／API、生产实现、CMake、CI、
STATUS 或历史报告。常规文档准备与纯静态检查属于本轮已授权范围；按冻结交接，存储架构须先
经协调者采纳后才开始生产实现。本轮未运行本地产品测试／编译，全部新产品验收为
PLANNED / NOT_RUN；没有伪造旧实现缺陷红例。设计作者的自查不是非作者审核或正式采纳。

六状态按生产能力登记：IMPLEMENTATION_COMPLETE=false；LOCAL_VALIDATION_COMPLETE=false；
PUSH_COMPLETE=false（本地准备截点）；REMOTE_CI_COMPLETE=false；PR_REVIEW_COMPLETE=false；MERGED=false。
本地小提交、后续推送与精确提交 CI 由协调者的外部交付回执登记，不预填审批或发布结果。
