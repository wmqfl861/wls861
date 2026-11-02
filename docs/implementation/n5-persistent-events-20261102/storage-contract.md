# N5 持久事件关联：拟议数据与事务契约

状态 **DESIGN_PROPOSED / IMPLEMENTATION_NOT_STARTED**；新增产品验收 **PLANNED / NOT_RUN**。
基线、选项和批准请求见
[选择包](README.md)与 [ADR-094](../../adr/094-persistent-event-call-association.md)。
以下名字、411 与 013 均为待采纳候选，不是当前 API／SQL；本文没有修改公共头或历史资产。

## 完整身份与 hash

每个关联保留以下不可变原值。恢复时可以取得当前 owner 来查询／收尾，但不能把它替换进原 tuple。

| 域 | 必须保存并在准入／首次派发核验 |
| --- | --- |
| 请求／Actor | request_id、role、principal_id、actor_task_id、task_id、team_id；原请求 tenant/project 归属引用 |
| claim | claim_id、generation、requirements_revision、dispatcher_epoch、execution_epoch；完整捕获 IDs 的严格递增集合 C，以及每项 inbox_id/event_seq/event_id/type/role/actor_task/subject_task/event_requirements_revision/source hashes |
| 执行 binding | 原 `ExecutionBinding` 的 request/task/team、requirements/plan/task/instruction revision、dispatcher/execution/policy epoch 全部十域 |
| 调用 | call_id、attempt_id、role_call_index、原 config_snapshot_id、model/profile 引用、完整 durable request_envelope 的 hash 和 reservation_sha256；不得只保存当前请求号 |
| 目标 | `expected_state_version`、workflow_action 或明确 plan/instruction/note 目标、各目标 ID/CAS/source hash；完整目标原字节及 target_sha256；所选 IDs S⊆C 与选择 predicate 版本 |
| 结果 | 原 settlement_sha256、完整 response/nullable usage 的原记录引用、业务 command_id/event_seq/object version/operation ID、model_tool_feedback.record_sha256、逐 ID receipt |

`writer_session` 仅是当前 Store 签发的临时权限，不作为持久历史 token，不进入原 Actor 指纹或
可复制票据。原 claim snapshot 保存 Actor 的持久域；实际调用仍必须由当前 Store 的原 Actor
经过 session 检查。复用同一 request/task 名不能续旧 Actor 的权限。

新 hash 使用明确 `format=n5-event-link-v1` 的规范 JSON 对象和原 `canonical_json/sha256_hex`。
C、S、每个 proof 的字段集合／排序固定；正整数 IDs 去重，重复输入直接拒绝。完整 envelope 的 hash
必须核实际 `call_attempts.request_envelope` 原文本；旧 reservation、settlement、feedback hash 原义不变。
事件来源 hash 核原 events payload/result 的实际持久字节，不以现在对象重新序列化代替历史正文。

## 推荐 B 的最小附加关系

拟议资产 `db/migrations/013_event_call_association.sql`，目标 411。这里仅描述关系及约束，不提供
可导入的迁移 SQL。六个逻辑关系足以表达第一纵向切片；后继 DDL 仍须单独审核。

| 关系（候选名） | 不可变权威／约束 |
| --- | --- |
| `n5_event_requests` | request_id 主键；协议／政策 hash、启用 barrier、原冻结 limits 和终身计数上限。首版仅允许零既有 attempt/ops 的新请求；启用记录不能删掉来绕模式门禁 |
| `n5_event_claims` | claim_id 主键，request/role/actor_task/generation 唯一；完整 C 和上述源项目 snapshot。独立于被 UPSERT 的 `event_actor_claims`，不对后者建立历史 FK |
| `n5_event_links` | link_id 主键，attempt_id、call_id 和 claim_id 各唯一；原 binding/target/S/envelope hash；FK 到原 attempt 和新 claim snapshot；不得 UPDATE 原事实 |
| `n5_event_items` | (link_id,inbox_id) 唯一，保存 S 的原来源身份；FK 到原 inbox 和 link。同一 ID 的明确后续回合可以留多条历史，不能覆盖旧关联 |
| `n5_event_receipts` | (link_id,kind,ordinal) 唯一；RESERVED/DISPATCH/SETTLEMENT/CLOSE/RECOVERY 等 typed 原字节和 hash；CLOSE 含 C 中每个 ID 的独立判定／证据；同 kind 的冲突重放拒绝 |
| `n5_event_open_items` | inbox_id 主键、link_id FK；仅是未决所有者投影。写入与 link 同事务，释放与逐 ID receipt 同事务；每次消费核不可变事实，投影丢失／冲突一律 Corrupt/HOLD |

权威事实使用只追加约束；不支持 UPDATE/DELETE 历史、级联清除证明或任意宿主 SQL 导入。
开放 ID 投影不是业务成功，也不能在恢复时因“现在没有 future”而清掉。新请求模式、当前 Store
scope、原 request/role/actor_task 和全部 FK／唯一性同时在事务中检查，不只检查 inbox_id。

恢复状态由有限种 receipts 求出，不需要把当前 `event_inbox` 或 `call_attempts` 枚举改成新值。
`HOLD_MISSING_RESPONSE/SETTLEMENT/FEEDBACK/PROOF`、`HOLD_UNKNOWN`、`INVALIDATED` 是附加解释；
不能把 UNKNOWN 改写成 FAILED/SUCCEEDED。重复打开只读已有 hold，不无限追加相同恢复事实。

## 拟议接口与 T0–T4

接口只向受信 owner 暴露 typed 值；模型输入不能提供 claim/attempt/目标/CAS 或完成 IDs。
旧 API 签名默认行为保持，启用新模式的请求必须在相应旧路径返回明确 Unsupported/NotReady，
禁止另一个同步 runner、原 `reserve_attempt` 或直接 `finish_events` 绕过新关联门禁。

| 拟议操作 | 单一短写事务内必须发生 | 事务外／失败边界 |
| --- | --- | --- |
| T0 `claim_event_batch(actor,max_ids)` | 原 claim 授权／scope／generation 操作，保存 claim snapshot；同 actor 一份 ACTIVE；只选择未被未决 link 围栏的 IDs | 先有内存结果槽、可用脚本和计数容量。无可用工作不写空 snapshot；缺脚本不预约 |
| T1 `reserve_event_call(captured_event_call)` | 复用原预约所有校验及预算递增；核 C／S／目标／claim ACTIVE；保存原 attempt、link/items/open projection、RESERVED receipt 与收尾预留 | 全部失败回滚，不能出现“有 attempt 无 link”或反向半写。精确 replay 同一个 link，不新增预算／计数 |
| T2 `claim_event_dispatch(original_actor,link_id)` | 核原 link 与当前原 Actor/CAS/完整 binding、claim 和请求模式；复用 `claim_offline_call` 唯一事件，追加 DISPATCH receipt | 只有首次返回 true 才通知 callback；重复 false、旧 session／epoch 不执行。commit 后崩溃也不再派发该 attempt |
| T3 `settle_event_observation(link_id,original_response)` | 原 response/call/attempt/envelope 核对；原 `settle_model_attempt` 不可变结算与 nullable 账目；追加 SETTLEMENT receipt | callback 只返回原响应，不访问 Store。持久失败保留内存原件／结果槽并触发 coordinator settlement_halted |
| T4 `commit_event_action_and_finish(original_actor,link_id)` | 再核当前原 binding/目标/CAS/claim；应用受支持数据库内 action，记录原 feedback、每 ID receipt 与 `finish_events` 子集和投影释放，**同一事务提交** | 没有模型等待／callback／文件或进程调用；异常回滚整个尾部。不能把 future Ready 或 accepted=1 作为证明 |
| `event_link_status/recover_event_links` | 同一读快照核原表／receipt／inbox 状态；需要当前 owner 的收尾另开独立写事务 | 只读结果不授权调用；旧 Actor/claim 不复活；缺原件保留 HOLD，不猜补成功 |

T1 被预算／容量／输入校验拒绝时，owner 仍需用原空完成集释放该次 T0 claim，保留 claim snapshot；
不把拒绝隐藏为成功，也不预留一个永不处理的活跃 claim。若释放本身失败，记录并保留恢复待办。

原公开 Store 方法各自持锁并开事务。后继实现要抽取受审的 `Impl::*_in_transaction` 步骤，沿用
原 authorization/replay/CAS/预算/容量/路由，旧公开 wrapper 仍开原事务。不能在外层 BEGIN 后
直接调用公开 `reserve_attempt/prepare_operation/start_operation/finish_operation/record_tool_feedback/finish_events`，
也不能省略其中一项 guard 来避免嵌套。T3 已有 `settle_model_attempt(...,existing_transaction)` 可作为
提取模式的参考，不代表其他步骤已可组合。

T4 首版仅支持 `apply_independent_plan`、`issue_instruction` 和数据库内 note；ack 由 owner 调用
原 `acknowledge`，再捕获原 instruction/ack 的具体 Worker inbox IDs。note 的 prepare/start/finish
三个原状态推进和原 events 全在这次事务中，仍扣原工具调用数、检查 applied tuple 和 UNKNOWN。
自己的 plan/instruction/CAS 正常推进被记录为显式 pre/post 过渡；不能把别人或控制更新后的当前
binding 刷回 captured 值。支持外的模型 action 结算后记录明确拒绝，保留未完成 ID。

现有 `OfflineRuntime::accept_captured_response` 仍保持原跨事务语义。新模式需要显式接入新尾部，
不能在运行旧 accept 后再补一个 link/receipt 来声称原子。业务验证错误须在回滚后用独立的错误
receipt/feedback 路径保存原失败，不能把失败异常当成可确认 ID；存储错误则保留原响应并停新业务。

## 逐 ID 完成规则

令 C 为原完整捕获集，S 为目标候选子集，D 为可证明已处理、I 为可证明已失效。只有 D∪I 可
进入原 finish 的 completed_ids；每个 ID 都必须独立满足 request/role/subject/源事件与原目标的关系。
所有 C 中项目都有一个不可变处置；claim 期间新到 C_new 从未加入 C，不能被 T4 修改。

| ID 判定 | 最小证据；不是单个 success 布尔值 |
| --- | --- |
| 计划根／AMEND subject | 原 plan/需求与本次实际 plan/处置命令；subject 精确映射、原 task CAS／team epoch；遗漏 B 不以 A 的处置完成 |
| instruction/ack/note | 原说明／ack 的 applied tuple，原 note 的同 binding、business_key=attempt_id、arguments hash、SUCCEEDED op 和原结算／反馈；另一任务同文 note 无效 |
| question/rejection | 原 question ID、holder/status 与原 transition；回答只是该问题完成，原 rejection 必须另核已纠正／合法升级／耗尽，不吞缺脚本唤醒 |
| 明确语义失效 | 原变更 command/hash 和精确旧目标失效依据，说明替代的新待办在哪里；取消／stop 本身不是业务效果撤销或进程退出证明 |
| 未选中 | C\S：NOT_SELECTED，回 PENDING，不借用别的目标结果，不创建虚假成功证明 |
| 选中但本次明确未触及 | 例如 partial AMEND 的 B：原模型 decisions 未含 B、事务证明 B 未改变且仍待办；DEFERRED_NO_EFFECT，回 PENDING，允许后继新捕获／正常预算调用 |
| 选中但无法证明 | 缺 settlement/feedback、业务异常中间事实、UNKNOWN：HOLD，ID 仍 PENDING/可见但 open_items 围栏禁止自动重派 |

D/I 判定与 finished 子集同事务持久；CLOSED replay 返回原处置，不再执行业务、不因后来对象变化
改变完成集合。旧关联只可由显式新 owner 的证据收尾处理，原 binding 不变。它不能把新 claim 里的
后到 C ID 批量确认。T4 正常路径已原子保存 DONE；恢复后无需复制旧成功效果到新 claim。

## 崩溃、失败和控制

T4 使“新模式业务已持久但原反馈／确认尚未持久”的窗口在数据库提交层不可见：要么全部在，
要么都不在。T3 settlement 可独立存在；T4 回滚后**不自动再次应用已结算响应**，保留原响应和
HOLD 交显式处置。原旧路径曾出现的业务已提交／反馈缺失仍作为历史事实保留，不能迁移时合成证明。

首版不承诺崩溃后总能继续业务，只承诺不重复已可能发生的效果。callback 已获得 T2 权限但还没
进入 adapter 就崩溃，与执行后丢响应无法区分；两者同样不重派。T2 未提交的保留 attempt 也不在
恢复中续用旧 Actor；保守 HOLD，允许未来单独审核的明确放弃／新调用政策，不赠送免费重试。

AMEND/cancel 沿原 coordinator 控制队列，原 Store 事务成功且推进当前版本才 stop 原旧绑定。
精确重放仍通过原授权，但不能 stop 新调用。T3 可保存迟到原响应；T4 再查当前状态拒绝旧业务。
取消不是进程退出证明。owner close 停准入、请求 stop、收集或保留响应并 join，最后才释放 Store；
外国线程析构不写 Store。有限原件重试只写原证据，不能重跑 T4 或 adapter，也不清 business fence。

## 容量与收尾预留

新模式冻结的上限建议如下，均可更小，不可扩大原请求冻结值：

- 64 个全局 outstanding/result 槽和 16 个独立 control 槽；普通准入扣管理／Worker 两种 reserve，
  原 4/8、1/1、A4 resource FIFO 及 Ready/Retained 持有保持；一 actor 同时一 claim/call。
- 每请求 `max_links=min(64,原 max_attempts)`、`max_claim_snapshots≤128`，均为终身计数，
  不因 poll/reopen/capacity checkpoint 清零。每次显式批次调用上限仍 1..64、每任务 Worker 1..16；
  总请求预算／管理 wake 上限继续生效。无脚本或无结果槽不先批量 claim。
- C≤64（比现有 Store API 256 更紧），S 非空且 S⊆C。target 原字节≤1 MiB，claim metadata
  snapshot≤64 KiB，新增单 receipt≤64 KiB；不复制完整 request/response 到每个 ID。
  引用原 attempt/feedback 及业务事件的原字节/hash，旧 1 MiB context／4 MiB 原件限制不扩大。
- 每请求新增关联事实的实际编码字节与未消费预留合计≤16 MiB；只统计新增元数据／证明，原
  attempt/response/feedback 自己的既有容量继续单列。该新限额是更紧的终身停止边界，不会
  因 checkpoint 清零，不能把“每行有上限”误当作全库总量有上限。
- 每 link 最多 8 个 typed receipt、最多 64 个 member，拒绝重复恢复造成无限事实增长。
  准入预留关联元数据、最大收尾 receipt／ID 证明所需逻辑字节；无法容纳在原预算前拒绝。
  实际数据库磁盘占满仍可能失败，不能宣称 SQLite 配额等于物理落盘保证。

原 `store_capacity.inc` 的已知事件分类不认识新类型，且已预留控制尾部只针对既有调用／操作。
新表不是免费容量：T1 须另外预留从 T2 到 T4 的原事件尾部和新事实空间。最小 action 的原事件
上界为 T2 一条普通首次派发，plan/instruction 各一条普通业务事件，note 两条普通 prepare/start
加一条 control finish；另有 T3 settlement 和 T4 feedback 两条 control。原 reserve 本身普通计数照常。
ack 的原 control 事件在 owner ack 时检查；不能靠后来 settlement 空间准入新业务。

实现时为每个原计数维度计算“实际用量+其他未完成预留+本次最大尾部≤原 limit”，T1 原子取得预留，
后续每步只消费一次并核原 guard；旧 pending_calls/pending_ops 预留要扣除同一 link 的重叠，不能重复
计费或虚假释放。最终证明／错误收尾只能花自己已预留的有限位置，不成为任意 maintenance 绕道。
逻辑空间、原事件 business/control 池、预算和模型次数分别核对；容量故障用例见故障 oracle。

## 410 兼容、迁移与回滚

1. 旧 `schema.sql`、004–012 SQL、旧模型 Schema、hash／指纹保持原字节。013 单独注册其 hash、
   元数据、资源 catalog/manifest，并在未来版本显式支持 411；这类生产改动当前没有实施授权。
2. 旧 402–410 显式前缀 API／同步行为不变，新驱动要求完整 411；只读旧库不升级。带数据的结构升级
   仅显式 all-request init，在原独占生命周期锁、WAL/FULL 与逐步迁移事务内执行，缺资产／错 hash 拒绝。
3. 410→411 只加空事实结构，不从历史 note/attempt 猜 inbox 关联，不改旧请求快照。旧请求默认 legacy；
   首版启用仅全新无既有调用／操作的请求，初始 admit wake 在启用 barrier 中精确登记。
4. 旧 aa2 程序对 411 在 `user_version` 校验处 Unsupported；这是预期兼容拒绝，不能把 pragma 改回 410。
   新程序的同步路径对已启用的新请求也明确拒绝，避免借旧公开 API 绕 proof 门禁。
5. 迁移失败回滚本步，410 原数据／hash 保持。发布前在独占维护下取得经核对的 SQLite 一致性备份；
   不直接复制活动 db 而忽略 WAL。迁移已成功但尚无新业务时可停止新版本并恢复该原快照。
6. 一旦有新 attempt／效果，不能用旧快照静默丢弃它们，也不提供删除新表／downgrade SQL。
   回滚执行面仅停新准入，保留 411 的读取、证据和受控收尾；要回旧程序，需另审有完整新事实的
   导出／补偿／重新迁移方案。没有获准方案就保留阻塞，不声称无损降级。

原 410 历史样本应验证可读、legacy 默认不变；新样本应验证旧程序拒绝且拒绝前未改业务。
结构迁移不等于接管所有请求，也不增加第二 Store writer 或跨请求执行权限。
