# S1 基础实现契约（0.2.0-s1.1）

本文件保留 S1.1 基础层的版本范围。当前 `wls 0.3.0-s1.2` 新增的文件/进程入口、工具循环、SQLite 403、控制和结果语义见 [S1.2 Worker 工具契约](S1-worker-tools.md)；原 S1.1 报告和日志仍对应当时版本。

更新：2026-10-17。目标蓝图仍为 [v0.4.1](../spec/v0.4.1/01_实施规格_v0.4.1.md)，承接提交 `e78cbfb4acf45af73e9edc1ddb014d1bba7a909d`。本文件描述已接入的 C++ 基础层；完整 S1 产品闭环仍按 [任务卡](../spec/v0.4.1/07_S1任务卡.md) 实施。实际结果见 [本批验证报告](../reports/s1-foundation-validation.md)。

## 1. 两个运行入口

| 入口 | 当前行为 | 持久化 |
| --- | --- | --- |
| `wls861 demo` | 原 0.1.1-m1 内核的合成请求、审核、回执、资源状态流程 | 原 Journal，格式和回归保留 |
| `wls` | 新 0.2.0-s1.1：严格配置、Schema、请求准入、单任务 FakeModel、说明确认、持久 note、AMEND、查询与取消 | 真正的 C++ SQLite Store，user_version=402 |

`wls_s1` 复用现有身份和配置解析类型，另建明确的 Store 与离线运行器。它没有把 M1 的全部候选、审核、交付和资源状态 API 自动搬到 SQLite。Python `scripts/validate_contracts.py` 帮助程序继续验证参考契约，不能用来宣称未接入的 C++ 模块已经运行。

本批不创建守护进程、外部身份认证、网络供应商请求、真实文件／进程业务工具、沙箱、版本化交付包、记忆或清理执行器。单任务的 note 成功后，request/task 仍为运行状态，`business_delivered=false`。

## 2. 模块与依赖

| 接口 | 实现职责 |
| --- | --- |
| `runtime_types.hpp` | 类型化错误、严格 JSON、内部规范序列化、SHA-256、ULID、完整执行绑定 |
| `config_loader.hpp` | LibYAML 加载、引用与政策检查、不可变配置快照、还原与字段来源解释 |
| `schema.hpp` | Valijson Draft-07，封闭本地引用、完整记录与模型输入投影、嵌套 TaskBrief 验证 |
| `store.hpp` | SQLite 迁移、身份／版本／幂等／预算／UNKNOWN 的事务关口和只读查询 |
| `context.hpp` | 根据实际文本和依赖编排稳定前缀、完整动态契约、缓存指纹和大小上限 |
| `model.hpp` | ModelRequest/Response、nullable usage、独立 attempt、可取消 FakeModel、完整响应检查 |
| `offline_runtime.hpp` | 将上述模块串为受信宿主流程，先记账，再校验并提交业务变更 |
| `src/cli.cpp` | 本地命令入口、明确参数、JSON 输出与错误码 |

C++20 / Linux；系统 Boost 1.83+、SQLite 3.37+、OpenSSL 3+、LibYAML 0.2.5。Valijson v1.0.6 和 LibYAML 公共头保持固定上游字节，来源、许可证与 29 文件清单见 [third_party](../../third_party/README.md)。没有构建期 `FetchContent` 或模型调用。

## 3. Store 的信任边界与事务

`StoreActor` 由受信宿主从已持久化的 Root／Dispatcher／team 所有者构造，绝不从模型参数反序列化。CLI 是本机数据库所有者的管理入口，不是已有多租户远程认证系统。Store 再检查每次动作的角色、请求、团队、主体及当前代次；模型输出只提供业务字段。

| 动作 | 执行身份 | 关键关口 |
| --- | --- | --- |
| admit、AMEND、cancel request | 本机准入宿主／Root | 入口作用域、完整快照、原文不可变、幂等内容指纹、请求 CAS |
| publish plan、create task、disposition、rebind | 请求所属 Dispatcher | 请求需求、计划、任务归属、已登记约束、版本 CAS |
| issue instruction | 当前 TeamLead | 当前任务／计划／需求、全部适用约束、独立 instruction_revision |
| acknowledge | 当前 Worker | 确有待确认说明；原说明的任务、计划、需求和执行代次一致 |
| prepare/start/finish/cancel operation | 当前 Worker | 准备与派发分别检查当前完整绑定、说明应用、授权交集、预算及 UNKNOWN |
| recovery、reconciliation、raw usage settlement | 受信宿主 | 保留原执行事实；核对原 tuple、独立事件和 CAS；不作为模型业务工具注册 |

Store 使用短 `BEGIN IMMEDIATE` 事务：状态、版本、幂等结果与事件一起提交。模型等待、Fake delay、上下文编译和 Schema 校验不占 SQLite 事务。已登记约束在提交计划／TaskBrief／WorkOrder 处再次核对，避免结构合格却漏掉当前约束。

配置完整性在准入时检验，function 必须匹配；团队模板必须存在。Worker 工具授权取已冻结模板工具、请求允许列表与当前撤销政策的交集。模型预约再检查冻结角色选择、模型能力、原输入和预算，不能靠绕开 CLI 扩权。

当前只实现部分 S1 状态路径。全局 Dispatcher/Worker 双生命周期配额、持久事件消费与完整 DAG 调度仍待后续；现有 M1 Scheduler 的有界线程、队列和审核预留继续独立回归。S1 单次运行器只串一个独立任务，不把 DAG 存储等同于 DAG 执行。

## 4. SQLite 版本与恢复

空数据库先执行已知 `db/schema.sql`，然后执行 `db/migrations/004_s1_store.sql`，进入 `user_version=402`。Store 校验 SQLite 特性、WAL/FULL、外键、迁移元数据、数据库完整性与外键一致性。迁移脚本指纹持久化；未知版本或不一致直接拒绝。

已知的 401 参考数据库只有在没有请求数据时才允许升级。非空旧参考库不具备本批所需的完整配置快照，不能臆造；Store 明确拒绝并保留数据。Journal、未知 SQLite 文件或坏数据均不被自动改写为新格式。对非空生产数据的迁移、备份和恢复政策需要后续独立实现。

同一数据库只有一个进程持有写者租约。Takeover 打开时先围栏活动请求，递增 Dispatcher／团队执行代次、暂停继续执行，使未决调用进入 UNKNOWN；在途副作用操作也保留 UNKNOWN，Prepared 操作不会因重开而盲目执行。

ReadOnly 打开不持写者租约、不迁移、不围栏、不增加代次。`show/events/budget/config-snapshot/pending` 使用此模式；它们也不签收交付。

恢复后的 Worker 要取得新代次下的新说明并确认，才能再次执行。已经发布的当前计划若在 task rebind 前中断，离线运行器会读取持久计划、核对节点与模板、完成显式 rebind，然后重发说明。已有任务的 AMEND 没有处置时继续阻断，CLI 不推断 update/keep/cancel。其他复杂 DAG、交付或资源清理中断不在本批自动恢复范围。

## 5. 原文、版本与 UNKNOWN

执行绑定独立保存 request_id/task_id/team_id、requirements_revision、plan_revision、task_revision、instruction_revision、dispatcher_epoch、execution_epoch、policy_epoch。请求级调用没有团队，task/instruction/execution 取零；团队级调用必须有对应 typed ID 和有效版本。

AMEND 只追加原文，需求修订递增，活动团队被暂停并围栏。每个任务必须有绑定当前需求和当前计划的 update/keep/cancel。keep 也推进任务修订，要求新的 TeamLead 说明和 Worker ack；不能借用旧说明的 applied 版本恢复业务。

当前基础层把入口提供的每条显式约束分配给每个团队。TaskBrief 和 WorkOrder 必须覆盖全部这些 ID，并保留其逐字文本。原文摘录只证明忠实；没有实现从任意自由文本抽取并证明完整用户意图的语义审查。CLI 初始约束目录可为空，AMEND 需提供整份当前目录。

note 是无外部副作用的持久操作，仍经历 PREPARED → RUNNING → SUCCEEDED，参数、绑定、内容哈希和证据引用进入数据库。真实 workspace/external 操作在 Store 回归中以宿主合成事实验证；这并不代表已执行真实文件或网络工具。非幂等结果 UNKNOWN 时，Worker 可确认当前说明，但新业务必须先等待宿主核对。

## 6. 冻结配置

配置目录在准入前一次加载；Root 默认、每角色覆盖、所选能力、全部模板及提示词／环境模板原文、逻辑路径和哈希一起进入请求快照。adapter_version 与 protocol 为必填且显式冻结；模型的每个缺省字段直接从该请求冻结的 Root 继承，不沿 Dispatcher/TeamLead 逐级继承。

恢复只读取快照字节及其 SHA-256，并重新验证内部一致性；不读取当前文件、不展开环境变量、不执行环境 `check`，也不加载密钥。不同检出目录的同一内容产生相同快照；更新后的配置只影响新请求。显式请求配置迁移命令尚未提供。

LibYAML 解析拒绝重复／未知键、错误类型、anchor/alias/merge、多文档、标签／指令、歧义标量、不安全引用和符号链接，并限制输入大小、节点和深度。模型／能力、审核预留和已定义政策不匹配时失败；无法执行的真实调用、付费重试、非空 pinned Skill、记忆、额外挂载和真实清理配置明确拒绝。

尚未执行的环境模板以 `checks_executed=false` 进入上下文；不能把模板中的 bash/git/bwrap 声明当成已验证隔离能力。被接受的调度及 sandbox 模板字段是被冻结的后续政策，不代表本批已有对应执行器。

## 7. 模型调用与预算

FakeModel 根据角色和调用序号选择脚本，支持文字、工具参数、延迟、错误、截断／中断、可空用量和取消。它不建立网络连接，也不创建每调用线程；步骤由宿主同步执行或由现有 Scheduler 调用。每次显式运行没有自动重试。

调用前先校验模型请求，再把完整 ModelRequest、实际稳定前缀／动态后缀、配置指纹、原始绑定、独立 call_id/attempt_id 和预留量写入 Store。input 预留按实际输入字节保守计算，output 受冻结 profile 上限约束；这不是供应商 tokenizer 或实际 token 用量的声明。初始 CLI 无配置 token 上限时使用明确的离线预留默认值；参数不得超出已冻结非空预算。

响应到达时先保存原输入对应的 raw response 和实际／未知 usage，再判断完整响应、工具声明、模型输入 Schema、原文约束和业务 CAS。错误、非法输出和迟到输出消耗的尝试仍计数；任何未报告的 input/output/cost 保持 null，并保留对应预留。实际报告超过预留也如实记账并影响后续准入，不倒改历史费用。

`call_attempts.accepted` **只表示结算时原绑定仍有资格且传输完成**，不是 Schema 通过、工具被执行、成果被接受或业务交付。实际采用必须有随后对应的计划／说明／操作事件与状态。CLI 成功的 `calls[*].result` 来自已通过这些关口的业务提交，不能只读 accepted 判断采用。

测试脚本显式报告的费用零和 token 数属于合成 fixture。真实供应商调用数为零；未知用量测试不补零。凭据、端点、真实协议和预算仍属于 S1-13 的独立配置与验证。

## 8. ContextCompiler 与 Schema

ContextCompiler 使用版本化、长度分帧的实际内容：工具定义、系统／角色／功能提示词、环境说明、已批准规则和项目快照构成稳定前缀；完整原文、全部 AMEND、任务、当前说明和消息构成动态后缀。稳定前缀 SHA 与模型、reasoning、adapter/protocol、所选模板／环境／实际 profile 等相关依赖共同决定缓存身份。动态任务变化不必改变稳定前缀；相关依赖改变必须失效。超过容量明确失败，不靠截断约束塞入上下文。

SchemaRegistry 使用 Valijson 的 Draft-07 强类型验证。文件必须来自显式本地注册目录；引用预先解析并限定到已注册文档，不访问网络，也不依赖当前网络资源解析 `$ref`。完整记录检查全部 runtime 字段；模型参数投影只移除相应宿主字段及其宿主条件，并禁止模型反向填入。嵌套普通业务字段和条件仍须验证，Plan 中的 TaskBrief 另作验证。

结构验证不证明原文全覆盖、工具权限、当前版本、语义正确、必需证据或成果验收。Store 与宿主继续分别把关。本批本地注册表支持现有 13 份 Schema；不同 Draft、跨目录引用或嵌套资源 ID 等未支持形式会明确拒绝。

## 9. CLI 使用与限制

命令示例见 [README](../../README.md)。所有输入文件有容量限制且拒绝路径中的符号链接和特殊文件；不展开 shell、`~` 或 `${...}`。输出为 JSON，参数／业务错误在 stderr 输出 JSON；冲突或旧版本退出码 3，其他已知错误 2，未分类运行错误 1。

`run-fake` 一次最多调用 Dispatcher、TeamLead、Worker 各一次。已有任务重开时使用冻结配置，必要时补计划 rebind，然后新说明与 ack。任务扩展到多节点、跨能力调用、自动审核、真实工具或循环时会明确拒绝；不会假装完成更复杂计划。

本地 CLI 的写者持有整个运行周期的独占租约，因此尚无第二个 CLI 进程通过 socket 取消正在执行的 Fake 调用。已有同进程控制 API、stop_token 和后续取消路径已测试；正式跨进程控制、并发端点槽位、事件唤醒和等待态释放需 wlsd/运行器接入。不能据本批离线测试宣称已完成这些产品验收。

## 10. 下一增量

优先沿 S1-06/07 实现受控文件与进程工具、输出／超时、路径边界、bubblewrap/cgroup 实际探测，再补角色循环、事件待办、交付／回执／清理与 wlsd。每个真实副作用仍必须进入现有 Store prepare/start/finish/UNKNOWN／reconcile 关口；不得把 note 回归直接改名为真实工具验收。所有新模块继续运行 M1、Schema/SQLite 参考及本批 C++/CLI 回归。
