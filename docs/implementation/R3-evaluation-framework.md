# R3.3c 本地评测框架

接口：`scripts/evaluate_r3.py`。设计见 [ADR-082](../adr/082-paired-evaluation-evidence.md)。
这是固定任务、共同条件、原账本统计和报告实现；实际供应商及单 Agent 质量对照未执行。

## 1. 计划与执行分离

先用普通 `wls submit` 准入一个仅用于读取配置的种子请求，不执行模型。
其配置、工具 allowlist、预算代表此次比较的共同条件。生成计划：

```sh
python scripts/evaluate_r3.py plan --db benchmark.sqlite --request req_SEED > plan.json
```

`req_SEED` 是待替换的真实种子请求 ID。真实供应商实验需显式选择 `--model-ref`、
`--reasoning` 与 `--evidence-mode provider`；该命令自身不授权或执行调用。
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

## 3. 本地读取界限

输入 JSON 限 8 MiB、64 层、100000 节点，拒绝重复键、非有限数、符号链接与特殊文件。
只读 SQLite 事务上限 10 秒、最多4096项/查询、32 MiB/结果、512 MiB 主库；逐行累计编码容量，到限即停止读取；
支持 Store 408/409/410，不执行迁移、恢复围栏或业务写入。SQLite WAL/SHM 属底层读取机制，
不能把只读 SQL 宣称为对任意目录完全无文件系统活动。输入数据库是受信本地主机资料，
不是远程攻击面；hash 是一致性校验而不是签名或外部真实性证明。

语义质量、人审标准、实际端点能力、供应商身份与真实计费验收仍按
[R3.3c 计划](R3-controlled-live-plan.md)执行。当前框架不包含通用单 Agent 执行器或付费入口；
它可接收符合共同条件、通过原 Store 保存的未来单 Agent 运行。
