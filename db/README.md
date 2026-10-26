# v0.4.1 SQLite 契约与可执行事务参考

`schema.sql` 是修订蓝图的目标数据库结构。`contract_store.py` 用真实 SQLite 事务验证审核发现的状态、版本和恢复边界。下文主要描述这个 Python 参考，不将它当作运行器。C++ `Store` 已在 S1.1 接入，S1.2 新增真实 Worker 工具与 403 迁移，S1.3 增加持久事件运行器与 404 迁移，S1.4 增加问答、提交快照与团队检查的405迁移，S1.5增加当前验收/交付/回执/经验的406迁移；原M1仍保留独立Journal。S1.6增加407受控workspace删除阶段；R2a.2增加408分配恢复和明确弃置。MemoryEngine、跨功能独立审核、Git Writer和私有数据purge仍待实现。不要把本模块注册成面向模型或网络调用方的工具服务。

## C++ 运行版本：402–408

`src/store.cpp` 及其 C++ 回归与 Python 参考分别验证。004 将已知空 401 参考库升级为 S1.1 的 402；005 将包括非空请求的 402 原地升级为 S1.2 的 403。每份迁移指纹须匹配，失败在事务内回滚；原 schema/004 字节保留。缺完整配置快照的非空 401 仍拒绝自动升级。

403 新增不可变团队 workspace identity/execution policy、operation_results 和原始输出 BLOB。真实工具使用唯一首次派发 claim，原 tuple 结果与事件同事务保存；旧结果不自动解除 UNKNOWN。控制数据库/租约/WAL/SHM 不得位于本 Store 的业务 workspace 中。C++ ReadOnly 查询不迁移或接管，403 打开仍需已分发的 005 资产。详细语义见 [Worker 工具契约](../docs/implementation/S1-worker-tools.md)。

006 将已知非空 403 原子升级到 404，新增逐角色/任务主题 `event_inbox`、捕获具体项的 `event_actor_claims`、`worker_waits` 和不可变 `model_tool_feedback`。领取不算消费，明确完成的子集才标 DONE；遗漏、新到项和重启释放的项继续待办。独立计划及初始团队分配在一个事务提交。迁移保留原请求/快照/操作/输出/账本，原 005 字节也不改变。404 打开要求相应分发资产，原接口可以继续打开402/403；ReadOnly仍不升级。详细边界见 [S1.3 契约](../docs/implementation/S1-event-runtime.md)。

007 将已知非空 404 事务升级到 405，复用原 questions 并增加原 binding/状态/等待代次和不可变转发/回答记录；workflow_submissions 保存原提交与完整文件快照，team_lead_checks 保存检查和返工，team_workflow_state 保存冻结上限下的返工计数。快照严格核字节、规范 base64、SHA 和路径覆盖，状态/事件与快照同事务提交。纯 epoch 恢复保留语义目标；AMEND 或语义版本改变使旧工作流失效。TeamLead 检查通过不等于业务验收。原 schema/004/005/006 不改；405 打开需匹配分发的007，ReadOnly仍不升级。完整边界见 [S1.4 契约](../docs/implementation/S1-workflow-runtime.md)。

008将已知非空405事务升级406；原schema/004–007保留。独立Dispatcher验收与experience_job同事务，报告保留原source快照和哈希；专用经验调用可在业务终态恢复，普通业务门禁不开放。交付先保存frozen intent，再由POSIX模块发布原字节目录，最后记录STORED；精确ACK与DELIVERED、留存receipt、cleanup_job同事务。取消也保存独立收尾记录，UNKNOWN仍需核清。S1.5时点只读查询不产生回执，尚无实际清理；S1.6新增范围见下一段。完整字段、容量与信任边界见 [S1.5契约](../docs/implementation/S1-acceptance-delivery.md)。

009将已知非空406事务升级407，保留schema/004–008原件。新增owned_cleanup_metadata、owned_workspaces与workspace_cleanup_runs：创建意图、完整identity、固定inventory和删除proof首次写入后不可变，更新必须推进CAS。资源登记由Root受信宿主执行，不是模型工具。cleanup_jobs使用领取代次和state_version围栏旧执行；workspace阶段DONE后全局作业仍PENDING，私有数据、原交付和所有权收据保留。默认cleanup false；旧库ReadOnly不迁移，407打开要求相同009哈希。详见[S1.6契约](../docs/implementation/S1-owned-cleanup.md)。

010 将已知非空 407 事务升级 408，逐值保留原所有权记录和清理 manifest。新增不可变 allocation_receipt、abandonment 与 ABANDONED 状态；有效资源按团队部分唯一，原请求/命令键永久唯一。新分配保存完整代次收据后才无覆盖发布；弃置只保留元数据和路径，不授予删除权。每请求 64 个有效资源、256 条终生历史，原窗口不能重置。CLI 提供 010；原嵌入调用省略追加的资产参数仍保留 407。带请求的结构升级只允许显式 AllRequests/init，OneRequest/AdmissionOnly 在迁移前拒绝，只读不升级。010 的 DDL/复制/版本同事务，但之后请求恢复是独立事务；后者失败不撤销已经成功的结构升级。408 打开要求相同 010 哈希。详见[R2a.2 契约](../docs/implementation/R2a-allocation-recovery.md)。

## 运行与数据库版本

以下章节仅描述 Python `contract_store.py` 的事务参考；C++ 运行器当前行为以此前的402–408说明和对应实现契约为准。

```sh
/usr/bin/python3 tests/contracts_tests.py
/usr/bin/python3 scripts/validate_contracts.py
```

测试使用临时数据库和合成输入。测试解释器需安装根目录 `requirements-test.txt` 的 `jsonschema`；SQL 参考只依赖 Python 标准库。本地已验证 SQLite 3.45.1。目标 SQL 使用 SQLite 3.37+ 的 STRICT 表、JSON 函数和 FTS5/trigram；部署前必须检查实际 SQLite 构建的功能。

每个连接，包括重开后的连接，都设置 `foreign_keys=ON`、`journal_mode=WAL`、`synchronous=FULL`、`busy_timeout=5000`。FULL 是默认可靠性配置；本仓库没有以吞吐为由自动降级 NORMAL 的逻辑。文件系统、硬件同步和部署故障仍需 S1 集成验收，单个临时数据库测试不证明掉电后的整机可靠性。

`schema.sql` 在一个事务内初始化空数据库，标识为 `contract_metadata.schema_version='0.4.1'`、`user_version=401`，迁移记录 `version=3` 表示修订契约的统一初始化。它不是针对已有 v0.4 业务数据的自动迁移脚本。`ContractStore(..., initialize=True)` 遇到已有表就拒绝；普通重开校验版本。原始 v0.4 DDL 保存在只读交接档案；后续部署如存在旧库，必须单独设计保留数据的迁移与回滚，不能删除旧库再“升级”。

## 宿主权威与短事务

`Actor` 是测试宿主签发的调用上下文，不是认证凭据。`actor()` 只是合成夹具的上下文工厂，不能向非可信调用者暴露。外部身份认证、凭据签发、租户命名空间和生产 ACL 属于未来适配器。当前参考校验 Root 的 `principal_id=root_id`、唯一 Dispatcher 身份、请求归属、TeamLead/Worker 主体与团队归属，以及 Dispatcher/执行代次。实例 ID 由主体和代次组合，恢复后的新实例不能冒领旧实例加载记录。

请求修改由用户经 Root 发起；Dispatcher 只能在当前修改中处置任务。`amend()` 拒绝 Dispatcher 与外部 Root。任务分配夹具不产生 TeamLead 说明：首次业务执行也必须经过 `issue_instruction()` 与 Worker `acknowledge()`。

所有变更用 `BEGIN IMMEDIATE` 短事务完成，事务里无网络、模型、命令或 Git 操作。读取权威行、检查调用起点的冻结版本、执行 CAS、写关联状态和事件在同一事务内完成；任一失败回滚整个事务。SQL 外键/唯一键/CHECK 是结构约束，跨字段业务语义与权限由宿主执行，不能把“SQL 可插入”理解为已授权。

| 记录类别 | 并发与更新契约 |
| --- | --- |
| requests、tasks、teams、ops、questions、change_orders、llm_calls | `state_version` CAS；应用提交 `WHERE state_version=expected`，成功递增一次 |
| runtime_policy、memory_meta、memory_feedback、receipts、experience_jobs、cleanup_jobs | 同样 CAS；撤权还原子递增 `policy_epoch`，待办领取另递增 `lease_epoch` |
| request_texts、request_constraints、plans、instructions、amend_dispositions、events、artifacts、review_tickets/results、verification_runs、memory_loads、memory_versions | 不可变历史；更新触发器拒绝原地改写；合法清理由宿主的聚合事务负责 |
| task_deps、memory_error_signatures、memory_fts | 派生集合；计划/版本聚合事务内整体检查或重建，不能单独接受 Agent 行级写入 |
| notes | 追加笔记；更正追加新笔记，不能把已有证据原地改写 |

所有可变表都有“忘记递增 state_version”触发器，但触发器不能替调用者提供正确的预期版本，也不是用户权限系统。参考中的跨行团队暂停、操作围栏和回执更新归属于已锁定的请求/任务/待办事务；不能拆成独立无 CAS API。恢复、撤权、经验完结、清理完结是 **宿主专用**入口。

## 版本、AMEND 与控制操作

`requests.requirements_revision` 标识原文及 AMEND；`plans.plan_revision` 和 `based_on_requirements_revision` 标识计划针对哪一版需求。任务分别存 `task_revision`、`plan_revision`、`instruction_revision` 及三项 `applied_*`，不再用一个 applied_rev 同时表示所有层次。

Dispatcher 的 AMEND 处置持久化为 `amend_dispositions.disposition=update|keep|cancel`。管理工具的 `action` 参数映射该列。`set_amendment_disposition()` 必须传入决定发起时冻结的 `based_on_requirements_revision`、`target_plan_revision` 和 task CAS，不能从响应到达时的当前行补成新版本。遗漏的任务保持暂停；`keep` 也增加任务修订、绑定新需求/计划，由 TeamLead 发出新说明并由 Worker 确认。仍在运行的请求内，先前 ACCEPTED 的任务可以显式重评，旧豁免标记随之清除；已经业务终态的请求拒绝重新处置。

`instruction_revision` 是任务级单调高水位；普通说明更新使用它围栏，不递增实例的 `execution_epoch`，因此成功调用的原 Actor/命令精确重试仍能取回原结果。同命令换 body 会失败。实例恢复、替换或 AMEND 围栏才推进执行代次。此参考只覆盖本轮 AMEND 和说明用例；完整 DAG 重计划、团队替换、审批及 Agent 循环仍属后续实现。

`acknowledge()` 是控制操作：校验主体、归属、CAS、当前需求/计划/任务/说明及代次，但不要求待确认版本已经 applied，也不因 UNKNOWN 阻塞。它必须确实存在待确认的新说明，不能用新命令 ID 无限重复确认。确认不会解决 UNKNOWN；新业务仍等待宿主结果核实。取消操作同样独立：PREPARED → CANCELLED，RUNNING → UNKNOWN，不能声称取消撤销了既有副作用。

## 操作幂等、派发与恢复

`ops` 为持久操作记录，业务幂等键唯一于 `(request_id, task_id, business_idempotency_key)`，并绑定工具、参数摘要、副作用类型以及完整的需求/计划/任务/说明、Dispatcher/执行/策略代次。相同键、相同原始意图只能查询同一个记录；参数、工具、版本或团队不同就冲突，不能重新执行。UNKNOWN/终态的精确重试也只返回原记录。

PREPARED 不表示已经执行。开始派发时重新核验冻结版本、当前 applied、状态和工具授权。原始请求冻结的 `tool_allowlist` 与运行时 `denied_tools` 共同生效；`revoke_memory()` 同一事务撤销记忆状态/脚本许可、推进 `policy_epoch` 并记录宿主指定的工具撤销集合。旧 PREPARED 操作无法靠旧许可派发，使用新策略代次也不能调用已撤销工具。生产脚本身份到工具许可的映射、沙箱限制和真正执行前的最后检查仍需未来执行器实现。

AMEND、计划变化、说明变化、请求恢复都会使未派发操作取消、正在运行的操作变成 UNKNOWN。UNKNOWN 包括工作区内的写入，必须凭宿主证据明确结果后才可继续。`recover_request()` 演示持久围栏；构造数据库连接不会自动取得生产运行器租约或恢复全部请求。未来启动流程须在签发 Agent 上下文前完成租约接管、围栏和待办扫描，不能仅依赖唯一 Dispatcher ID 或 SQLite 的写锁来声称进程唯一。

## 记忆版本与独立使用证据

`memory_meta` 指向当前 Git 内容版本；`memory_versions` 保留已知版本索引。`memory_loads` 使用独立 `load_id`，保存请求/任务/团队/Agent 实例、实际 `revision`、摘要及加载方式。同一任务、同一角色可以先后加载 A、B，甚至同版本多次加载，均不会覆盖旧证据。`load_memory()` 对 `global`、匹配当前 `project:<id>`、匹配当前 `function:<id>` 做明确范围检查，未知范围拒绝；这只是参考范围判断，不含完整生产 ACL/检索。

Submission 中的 `memory_id/revision/load_id` 必须匹配该任务和实例的实际全文 `load` 或 `rule` 加载。宿主从不可变加载记录补全 revision，不能读取当前 memory_meta 来重盖章。仅卡片、recall、报错提示曝光不是实质使用；这些记录可用于追溯，不能凭曝光给成功加分。

反馈以 `(memory_id, revision, source_key)` 去重，`source_key` 是宿主按来源分组的键，不强制等于 request_id。关联请求可以共享同组，不能把多个请求自动算成独立证据。宿主还必须验证动作和自足证据；`eligible=1` 才参加评分。同组中有效 failed 优先于 worked，再优先于 irrelevant，重复或较弱评价不会抬高统计。`memory_stats` 按 `(memory_id, revision)` 聚合，`memory_current_stats` 只连接当前精确版本。测试明确证明 A 的五次成功不会传给 B，B 首次失败得到 0 次成功/1 次失败。

`revise_memory()` 同时比较 `expected_base_revision` 和 `state_version`，原子更新当前索引并追加版本行；冲突不覆盖当前内容。新版本的许可清零，旧版反馈保留在旧版本下。该参考不写 Git，冲突只抛出异常，**未实现持久冲突草稿**。未来 S3 Writer 必须在 Git/索引一致性协议下保留冲突提案，进入 draft 或人工合并，不得丢失提案或假称 CAS 已完成 Git 提交。

## 审核与确定性验收

`review_tickets` 保存生产方候选 ID/摘要、提交 ID、各层版本和审核轮次，并保存完整独立的 `reviewer_*` 身份、版本与代次。`review_results` 和 `verification_runs` 是不可变证据。响应先匹配调用起点 ticket，再分别匹配当前生产方和当前审核方；审核团队恢复后的旧回包不能被盖成新代次。

`scripts/validate_contracts.py` 的语义参考要求独立宿主检查结果、runner 标识、证据引用，以及精确的候选/版本/ticket 绑定。模型填写 pass、Schema 校验成功或交付清单的 accepted 标记均不能替代这些证据。用户豁免必须来自宿主已认证用户，并绑定具体问题及候选，且不能豁免确定性检查失败/未执行。参考不执行验收命令，也不认证用户；生产适配器必须从持久证据行和真实执行器读取这些参数，不能照搬模型字符串。

## 持久经验与清理待办

`experience_jobs` 按请求、任务、团队和该团队最后发行的任务/说明版本区分。每行保存来源快照、状态、报告引用、CAS 和租约字段，无 request 外键。多团队各自完成报告；同一团队重新接受 AMEND 后，旧 DONE 报告不能完成新说明版本的经验收尾。历史团队匹配自己的最后一份说明，不要求它冒领替换团队的新版本。未发行说明的团队可记录明确的无新增经验回执。

只有请求业务终态、资源已经停止、没有 PREPARED/RUNNING/CANCEL_REQUESTED/UNKNOWN 操作、每队当前适用的经验待办均 DONE，才能创建 `cleanup_jobs`。DELIVERED 还要求宿主确认交付回执。待办保存交付/清理清单快照及独立 `receipt_ref`，先持久化后才允许清理请求行；它不因 request 的 CASCADE 丢失。请求已终态并不意味着清理完成，恢复要扫描这些独立待办。

领取或重新领取 RUNNING 清理任务会推进 `lease_epoch` 和 CAS；旧工作者不能完成新领取的任务。`finish_reference_cleanup()` **只删除测试数据库中的请求行**，不会检查或删除任何工作区/输入/产物文件。生产实现必须先在当前租约下幂等完成和验证实际清理，再持久化完成状态。回执、经验报告引用和反馈证据保留至其各自明确的保留策略到期；参考不自动运行 TTL 清除。
