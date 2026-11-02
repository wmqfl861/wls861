# N5-2 图与审核事实：存储准备接口

状态 **DESIGN_PREPARATION / PRODUCTION_NOT_STARTED**。N5-1 关联方案稳定及协调者存储批准
是前置；[ADR-094](../../adr/094-persistent-event-call-association.md) 的选择不自动授权本卡生产实现。
本页准备将来的事实依赖，不改 `task_deps/review_tickets`、Schema、Store API 或生产迁移。

## 当前代码的具体边界

- `src/store.cpp:1570` `independent_nodes` 限最多 20 个节点且受原冻结 team 上限；明确要求
  depends_on 为空、无 review/capability/input。`apply_independent_plan` 的原原子分配可复用，
  但不能直接删除这个拒绝条件冒充有 DAG guard。
- `db/schema.sql:129` 的 `task_deps` 无 plan_revision；旧主键不能保存不同图版本全部边。
  原 Plan 正文可作历史证据，但派发仍缺事务内的当前图／逐边原验收证明。
- `src/store.cpp:2430` 的 `accept_candidate` 明确拒绝 required review；原 `workflow_submissions`
  保存不可变 binding/body/artifact_snapshot/record_sha256，但 submission_id 不等于 artifacts.artifact_id。
- `review_tickets`/ReviewRequest 有 producer dispatcher_epoch，缺完整 producer execution/policy
  域；reviewer 的字段不能代替 producer。原参考 Schema/校验器、M1 Journal ReviewRecord 及
  WorkClass::Review 不构成 S1 Store 的独立审核授权。

## 必须先冻结的逻辑事实

| 后继事实 | 拟议不可变 key／payload | 使用 N5-1 的依赖 |
| --- | --- | --- |
| 图版本 | (request,requirements,plan_revision,graph_format)，完整 Plan 原字节/hash、节点和边、政策版本 | 发布与节点分配／唤醒同事务；对应原 event link，不能重放旧 plan 刷新 graph |
| 图边 | (graph_id,from_node,to_node)，必需性／首版失败政策；节点映射到原 task/team | 不复用无版本旧 `task_deps` 覆写历史；错误图零 plan/task/team/edge 半写 |
| 逐边依赖凭据 | predecessor task/submission/candidate/hash/acceptance ID/CAS、完整 binding 和 graph_id | 首次派发 T2 与业务 T4 分别核当前图和每条证明；ready 列表只是观察 |
| 候选映射 | candidate_ref/artifact_id ↔ submission_id，版本化完整文件集合封装原字节/hash 和原 producer tuple | 从保存的原快照构造，一次写定；不能读取当前工作目录或用模型自报 hash |
| 双边 ticket | producer/reviewer 各 request/task/team、完整十域 binding、双方 CAS、submission CAS、candidate/hash、review_id/round/max_rounds、原输入/hash、必需 checks、预算归属 | 审核 callback 原 attempt/link 不能刷新任一方；接收和验收各自复核双方当前资格 |
| verdict 与检查 | 原 reviewer attempt/settlement/feedback、模型 verdict/findings、独立 host checks 的版本／证据、有效性／失效理由 | 原响应保留，精确 replay 不加轮次；模型 pass 与确定性 checks 分别满足后仍需 Dispatcher 明确验收 |
| 预算归属 | parent 总预留、review child 允许扣用的有限子账本与额度／计数原键 | 不新建独立 child 总预算绕限；inconclusive/UNKNOWN/轮次耗尽阻断，不自动批准 |

图准备限定 `a→c←b`，两条必需边，无可选失败政策扩展。a/b 当前 ACCEPTED 的完整事实同时有效
才可派发 c；FAILED/CANCELLED/SUPERSEDED 阻断并留原因待办，UNKNOWN 单独阻断，不能转 FAILED。
AMEND/新 plan/说明／恢复使旧证明失效；在途旧结果仍记账，但不能提交到新图。

## 拟议事务与退出分界

第一阶段未来 `apply_dag_plan` 在同一事务校验全集节点、唯一端点／边、无环、自环／间接环、
请求／图版本、原模板／约束／team 容量，随后写 plan/节点/版本边/初始就绪证明和原唤醒。
无效输入不写半个 plan 或团队。T2/T4 增加原 graph 与依赖 guard，而不是信任一次 ready 查询。
历史 graph 不删除；不能把旧 410 请求静默升级成 DAG。

第二阶段未来 `prepare_review` 需要真实获准的另一 reviewer 请求／团队与明确跨请求 host scope。
生产 Actor 不能自称 reviewer；OneRequest 不能因有 child_id 获得另一请求写权。双方 source tuple、
candidate 映射和 parent 预算先固定，再经同一持久关联／唯一派发产生原 verdict。
changes_requested 留原 findings，走新的 task/instruction 和 Worker ack；共享内部返工计数不重置。
round 上限维持原 Schema 1..5 与更紧配置；重放不加轮次，耗尽／不明向用户升级。

两种 Review reserve 仍是排程容量。producer 等审核释放活跃执行槽，但不释放未知预算、保留结果或
冲突资源；reviewer 使用原不可变候选和独立资源 key，不等待 producer 同步占住的池槽。
异步第一片无文件 executor；真实候选仍来自原受控 WorkerTools／提交路径。需要阶段切换时须先
设计 owner/drain/claim 交接，不在 callback 访问 Store，不把同一 attempt 再交同步 runner。

B 的 013 只服务事件关联，**不预先塞入未审核的 DAG／ticket 迁移**。本页逻辑表名／关系不是
DDL 批准。将来先选择图历史与候选映射载体、双边授权和 parent 预算方案，再给单独新增资产／
版本设计、旧 411 解释和回滚门禁；保留历史 SQL/Schema 原字节。

## 未来验收的最小事实查询

- 图发布：a/b/c 和两边的同 graph hash；环、重复、跨请求端点、超容量各零半写。
- 汇合：仅 a 接受时 c 的 dispatch count=0；b UNKNOWN／FAILED 各留下不同原因；双方有效接受后 c 仅一次派发。
- 版本竞争：就绪观察后 AMEND／recovery，在 T2/T4 各拒绝旧图；原 response/usage/图字节仍在。
- 票据：同候选内容但不同 submission／版本、producer 自审、任一方 epoch/CAS/policy 改变均拒绝；不刷新 ticket。
- 返工：changes_requested 新说明／ack；精确 verdict replay 次数不变；轮次耗尽／UNKNOWN／checks 缺失不接受。
- 崩溃：publish/ticket/verdict/acceptance 前后套用 N5-1 原子事实和逐 ID oracle，无唯一派发／原反馈则保持 HOLD。

这些均为 PLANNED / NOT_RUN。N4-B 已列的 B01–B09 继续作为后继验收范围，本文补足存储前置，
不以模型 Schema、设计文件或离线表关系演示宣布 DAG／独立审核生产闭环。
