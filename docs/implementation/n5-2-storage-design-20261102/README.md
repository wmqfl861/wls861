# N5-2 存储选择包 v1：三节点汇合与独立审核

状态：**DESIGN_PROPOSAL / PRODUCTION_NOT_STARTED / ACCEPTANCE_NOT_RUN**。
本包是协调者要求的具体设计准备，不是生产实现授权。设计源基线为 PR16
`451b232a0dde08220a5e096d91209087b027b42e`，所在分支
`design/n5-1-persistent-events-20261102-csj777e9`；本包使用独立文档工作树。
已有 N4-B 和 N5-1 提案保留原字节；本包不追认其中尚待批准的选择。

协调者已批准 N5-1 的 **013 / Store 411** 事件关联方案。该批准不包括下面提出的
**014 / Store 412** 图事实，也不包括后续独立版本的审核票据／预算迁移。
PR16 本身是设计基线，不是 411 运行时代码和测试完成的证明。
本包不新增 SQL、Schema、C++ 接口或测试注册，不移除 `independent_nodes` 和
`accept_candidate` 的现有 Unsupported 守卫。

## 可审查的推荐组合

| 决定 | 推荐 v1 | 取舍与批准边界 |
| --- | --- | --- |
| D1：图历史载体 | 新增版本化 graph/node/edge 事实及独立投影；原 Plan 字节与 hash 同时固定；首片仅三个 team_task、两条必需边 `a → c ← b` | 不覆盖无 plan_revision 的 `task_deps`。代价是新增受限事实和逐边证明；一般 DAG、可选失败策略另卡 |
| D2：依赖授权 | 原始 ACCEPTED 事实与当前派发凭据分开；T2 与 T4 各核完整图／两条边；恢复使旧凭据失效 | 原 ACCEPTED 字节可保留有效，但恢复后须由当前 Dispatcher 显式重捕获、证明原恢复连续性，不能凭相同 task ID 刷新许可；见 [graph-contract](graph-contract.md) |
| D3：候选载体 | 新的 `candidate_artifacts_v1` 数据库内容对象与 `candidate_submission_maps_v1`，typed `art_` ID 映射原 submission；不伪造历史 `artifacts.path` | 第一阶段候选完全来自原受控提交快照；后续票据外键指向新对象，不使用历史 review_tickets 的不完整 producer 字段；见 [review-and-budget](review-and-budget.md) |
| D4：跨请求审核权限 | 同一 owned Store 内显式登记一个 producer/reviewer 对；同 tenant/project/root、不同 request/team；专用、短事务、方法受限的双边 host 授权 | 需要后继单独增加 bounded scope。OneRequest 不扩权，现 AllRequests 不成为生产模型跨请求授权；保留数据库独占写者 |
| D5：父预算 | 原各请求账本保持只解释自身 attempts；追加父级授权／归属／借记账本，同一 T1 原子检查和记账，两边 UNKNOWN 联动阻断 | 推荐实际调用逐次借记；不做父原账本伪 attempt，不给 child 新增无关联总预算。保留 escrow 作为被比较的后继选项 |
| D6：版本与落地顺序 | 先 014/412 图与候选映射；独立评审通过后再新增票据、双边 scope 与预算迁移（编号未分配） | 013 不混入后继表；真正旧 411 ELF 打开 412 必须在副作用前拒绝，不能只测新程序 target=411 |

以上六项可以作为一个最小决策集接受或逐项退回；文档作者不自行宣布设计获准。
其中 D2 是新的图恢复合同，D4/D5 是现有 Store 尚不具备的能力，不能当作已有 API。
所有数值上限须随模式启用一次冻结；本包建议的有限上限不自动修改现有请求配置。

## 分片和前置

第一片 G：只在新准入且明确启用图协议的请求上发布三节点图。a/b 通过原真实提交、TL 检查、
Dispatcher 接受后，c 才能获得可复核的两条依赖凭据。无 review 配置；依赖验证进入 Store 的
预约、唯一派发和业务提交边界。G 需 N5-1 的原 T0–T4 持久关联、控制和 UNKNOWN 合同已验证。

第二片 R：只登记一个离线审核映射，最多一张活动票据；固定原候选，覆盖 pass、
changes_requested、inconclusive 和轮次耗尽。包含 D4/D5 的完整事务实现和独立审核，
不能只加模型 Schema 或调用 WorkClass::Review 就宣布完成。R 不授权 live、进程、MCP、
通用 capability、跨宿主请求或用户豁免。

第三片的执行相位连接另需具体批准：N5-1 首片没有 file executor。真实候选沿用既有
WorkerTools／同步 workflow，在证明原 owner、drain、claim、预算交接之后才能接入；
callback 不访问 Store，不向同步 runner 重交同一 attempt。不能以 SQL 插入 ACCEPTED
行代替纵向路径。图／票据组件通过不代表该执行相位连接已经完成。

## 文档与事实来源

- [图事实、状态机和 T2/T4](graph-contract.md)：明确事实键、CAS、恢复和所有公开入口的守卫。
- [候选、双边审核与父预算](review-and-budget.md)：格式、最小接口、失败与借记守恒。
- [迁移、回滚和验收](migration-and-oracles.md)：旧程序拒绝、原子故障点、逐事实查询。
- [输入清单](source-inputs.sha256)：设计基线中实际参考文件的 SHA-256；不是本包自哈希。
- [复核补充输入清单](review-source-inputs-v2.sha256)：同一451基线的实际账本／未结算检查与参考审核verifier；原21项清单保留。
- [本批验证报告](../../reports/n5-2-storage-design-validation-20261102.md)：实际文本检查、未运行项和提交前时域。

本包承接 [N5-2 依赖](../n5-persistent-events-20261102/n5-2-dependencies.md)、
[N4-B](../next-cycle-20261102/02-dag-review-tickets.md) 和
[ADR-094](../../adr/094-persistent-event-call-association.md)。
根 AGENTS、已发布 v0.4.1、原恢复／验收／结算合同仍是约束；源代码实际边界与未来设计分别陈述。
