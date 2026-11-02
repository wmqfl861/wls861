# N5-2 R v1：候选映射、双边票据与父预算

状态 **PROPOSED / NOT_IMPLEMENTED / NOT_RUN**。候选内容载体建议随图片 014/412 引入；
审核票据、ReviewPair scope 和借记账本属于**后续单独版本迁移，编号未分配**。
这里不给历史 SQL、ReviewRequest/ReviewVerdict Schema 加列、加字段或改变旧 hash。

## 候选与原提交的精确映射

推荐增加 `candidate_artifacts_v1`，固定 host 生成的 `art_` typed ID、request/task/team、
format=`submission-candidate-v1`、payload 原字节、SHA-256 和 size_bytes。它是新协议内的
数据库内容对象，不是假装已经存在的文件，也不是历史 `artifacts` 的一行。
历史 artifacts.path 的含义和原记录不变；不把数据库 URI 塞进该 path 来凑旧外键。
后继新 ticket 表引用这个新对象，**不写历史 review_tickets**。

对应 `candidate_submission_maps_v1` 固定 candidate_ref、submission_id、submission 原
record_sha256、原 producer 十域 binding 与生成时 current binding（分列）、生成时 task/submission CAS、原 producer
instruction/brief/requirements 引用及 payload hash。唯一 (submission_id, format)；
同一不可变提交精确重放得到原对象，不增 artifact revision 或审核轮次。同字节的另一个
submission 或另一语义版本有新的映射和 ID，不能套用旧 ticket。

payload 的确定格式建议是当前 C++ `canonical_json` 编码的对象：

```text
format: submission-candidate-v1
submission_id, request_id, task_id, team_id
original_binding: 原 workflow_submissions.original_binding 的完整十域值
source_record_sha256: 原 submission 已验证的 record_sha256
source_body_bytes: 原 body 存储字节的 base64
source_artifact_snapshot_bytes: 原 artifact_snapshot 存储字节的 base64
source_body_sha256, source_artifact_snapshot_sha256
files: 按原 artifact_snapshot 数组顺序保留 path/size_bytes/sha256/content_base64
```

明确数组顺序有意义，不按当前目录排序，不把当前 mutable status/CAS、查询时间或 session
放进内容 hash。映射的捕获 CAS 与内容身份分列保存。Store 先复核原 submission 的事实 hash，
再按现有 submit_work 规则复核每个原文件的 base64、实际长度和 SHA-256、唯一受控路径、
全量文件集合；禁止截断内容后声称候选完整。提案可能重复原字节，需将重复编码计入冻结容量；
体积超过上限就拒绝，不扩大原 524288 字节 submission 快照上限。

`candidate_ref` 在新协议中仍满足旧 Schema 的 art_ 类型，但解析必须带协议版本：只从新
候选表按确切映射读。未知格式拒绝；不先查不到新表再回退到文件系统或旧 artifacts。
将来若需要实际 artifact 文件，必须另有受控发布协议，不能由本包的数据对象授予文件权限。

## 双方身份和最小作用域

ExecutionBinding 的十域是 request_id、task_id、team_id、requirements_revision、
plan_revision、task_revision、instruction_revision、dispatcher_epoch、execution_epoch、
policy_epoch。ticket 必须分别保存 producer/reviewer 两组，不能拿 reviewer 的 epoch
替 producer，也不能仅保留 request ID。额外各存 task CAS、request CAS、policy/config
hash；producer 另存 submission CAS、candidate map/hash、graph ID，reviewer 另存原输入 hash。
当前 StoreActor 的 writer_session 是临时权限，仍在每次调用前验证，不写入持久票据。

推荐 D4 的具体宿主入口是新 `StoreRecoveryScope::ReviewPair(parent, child, mapping_id)`，
仅同一数据库、同 tenant/project/root 的两个明确请求，首片同时只启用一个 pair。
它是待批准接口，当前代码没有这个枚举。打开时分别执行原按需恢复，然后才签发各自 Actor；
整个 Store 仍由一个原 owner 线程和原独占写者锁拥有。原 OneRequest 仍拒绝另一请求，
AdmissionOnly 不签发执行身份；不能从 child_id、公开构造 Actor 或 WorkClass::Review 扩权。

为避免在 OneRequest 内偷做第二个请求的准入，建议分三个有记录的宿主阶段：

1. 原运行器 drain/close 并结算可确定结果；未解决 UNKNOWN 保留且阻断后续业务。
2. AdmissionOnly 通过后继 `admit_review_only_child` 创建冻结的 review-child-v1 请求，
   明确 parent/mapping/config/原有限预算。该新模式从准入起禁止任何普通业务／attempt，
   直到双边关系和父级额度原子登记；原 admit 路径不能产生相同新模式的未受限 child。
3. 关闭 admission Store，用 ReviewPair 打开同一数据库，恢复两方后取得新 Actor；
   双方当前 root 身份和冻结映射完全匹配才登记 pair。此时尚未创建 ticket 或消耗模型轮次。

pair 不是通用跨请求 capability。只允许固定候选只读、票据预约、原审核 response/usage
记录、有限 verdict/check 及当前 producer 验收／返工控制。所有其他 request 访问拒绝；
审核 Worker 不执行 fs_write/run，不读取 producer 工作区。只读内容来自 Store 原候选快照。
映射选择由受信宿主冻结，生产模型不能选择 reviewer、更换 parent 或声明自己是 reviewer。
首片使用一个明确登记的本地 FakeModel 审核能力；独立 request/team/principal，不是同名重标。

## 票据、原观察与业务采用

| 追加事实或投影 | 必须固定的内容与唯一性 |
| --- | --- |
| review_pairs_v1 | parent/child、原准入/配置/映射 hash、双方 owner、预算授权 ID；child 至多一个 parent，不嵌套、不转账、不重绑 |
| review_tickets_v2 | review_id，producer/reviewer 全部十域及原 CAS，candidate_ref/map/hash/submission，graph，round/max_rounds，必需 criteria/checks，原 ReviewRequest bytes/hash 和新 host envelope bytes/hash |
| review_dispatch_links_v1 | ticket、原 child call/attempt、N5-1 link、claim/generation/C/S、预算 debit ID；一个 ticket 至多一个已关联调用，不用恢复后的新 inbox ID再送 |
| review_observations_v1 | 原 response/verdict/findings、usage/settlement/feedback hash、原 unique dispatch receipt；同 attempt 精确重放只读，冲突拒绝 |
| review_host_checks_v1 | ticket/candidate/map/hash、criterion、host verifier ID/version/input/output/evidence hashes、pass/fail/not_run；每项事实追加，不接受 model evidence 自证 |
| review_ticket_state_v1 | PREPARED/RESERVED/DISPATCHED/OBSERVED/PASS_READY/REWORK_REQUIRED/BLOCKED/INVALIDATED/CONSUMED，CAS 与原因引用 |
| review_round_facts_v1 | parent/task/review-policy-lineage、ticket、round、原 max_rounds、原内部返工计数；不可变，语义重建不得归零 |

旧 ReviewRequest/ReviewVerdict 都是 additionalProperties=false。新 host envelope 与原 schema
正文分别存储；模型只填写允许的 verdict/findings，host 填原捕获运行时字段。完整的 producer
execution/policy、team、CAS 等新字段属于 envelope，不塞入旧正文。新 verifier 必须同时校验
旧正文与 envelope 的共有值相等，格式未知拒绝，不能只通过旧 JSON 形状就采纳业务。

拟议 `prepare_review(pair_root, producer_dispatcher, both_captured, candidate_map,
expected_cas, checks, budget_grant, command)` 在单事务保存 ticket、round fact、pair 版本、
原 producer 等待投影与 reviewer 待办。首次 ticket 创建消耗一次原 round；只允许一个活动
ticket，失败的准入事务零轮次变化。ticket 重放不增轮次；身份失效后的新 ticket 消耗下一轮，
不以 retry 名义绕上限。max_rounds=min(原 Plan 1..5、更紧冻结审核限制)，不得恢复／AMEND 归零。

`reserve_review_call` 原子复用 N5-1 T1，把双边 ticket、原 child reservation、父借记和
事件关联一次保存。通用 reserve_attempt 在 review-only child 上拒绝；不能独立预约后补关系。
T2 唯一 dispatch 复核双方当前身份、CAS、候选、pair、父预算状态以及原 link；任一侧恢复／
改说明／撤销／终态拒绝。callback 只得到有界原候选和输入，无 Store 权限。

T3 `record_review_observation` 依据原 dispatch 记录原响应／usage；即使取消、恢复、双方
scope 不再有业务权，也可在受限双边 accounting 路径写原事实和原父借记投影。
T4 `apply_review_verdict` 再核双方原捕获 tuple、CAS、候选和账目，才提交原 verdict 的业务
投影与精确 feedback/已完成 ID。先保存原观察，后业务拒绝；不得把 T3 成功当业务 pass。
将来的 `accept_reviewed_candidate` 再做全部原 S1 验收守卫，加有效 ticket/pass 与每项必需
host check；创建原 acceptance/experience 和审核消费事实同事务。旧 accept 路径仍拒绝。

双方 CAS 可能因**本协议自己**的等待／ticket 投影而变化。ticket 同时保存 prepare 前捕获值
与同一事务预定的 prepare 后值；后续只接受有原 transition receipt 的本协议变化。
不能要求当前值等于错误的前置 CAS，也不能以任意大于等于来容忍外部修改。

changes_requested 原样保留 findings，只有 review round 和原共享 team_internal_rework_max
都剩余额度时才准备有版本 ChangeOrder、新 task/instruction 与重新 Worker ack。
新成果必须是新 submission/map/ticket；不改原 verdict。inconclusive、额度耗尽、缺 checks、
UNKNOWN 均阻断并走原用户升级，用户回答不赠额度、不代签接受。精确 replay 零计数增量。
review-only child 不因“审核完成”自动取得生产验收/交付/删除工作区权限。

## D5：两种父预算方案及推荐

现 request_budgets 按 request_id 关联自身 call_attempts，原 verify_usage_ledger 重建这些
事实。把子请求费用直接加到父 request_budgets 而不改变可核验来源会破坏账本；制造一条
父假 attempt 又会破坏真实调用／响应和计数。两种方案都应新增归属事实，保留原各请求账本。

| 方案 | 原子协议 | 优点／代价 |
| --- | --- | --- |
| A：父级逐次借记（推荐） | 每个 child T1 同时追加 parent debit 并检查父聚合剩余额度；不预占未调用额度 | 最少额外释放状态；父业务与审核竞争同一冻结总额，公平调度不保证审核一定有剩余钱 |
| B：父级 escrow | 创建 pair 时按授权额度锁定父的可用子额；每个 child T1 从 escrow 转入借记，T3 按原 usage 转 spent/unknown；关闭时只归还可证明未用部分 | 可提前保证审核额度，但多一套 escrow CAS、释放／超预留协议；UNKNOWN 和在途金额不可归还 |

推荐首片 A，不实施 escrow。parent grant 是子账本上限，**不是已从父总额之外新增的钱**，
也不承诺未来一定有额度。每个 child 的 frozen AdmissionInput.budget 同时作为更紧上限，
每笔预约还受 grant 和 parent 聚合上限；任何一项不足在 T1 零副作用拒绝。成本即使 Fake=0，
原 max_attempts、input/output 和 nullable usage 仍逐笔计入，不能只约束 money。

建议新事实：budget_parent_grants_v1 固定 parent/child/授权四维上限／配置 hash，
review_budget_debits_v1 按唯一 child attempt_id 固定原 parent、grant、call/link/ticket、
预约四维和两方原 budget CAS，review_budget_observations_v1 按原 settlement/hash 追加已知／
未知变化。独立 group projection 只作缓存，可从原事实和本地账本同一读快照完全重建。
子请求所有 model attempts（含计划、审核、纠错和管理）都必须有唯一 parent debit，
不能只对最终 verdict 收费；首片禁止嵌套 child 和任何免费旁路。

对每个金额／token 维度 d，父级准入约束是：

```text
effective_committed(parent,d)
  = parent_local_spent[d] + parent_local_reserved[d]
    + sum(charged_child_spent[d] + charged_child_reserved[d])
effective_attempts(parent)
  = parent_local_attempts + count(unique charged_child_attempt_id)
new_effective_committed + reservation <= parent_original_limit
new_effective_attempts <= parent_original_max_attempts
```

原 reserved 已包含 unknown 部分，不再加一次 unknown，避免双计；UNKNOWN 为零预留时也
仍有未明状态门禁，不因数值为零放行。cache_read/cache_write 的可空性继续参与原完整
结算门禁，不重复加到 input/output 金额。所有整数运算须沿用防溢出验证。

parent 的每个普通 reserve 同样检查 group 聚合；否则 child 与 parent 交错可以各自通过。
唯一 owned Store 的同一事务更新原本地账本及关联投影，按 parent/child ID 的固定顺序读写；
本设计不增加第二 writer。两个 child grant 也不能复制父额度，首片一个 pair 仍测试重复建对拒绝。

T3 按原 settlement 逐维投影：已知值释放对应 reservation 并记实际 spent；未知继续保留
原 reservation/unknown 标志，父子都 HOLD。真实已知超预留时记录完整超额，停止后续业务；
不可截断实际费用适配预算。原 attempt 计数永久消耗，不因取消、未派发、失败或 replay 退还。
late response、恢复、cancel、close 只能记录原 debit 的事实；不能重新送审、改变 parent，
也不能为释放额度把 null 改成零。首片 offline 不授权 live reconciliation；未来接入需将其
原追加投影一并关联，不能把旧 nullable 原 settlement 改写。

父级验收／交付／终态收尾须核所有关联 child 的完整 accounting 和保留资源；新报告可以
增加明确版本的聚合摘要，但不改原 usage.json／receipt 的历史字节或把原本地账本解释成总账。
新增 `verify_parent_review_ledger` 复核两边 hash、唯一归属和四维总额；旧 verifier 仍核自身
attempts。任何账本或投影不一致都停止业务，原 T3 有界证据保存通道继续可达。
