# S1.4 问答、提交与团队检查契约（0.5.0-s1.4）

更新：2026-10-19（UTC）。目标蓝图仍为 **v0.4.1**，开发基线为已推送的 S1.3 `8a9328d90175b63161582fa62fa1a3843d149cc5`。本增量沿 S1-03/07/08/09/10/12 接入 Worker 提问、逐级回答与转发、真实文件提交快照和 TeamLead 有界检查返工。

[S1.3 事件契约](S1-event-runtime.md) 的具体项领取、独立任务、原绑定反馈与容量边界继续有效；[S1.2 Worker 工具契约](S1-worker-tools.md) 的文件授权、目录身份、派发、UNKNOWN 与强进程边界继续有效。当前实际测试证据见 [S1.4 验证报告](../reports/s1-workflow-runtime-validation.md)。原报告保留各自提交的历史含义。

## 1. 范围和状态

本批仍使用有限同步 `pump-fake` 和冻结 FakeModel 脚本。一次显式 pump 可以推进“提问 → TeamLead 回答或转发 → Dispatcher 回答或上报用户”，也可以推进“真实文件 → 提交 → TeamLead 检查返工 → 新说明确认 → 再提交”。用户回答由独立 CLI `answer` 输入，不由脚本代签。

TeamLead 通过只将该份提交记为 `CANDIDATE_READY` 并留下 Dispatcher 后续处理待办。它不产生独立审核票据，不把任务置为 `ACCEPTED`，也不把请求置为 `DELIVERED`。提交记录明确保留 `business_accepted=false` 和 `business_delivered=false`。

本轮不接入跨功能正式审核、确定性业务验收器、交付包发布、回执、经验、清理、wlsd/API 或真实供应商。原 M1 的合成候选/验收/回执仍单独回归。

## 2. SQLite 405 和兼容

追加的 `007_workflow_runtime.sql` 将已知 404 数据库事务升级到 405。原 `schema.sql`、004、005、006 的字节保持不变；升级保留原请求、配置、任务、事件、等待、操作、输出及调用账本。

| 新结构 | 用途 |
| --- | --- |
| `workflow_runtime_metadata` | 007 分发资产的 SHA-256 与实现版本 |
| `question_runtime` | 复用原 questions 行，另存不可变原 binding、内容哈希、状态、blocking 和等待 generation |
| `question_transitions` | 每次转发/回答的原调用身份、捕获 binding、对象版本和原始内容 |
| `workflow_submissions` | 原提交、原完整 binding、实际文件快照及不可变哈希，另有处理状态和 CAS |
| `team_lead_checks` | 每份提交的检查、原检查调用 binding、返工说明和处理结果 |
| `team_workflow_state` | 团队内部返工次数，不用请求需求版本替代返工计数 |

Store 构造函数第七个参数为 007 资产。405 打开须提供并匹配各代分发资产；迁移失败回滚。旧 402/403/404 调用方继续使用原接口。ReadOnly 不迁移、不接管、不增加代次；工作流新查询在尚未升级的库上明确返回不可用。

正常 S1.3 的 questions 表为空；若旧 404 库含没有原执行绑定的问题行，007 拒绝自动编造其授权历史，须专门迁移。缺完整冻结配置的非空 401 仍须专门迁移方案，M1 Journal 不作为 SQLite 导入。测试中的事务与重启证据不等同于整机掉电保证。

## 3. Worker 发起的逐级问题

Worker 的 `ask` 使用现有 Question 模型投影：`text`、可选 `context/options/blocking`。问题 ID、提问者、任务、原始完整版本和等待代次由宿主填写，模型不能提供这些字段。当前主动提问入口限 Worker；管理角色处理该原问题。

问题默认 blocking。同一事务创建原问题与精确等待，后续新 Worker 预约及业务派发被阻止。非阻塞问题保留待办而不创建该等待。原文、背景、选项和原绑定不可变，各级转发只能追加自己的 note。

| 当前持有人 | 允许动作 | 后续持有人 |
| --- | --- | --- |
| 本任务 TeamLead | 回答，或附注转发 | answered 或 Dispatcher |
| 本请求 Dispatcher | 回答，或附注转发 | answered 或 user |
| user | Root 接收明确用户答案 | answered |

每次处理检查真实身份、对象归属、当前持有人、问题 CAS、原语义目标及该次管理调用的原 binding。模型回答参数只有 `text` 和/或原选项的 `selected_option`；不能指定任意问题 ID、另一团队或新版本。Root 不能跳过层级回答尚由 TeamLead/Dispatcher 持有的问题。

问题原状态为 `OPEN / ANSWERED / SUPERSEDED`。每次转发/回答有独立不可变记录。空白文本、未知选项、重复选项身份或过期对象不能成为有效答案。

## 4. 回答、等待和恢复

回答只完成原问题，以及与它绑定的那一代等待。它不会修改 AMEND 处置、applied 版本或 UNKNOWN 操作结果。宿主的普通 `wait/wake` 不能替换或解除一个尚未回答的 blocking question。

纯 Dispatcher/执行代次变化的接管可以继续处理同一个语义问题；原问题和答案不重新盖成新版本。需求、计划、任务或说明变化会使旧问题失效，旧答案不能唤醒后来新的等待。政策变化后，持当前 policy_epoch 的新管理调用仍可回答或转发原问题，避免等待控制被锁死；原问题政策号保持历史值，已捕获的旧政策响应仍被拒绝。跨进程恢复后的 Worker 仍须按现有规则取得当前说明、确认新应用绑定，并核清 UNKNOWN 后才能继续业务。

等待一个任务不暂停兄弟任务。管理模型仅为当前可处理的问题预约，不能借回答通路声明 `issue_work_order` 来绕过等待。达到调用、预算或上下文上限时，问题继续保持持久待办。

## 5. 实际文件提交快照

Worker 的 `submit` 使用现有 Submission 模型投影。Worker 只能声明摘要、成果路径、自检、验证和已知问题，不能提供提交 ID、执行版本、文件哈希或 `team_lead_check` 自我批准。

宿主对每个声明的 `/work/...` 路径调用当前团队获准的受控 `fs_read`，捕获完整文件。该流程需要当前 `submit` 和 `fs_read` 权限、已绑定 Workspace，以及当前业务就绪状态；未结算操作阻止提交。路径先经过原有 openat2、目录身份和链接检查，不读任意宿主文件。

| 快照边界 | 上限 |
| --- | ---: |
| 每次提交文件数 | 8 |
| 每个文件完整字节数 | 65,536 |
| 单次合计原始字节数 | 262,144 |

超过上限、缺文件或读页不完整时拒绝整次提交，不截取前缀冒充完整成果。每个快照项固定为 `{path, sha256, size_bytes, content_base64}`，Store 再校验规范 base64、实际字节数、SHA-256 与成果路径一一覆盖，和原提交及 binding 同事务保存。

保存后再修改工作目录，不改变旧提交。TeamLead 和后续查询读取原快照，不能重新读取现在的文件来替换原证据。TeamLead 上下文另有从持久 base64 解码的 submission_file_views：UTF-8 文件显示原完整文本，二进制文件明确标为 binary=true/content=null，原字节与哈希照常保留。每次新提交有新 ID，原内容及检查记录保持可追溯。多个文件是有界逐文件观察；这不是对任意同 UID 外部写者的全目录同时刻快照。

这些字节是 SQLite 内的不可变提交证据，尚不构成 S1-11 的交付目录、原子发布包或接收回执。

## 6. TeamLead 检查与有界返工

TeamLead 一次检查一个宿主在模型预约时捕获的 `SUBMITTED` 对象。上下文包含原提交、文件快照、自检、验证、当前 TaskBrief、说明与版本；模型不填写提交 ID、CAS 或候选哈希。

`check_submission` 参数为 `passed`、可选 `notes/criteria_results/work_order`。失败必须给出合法的新 WorkOrder；通过禁止夹带新 WorkOrder。宿主按原调用目标分离检查内容与返工说明，Store 原子核对任务 CAS、提交 CAS、团队身份及语义绑定。

通过要求 TeamLead 检查精确覆盖当前验收项、没有重复或外来 ID 且全部为 yes；原自检也须全部为 yes 并有非空证据。若声明验证结果，则每项必须通过且有证据；存在已知问题时不能通过。本轮未接豁免适配器。提交时允许 no/partial 自检进入返工流程。Schema 通过仍不证明任务语义质量；FakeModel 的判断是固定夹具，本批不声称真实模型已审查任意成果。

允许返工时，原提交进入 `RETURNED_FOR_REWORK`；任务修订和说明修订分别增加，需求和计划修订不因内部返工增加。新说明保留当前任务目标、验收项和全部登记约束，Worker 重新 ack 后才恢复业务。返工次数取请求冻结的 `team_internal_rework_max`，达到上限进入 `BLOCKED` 并保留 Dispatcher 待办。计数绑定 team_id，重启、普通重发说明或 AMEND 不隐式清零；上限为 0 时首次失败即阻塞。

提交后政策变化时，正面检查要求原提交政策仍匹配；不能把历史政策下的提交直接放行。持当前政策绑定的新 TeamLead 可以做负面检查或撤销就绪，明确返工后在新政策下重新提交。原提交和原政策不被改写；任何已捕获的旧政策检查响应仍拒绝。

提交及普通检查计普通事件额度（每请求 4096）。有限问题处理与必要控制保留各自受限通路，不能以新增问题或检查名义取得无限控制写入。Store 宿主 API 允许对当前 CANDIDATE_READY 做带原目标 CAS 的负面复查，并使用有界控制额度（每请求 8192）撤销就绪资格；模型运行器当前只选择 SUBMITTED 作检查对象。完整正式审核、自动复查调度和业务验收继续属于后续实现。

## 7. 捕获目标、逐项事件与上下文

405 的运行器在原 ModelRequest 中固定一个 `workflow_action` 及完整目标快照。问题调用只声明回答/转发工具，提交检查调用只声明检查工具；工具返回后使用这份原捕获身份、对象 ID 和 CAS。旧响应不以读取当前数据库的方式重新获得资格。

每轮仅确认已经处理或已明确失效的捕获事件。当前 plan/instruction 存在不表示新到问题、提交或 Dispatcher 候选待办已完成。缺步骤、错误、达到调用次数/预算上限的对象保留待办；一个任务阻塞不吞掉另一个任务的事件。

Worker 动态上下文恢复自己任务的原问题、答案、提交和返工反馈，管理角色只获取本次授权的目标快照。原历史的绑定、明确缺失与容量前置检查继续沿用 S1.3；不递归加入旧 compiled context，不静默删除有效约束。完整调用上下文仍限 1 MiB；大文件视图或累计历史使其超限时，在预约前明确拒绝，待办保留。

原 `run-fake` 保留单任务兼容能力，但遇到尚待处理的问题或提交时明确要求使用工作流入口，不能自动重发说明使旧工作流失效。405 pump 的新流程与旧 402/403/404 接口分别回归。

## 8. CLI

新增命令使用本机数据库所有者的现有信任边界：

```sh
wls questions --db FILE --request ID [--task ID] [--limit N]
wls submissions --db FILE --request ID --task ID [--limit N]
wls answer --db FILE --request ID --question ID --expected-version N \
  [--text-file FILE] [--selected-option ID]
```

两个查询只读，包含完整原记录及明确状态；questions 最多 128 条，submissions 最多 64 条，限制不足以返回完整要求范围时明确报错。answer 至少提供文本或选项，文本最多 4000 个 Unicode 字符（文件读取上限 16000 字节），`--expected-version` 使用查询得到的问题 `state_version`。CLI 在打开写者前检查参数、归属、持有人、对象版本和选项，Store 在接管后的事务中再次检查。

CLI 不隐含模型重试，也不把重复提交旧答案当作新的授权。原 Store 管理命令的精确重放仍使用原捕获上下文；CLI 当前不暴露跨进程复用的 command ID。写者仍持独占租约，跨进程异步取消与远程身份属于 wlsd/API 后续任务。

可运行脚本在 `tests/fixtures/s14_fake_ask.json`、`s14_fake_question_route.json`、`s14_fake_submit.json` 和 `s14_fake_rework.json`；其中 question_route 演示逐级转发到用户，ask 在 TeamLead 持有问题时停止。对应实际跨进程驱动为 [s14_cli_smoke.py](../../tests/s14_cli_smoke.py)。README 提供独立目录示例。

## 9. 后续范围

下一增量将候选就绪待办接入当前版本的业务验收、不可变交付包、回执、经验与可恢复清理。完整跨功能独立审核、DAG 和全局审核双容量按 S2 推进；强沙箱正向、残留 cgroup 恢复、后台 run 和真实 input 挂载继续单独验收。

本批没有真实模型调用、费用、服务部署或用户数据清理。S1 整阶段、原 51 项产品验收及尚未覆盖的任务卡不因问答/返工回归通过而自动完成。
