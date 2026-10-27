# R3.3c 本地评测框架

接口：`scripts/evaluate_r3.py`。设计见 [ADR-082](../adr/082-paired-evaluation-evidence.md)
及 [ADR-087](../adr/087-model-decision-evidence.md)。
模型来源契约更新日期：2026-10-26（America/Los_Angeles）。
这是固定任务、共同条件、原账本统计和报告实现；实际供应商及单 Agent 质量对照未执行。

## 1. 计划与执行分离

先用普通 `wls submit` 准入一个仅用于读取配置的种子请求，不执行模型。
其配置、工具 allowlist、预算代表此次比较的共同条件。生成计划：

```sh
python scripts/evaluate_r3.py plan --db benchmark.sqlite --request req_SEED > plan.json
```

`req_SEED` 是待替换的真实种子请求 ID。真实供应商实验需显式选择 `--model-ref`、
`--reasoning` 与 `--evidence-mode provider`；该命令自身不授权或执行调用。
新生成的 provider 计划固定为 `schema_version: 2`、`r3-small-tasks-v2` 和
`decision_evidence: model-origin-v1`。Synthetic 计划默认保持 v1，可显式传
`--decision-evidence model-origin-v1` 验证本地合成的完整来源关系；计划命令不执行模型。

计划固定五项任务，每项 `multi_agent`、`single_agent` 各一次；更大的采样设计应另行版本化。
按计划中逐字 material 新建各自请求，共同模型、约束、工具、环境、政策和预算必须相同。
运行器仍走原 Store；运行索引只记录本次计划、原数据库/请求及宿主观察的耗时。

```json
[
  {
    "task_id": "tool_roundtrip",
    "condition": "multi_agent",
    "database": "runs/tool-roundtrip.sqlite",
    "request_id": "req_REPLACE_WITH_ACTUAL_ID",
    "elapsed_ms": 1234,
    "plan_sha256": "REPLACE_WITH_ACTUAL_PLAN_SHA256"
  }
]
```

这些是说明用占位符；真实指纹必须来自计划。数据库相对路径以运行索引所在目录为基准。
没有执行的样本不造数据库或调用记录；可以直接用空数组生成十项 `not_executed` 报告。

```sh
python scripts/evaluate_r3.py report --plan plan.json --runs runs.json > report.json
```

## 2. 判据与数据来源

| 项目 | 实际来源/行为 |
| --- | --- |
| 相同材料 | 每个 request 原 revision=1 文本必须逐字匹配固定任务 |
| 公平条件 | profile、reasoning、环境依赖、初始约束、工作流政策、工具集合与逐维度预算指纹 |
| 单 Agent | 原 attempts 只含同一 principal/team 的 Worker 模型调用；多个 Worker 身份不能换名为单 Agent |
| 多角色 | 原 attempts 至少包含两种角色；未发生调用不算运行完成 |
| 工具/文件 | 原操作成功状态；提交的原 base64 字节、长度和 SHA256；不重读现文件冒充旧成果 |
| 任务版本/AMEND | 当前需求/计划与任务原绑定；必须有两个不同且已接受的任务，原处置分别为 update、keep，全部取消不算完成 |
| 返工与升级 | 同一原任务的否定验收、较新修订的接受与该团队返工计数；升级须有原 escalate_task 与当前未回答问题 |
| 交付 | 当前需求/计划的 CONFIRMED 交付与同 request 的精确 receipt，验证保存的原包字节 |
| token/费用 | 每个原 attempt 或其独立对账投影；null 单列 unknown，已知值累计，不将未知填零 |
| 实际类别 | fake、local_tls、provider 分开；live 原观察 hash 单独列出，不把配置当发送证明 |
| 时间 | 宿主索引提供 elapsed_ms，单列来源并核墙钟限额 |
| 不完整样本 | failed、invalid_evidence、not_executed 保留在固定十个样本位中 |

金额为原账本 micro USD；cache read/write 是供应商报告的可空 token 子量，
不推算折扣、命中率、供应商价目或普遍成本优势。attempts 的 accepted 也不等同于业务验收。
报告给出当前 request 状态、原 attempt/settlement 身份、计数、hash 和判据，不展开提示词、
私有 provider 原响应、凭据或原交付正文。

## 3. 版本与模型决定来源

| 计划版本 | 原判据解释 | 报告范围 |
| --- | --- | --- |
| v1 / `r3-small-tasks-v1` | 原固定判据不变；旧 provider 计划也不改义 | `structural-v1`，`original_structural_checks_only`；来源 `not_checked` |
| v2 / `r3-small-tasks-v2` | 原结构判据加 `model-origin-v1`，下述两项原诊断/路径来源明确化 | `model_application_and_structural_checks`，逐应用来源 linked/incomplete |

v2 从原 accepted completed response 的唯一 tool_call 开始，核对原 request envelope 身份、
冻结配置和绑定，读取当时捕获的 workflow_action 与目标版本，再关联 immutable
`model_tool_feedback`、原业务 event 及实际业务记录。原 v1 task_contract 帧与 v2 logical input
均按自身格式读取；不能用当前任务/目标行补写旧模型输入。event 的 command ID 必须来自原
attempt/tool_call；payload、feedback/result 和业务记录必须相符。

存在 live 原记录时，还需同一 attempt 的唯一 send claim、已冻结的请求/端点配置 hash、完整
transport、原 decoded response 及原 headers/body/protocol hash 相同。该来源链只证明受信宿主
资料内的关系；本地一致性 hash 不是 provider 签名或账单真实性证明。

各任务的 v2 附加判据如下：

| 固定任务 | 必需的原模型来源 |
| --- | --- |
| tool_roundtrip | 同绑定按顺序 read seed → write result → read result → model submission → model check；期望原文件字节属于这同一 submission |
| partial_amend | 捕获原 AMEND 阶段、pending 节点与 task CAS；模型 update/keep 与原处置同值；两个不同当前任务都由模型提交、检查、接受 |
| rejected_rework | 同任务原模型否定接受、change_task、较新修订提交/检查/接受；不能仅增加返工计数 |
| exhausted_rework | 原失败来源、捕获时冻结返工额度耗尽、模型 escalate_task 与实际当前用户问题同目标 |
| delivery_receipt | 原 CONFIRMED package/receipt 的每个 acceptance 都来自模型 plan/order/submit/check/accept 链 |

v1 的工具/文件判断保持原解释。v2 明确读取 `fs_read` 的原诊断 feedback，因诊断不生成
业务 ops 行；文件判据仅将原提交 `/work/result.txt` 映射到固定逻辑文件名 `result.txt`。
这两项只作用于 v2，不静默改变已经发布的 v1 判据。

v2 的文件判据仅消费已验证原模型 submit 对应的原提交，不能从另一宿主提交补齐。
往返后的同一提交必须处于当前业务版本，且原 readback 早于该模型 submit、submit 早于该
submission 的模型检查。当前接受链还核实际 acceptance/任务仍为 ACCEPTED，以及原 producer
需求、计划、任务、指令版本；恢复 epoch 不替代业务版本判断。返工和升级的原来源必须与同一
当前要求/计划的实际接受或问题一致，旧模型事实不能与当前宿主事实拼接通过。

缺原 feedback 或原应用 event 使来源不完整并失败；原响应、捕获目标、实际应用或 hash 矛盾为
`invalid_evidence`。受信宿主独立写入的合法管理事实不能填补模型链。报告只暴露原 ID、角色、
动作名和 hash，既不暴露模型 arguments/私有正文，也不重读当前工作区代替旧提交。

`provider_quality_acceptance_established` 始终为 false。v2 来源通过、原结构通过与真实供应商
质量是三种不同结论；后者仍需明确的授权、端点能力、语义标准、真实样本与人审。固定脚本的
Fake 或 TLS peer 只能验证工程路径。`single_agent` 在两个版本都保持同一 Worker
principal/team 规则；多任务基线身份设计与执行器仍开放，不能把多个 Worker 或管理者重命名通过。

## 4. 本地读取界限

输入 JSON 限 8 MiB、64 层、100000 节点，拒绝重复键、非有限数、符号链接与特殊文件。
只读 SQLite 事务上限 10 秒、最多4096项/查询、32 MiB/结果、512 MiB 主库；逐行累计编码容量，到限即停止读取；
支持 Store 408/409/410，不执行迁移、恢复围栏或业务写入。SQLite WAL/SHM 属底层读取机制，
不能把只读 SQL 宣称为对任意目录完全无文件系统活动。输入数据库是受信本地主机资料，
不是远程攻击面；hash 是一致性校验而不是签名或外部真实性证明。

语义质量、人审标准、实际端点能力、供应商身份与真实计费验收仍按
[R3.3c 计划](R3-controlled-live-plan.md)执行。当前框架不包含通用单 Agent 执行器或付费入口；
它可接收符合共同条件、通过原 Store 保存的未来单 Agent 运行。
