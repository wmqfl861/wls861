# N5-2 实施准备清单：D1–D6 决策与分片门禁

状态：**DESIGN_READINESS / PRODUCTION_NOT_STARTED / ACCEPTANCE_NOT_RUN**。
本页落实冻结交接的设计准备要求，不代表协调者已经采纳 PR #21。
**013 / Store 411 已在产品候选实现；014 / Store 412 及后续票据迁移仍未授权。**
不新增 SQL、Schema、Store API、测试注册或启用配置，不移除当前 Unsupported 守卫。

## 输入和当前边界

- 产品输入：[PR #25](https://github.com/wmqfl861/wls861/pull/25)，精确提交
  `c2a5cea64aeea261b214cdfd9a968b53f491110a`。后续执行必须另冻结实际集成 SHA。
- 决策要求：[冻结交接 §6](https://github.com/wmqfl861/wls861/blob/d843493fafe158f3a6443bafaa0de95cbf6c05c2/docs/reviews/coordinator-handoff-n5-20261102/NEXT_AGENT_PROMPT.md#6-n5-2-dag-设计边界)。
- 待审提案：[PR #21](https://github.com/wmqfl861/wls861/pull/21)，精确提交
  `4326ea5004889c4e14b31e26c6b86e6b0a8509b3`，其设计输入为 PR #16
  `451b232a0dde08220a5e096d91209087b027b42e`；不是当前运行实现。
- 当前 [Store](../../include/wls/store.hpp)、[持久关联实现](../../src/store_event_association.inc)
  已有 claim、reservation、unique dispatch、原 settlement、业务 action、feedback、逐 ID finish。
  [PersistentEventRuntime](../../include/wls/persistent_event_runtime.hpp) 仍限定 FakeModel，
  callback 不访问 Store，未提供文件或进程执行器。
- [现有依赖准备](n5-persistent-events-20261102/n5-2-dependencies.md) 和
  [N4-B](next-cycle-20261102/02-dag-review-tickets.md) 保留。
  `independent_nodes` 仍拒绝 depends_on/review/capability/input；`accept_candidate`
  仍拒绝 required review。WorkClass::Review 是容量类别，不是审核授权或 verdict。

以下清单的“验收”均为 **PLANNED / NOT_RUN**。主协调者应逐项记录采纳、退回或缩减范围，
明确绑定提案 SHA、实施输入、容量上限、事务边界和验收 oracle；执行者不代签采纳。

## D1：图版本与不可变历史

- [ ] 决定新增 graph/node/edge 不可变事实和 CAS 投影；保留原 Plan 字节/hash，
  不覆写缺少 plan_revision 的历史 `task_deps`。
- [ ] 首片限三个真实 team_task、两条必需边 `a → c ← b`；无可选边、审核、
  capability 或 input 扩展，保持原最多 20 节点及更紧的冻结 team 上限。
- [ ] 新准入请求显式冻结图协议和容量；提案的图版本、证明数、元数据、映射数上限
  尚待批准，不静默加入旧快照。
- [ ] 原子发布 Plan/节点/边/初始唤醒；环、重复、自环、跨请求端点、超容量、
  失效版本均拒绝，零半写；保留所有原图版本。

代价：新增有界历史、投影重建和逐边验证，不能靠删除现有拒绝条件交付。
依据：[图契约](https://github.com/wmqfl861/wls861/blob/4326ea5004889c4e14b31e26c6b86e6b0a8509b3/docs/implementation/n5-2-storage-design-20261102/graph-contract.md)。

## D2：原验收事实与当前依赖权限

- [ ] 固定原 predecessor task/submission/candidate/hash/acceptance、完整 binding、
  graph 与两条边证明；ready 查询只作观察，不能授权。
- [ ] 在 T1 捕获并验证依赖，在实际 T2 唯一派发和 T4 业务事务再次核验图、
  各边、Actor/epoch/CAS。AMEND、撤销和 recovery 使旧许可失效。
- [ ] 处理原 acceptance 保存“接受前 CAS”、接受成功后 task/submission 各加 1 的语义；
  校验精确接受后 CAS，不使用 `>=`，不从当前状态刷新旧证明。
- [ ] 单独决定恢复后的显式重捕获协议：原 ACCEPTED 字节可保留，权限须由当前
  Dispatcher 核完整 hash-bound 恢复连续性证据；缺失或篡改保持 HOLD。
- [ ] 验收仅 a 已接受时 c 派发数为 0；b UNKNOWN、FAILED、取消分别留原因；
  两边当前有效接受后 c 恰派发一次。就绪后 AMEND/recovery 在 T2/T4 均拒绝旧图，
  原 response/usage/证据保留，旧 Actor 不续权。

代价：完整历史事实不等于当前权限，恢复重捕获是新合同，须独立批准。

## D3：候选内容与 submission 映射

- [ ] 决定采用提案的 `candidate_artifacts_v1` 内容对象及
  `candidate_submission_maps_v1`；typed `art_` ID 映射原 submission/binding/record hash
  和完整有序文件快照，不伪造旧 `artifacts.path`。
- [ ] 从受控提交的原快照构造一次性映射；不读取当前工作目录替代原内容，
  不把 submission_id 当成 artifact_id；相同内容、不同 submission/版本不得复用票据。
- [ ] 保留原序列化快照 524288 字节上限；新增封装/编码另计有界容量。
  内容 hash 不包含可变状态、当前 CAS 或会话。
- [ ] 用原 `submit_work → check_submission → accept_candidate` 纵向事实验收，
  包含内容改动、映射错配、顺序变化、超容量和同内容不同来源负例。
  不以 SQL 插入 ACCEPTED 行代替业务链路。

代价：需要新内容存储及来源映射，而非仅换历史 artifact 外键。
依据：[候选、审核和预算](https://github.com/wmqfl861/wls861/blob/4326ea5004889c4e14b31e26c6b86e6b0a8509b3/docs/implementation/n5-2-storage-design-20261102/review-and-budget.md)。

## D4：producer/reviewer 双身份与票据

- [ ] 独立批准提议的 bounded ReviewPair scope 和 review-only child；它们不是当前 API。
  同一 owned Store、单写者；同 tenant/project/root，且 request/team/principal 分离。
  OneRequest、AllRequests、WorkClass::Review 均不得借此变成模型跨请求授权。
- [ ] 每张票据分别固定双方完整十域 ExecutionBinding、双方 CAS/config/policy、
  原 candidate/submission 映射、review ID/round/max、输入 hash 和 host checks 版本。
- [ ] 任何 recovery 写入前先完成双边授权，再同事务恢复两边；无效配对零 epoch、
  claim、预算副作用。任一身份失效须新票据/新 attempt，不补权限给旧结果。
- [ ] 保留 T3 迟到观察；T2/T4 复核当前资格。producer 自审、同内容不同版本、
  任一方 epoch/CAS/policy 改变、旧票据精确重放均有确定性负例。
- [ ] verdict pass 与确定性 host checks 分开；仍需 Dispatcher 明确接受。
  changes_requested 走冻结有限返工、新说明与 Worker ack；重放不增轮次；
  inconclusive、耗尽、UNKNOWN 和缺 checks 均不自动接受。

代价：新跨请求授权和双边恢复语义；审核预留只保证排程，不替代权限。

## D5：父预算与逐次借记

- [ ] 选择提议的逐 child-attempt 借记，或明确退回比较 escrow；不得先实现后采纳。
  各请求仍只解释自身真实 attempts，不在父账本伪造 attempt，不另给 child 无关联资金。
- [ ] child T1、唯一父借记/link/ticket 同事务；父普通预约也检查聚合占用。
  精确 replay 不再借记、不加轮次，保留原调用、输入/输出、费用及更紧子额度限制。
- [ ] UNKNOWN、nullable usage、原预留全部保留；不重复计数，不用未知费用补零，
  不能自动退款恢复权限。独立核实提议审核模式的五维完整性，包括 cache null/已知 0；
  不改旧 FakeModel 的兼容解释。
- [ ] T3 结算与父投影同事务，迟到事实保留；从不可变账目重建守恒。
  测父普通调用竞争、子超额、事务回滚、commit outcome unknown、重启和迟到结算。

代价：跨请求聚合容量和恢复 oracle；仅新增 child budget 字段不足以保证父预算。

## D6：迁移、兼容和回滚

- [ ] 由协调者明确批准具体 DDL/版本后才新增资产；提案先 014/412 图与候选映射，
  票据、双边 scope、预算属于后续独立版本，编号未分配。013 和发布 SQL 原字节不改。
- [ ] 获准分片同时更新 registry/catalog/manifest、安装和捕获、原生 detached 资源元数据；
  populated upgrade 仍走显式 AllRequests maintenance，不给旧请求静默启用新协议。
- [ ] 使用真实旧 411 ELF 打开真实新库，分别验证 read-only 与 takeover 拒绝及 main、
  WAL、SHM 原字节/身份/权限 inventory；不能用新程序 target=411 替代。
  当前 Compatibility02/03 历史失败不因新设计或有限 sidecar 对照自动关闭。
- [ ] 不提供 DOWN SQL 或手动改 user_version。停止新模式后仍保留新版只读、结算和 HOLD；
  恢复升级前一致备份需单独授权，不能丢弃新业务或证据。

代价：新增版本的旧 ELF 兼容、资源交付和回滚验证，不能只测试空库升级。
依据：[迁移与 oracle](https://github.com/wmqfl861/wls861/blob/4326ea5004889c4e14b31e26c6b86e6b0a8509b3/docs/implementation/n5-2-storage-design-20261102/migration-and-oracles.md)。

## 执行依赖与退出条件

1. **先稳定 N5-1。** 冻结单一候选，保存 T0–T4 原子性、逐 ID/partial finish、
   UNKNOWN、旧 Actor/claim、重启、迟到响应及 close join-before-Store-release 的实际证据。
   本机 sanitizer 和历史失败分别处置；设计就绪或远端绿色不豁免发布门禁。
2. **再取得 D1–D6 明确决策。** 记录批准的分片、数值上限、事务、迁移及测试 oracle。
   未采纳部分保持 NOT_STARTED；本页不能充当批准记录。
3. **首片只做三节点汇合。** 先设计现有 WorkerTools/同步提交与异步 owner/drain/claim/
   预算的执行相位交接；callback 不访问 Store，同一 attempt 不重交同步 runner。
   纵向执行真实提交、TL 检查、Dispatcher 接受和 c 唯一派发；图发布、T2/T4 竞争、
   公开 API 绕过、崩溃/重放负例及适用完整矩阵通过后交主协调者复核。
4. **图分片复核后才做审核票据。** 先批准独立存储版本、双身份和父预算，
   再完成 pass/返工/inconclusive/耗尽、双边恢复和满载审核容量的实际闭环。
   不扩展 live、通用 DAG、记忆、MCP、服务化或 Windows 强沙箱。

本页只作文本范围、引用和 diff 检查；没有运行 DAG/票据产品测试，也不声称相关
IMPLEMENTATION_COMPLETE、LOCAL_VALIDATION_COMPLETE 或正式 PR_REVIEW_COMPLETE。
正常推送、远端 CI 和实际合并状态由最终交付另记；本轮不合并 PR/main。
