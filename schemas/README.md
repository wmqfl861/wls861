# v0.4.1 消息契约

这里是修订后的 JSON Schema draft-07 与完整消息夹具。原始 v0.4 逐字节归档在 `docs/handoff/v0.4/`，不再改写。本目录与 `db/schema.sql`、`docs/spec/v0.4.1/` 对齐；它定义目标协议，不表示当前 C++ 内核已经实现所有能力。

| 文件 | 消息及主要边界 |
| --- | --- |
| request_snapshot.schema.json | 原始请求、逐版 AMEND 和宿主确认的有效约束目录 |
| execution_binding.schema.json | 请求/任务/团队及各层版本、执行代次、策略代次 |
| plan.schema.json | Dispatcher 计划，显式 plan_revision 和 based_on_requirements_revision |
| task_brief.schema.json | 任务说明，包含宿主分配的 constraint_refs |
| work_order.schema.json | TeamLead 说明与完整冻结执行绑定 |
| change_order.schema.json | 变更/返工；目标版本与父变更绑定，最多 100 条 findings |
| submission.schema.json | 完整提交；实际 load_id/revision、冻结版本和逐项自检 |
| review_request.schema.json / review_verdict.schema.json | 候选/提交/审核轮次及生产方、审核方独立冻结绑定 |
| change_task.schema.json / escalate_task.schema.json | R2.1 捕获拒绝成果的纠正修订／上报用户参数；模型不提供身份、版本、额度或新的 TaskBrief |
| question.schema.json | 逐级问题和宿主填充的定位元数据 |
| experience_report.schema.json | 经验提案、加载评价、expected_base_revision |
| delivery_manifest.schema.json | 交付文件清单、摘要、大小和来源版本 |
| memory_frontmatter.schema.json | 记忆文件头；内容授权/发布由宿主另行核验 |

## 完整记录与模型工具输入

`x-runtime-field: true` 标出只由宿主填写的字段。对应的完整记录中，消息路由、安全版本和摘要等必需字段设为 required。工具参数投影递归移除这些属性及相关 required；模型不能伪造版本或 Worker 自填 `team_lead_check`。该检查在 `scripts/validate_contracts.py` 中可执行；投影不靠自然语言描述猜测属性。

流程是：**调用发起时**冻结权威上下文并持久化 → 验证模型输入格式且拒绝宿主字段 → 从该调用的冻结上下文补齐 → 完整 Schema 校验 → 与当前权威状态作语义/CAS 检查 → 事务提交。不能在响应回来后从“最新行”重新填版本。TeamLead 对 Worker 提交的复核由宿主校验其团队归属后补充，Worker 的工具输入中不存在这一字段。

Schema `$id` 是稳定协议标识，不是下载地址。验证器维护显式本地 registry，未知 `$ref` 即使在未访问的分支也失败，resolver 不回退 HTTP/file 下载。draft-07 元 Schema 来自已安装库。Plan 中 team_task 的 brief 额外按 TaskBrief 校验；动态 capability 输入必须由未来能力注册表查到对应的本地 Schema 才能执行，不能因其类型是 object 就放行。

## 结构验证之后的权威检查

### 需求覆盖和层间交接

`request_snapshot` 保存连续的 `requirements_revision` 原文和有效 `constraint_catalog`。目录中的每条约束拥有稳定 ID、原文以及 source_revision；它必须能追溯到指定版本的原文。宿主或已批准的人类流程负责目录完整性、AMEND 中约束的保留/撤销和对任务的分配，模型不能自行决定哪些约束适用。

TaskBrief/WorkOrder/ChangeOrder 的 `constraint_refs` 必须与宿主分配集合一致，并保留对应原文；其他摘录也必须来自请求原文。只验证“剩余每句都是原文子串”无法发现删除了一项。回归故意删除“不要修改原始文件”：Schema 格式仍通过，权威覆盖检查拒绝。此参考能够检查已给定目录的完整转递，不能证明自然语言抽取本身没有遗漏。

WorkOrder 必须保留 TaskBrief 的全部约束。TeamLead ChangeOrder 必须来自拥有当前任务的团队，绑定相同请求/任务、父 ChangeOrder、新需求/计划/任务/说明版本和代次；不能改变父 acceptance_changes 或删除、降级 findings。Dispatcher/运行时负责规范标准和审核返工来源，TeamLead 负责操作性说明。ReviewVerdict 与 ChangeOrder 均允许 100 条 findings；测试覆盖 51 和 100 条完整传递，101 条明确拒绝。

### 提交、审核和验收

Submission 的 `requirements_revision/plan_revision/task_revision/instruction_revision` 与 `dispatcher_epoch/execution_epoch/policy_epoch` 分别独立。`self_check` 必须覆盖宿主当前标准，不能通过删掉未满足项获得通过。

ReviewRequest/Verdict 冻结生产方请求、任务、候选摘要、submission_id、review_id、round 与版本。另有完整的 `reviewer_request_id/task_id/team_id` 和 `reviewer_requirements_revision/plan_revision/task_revision/instruction_revision/dispatcher_epoch/execution_epoch/policy_epoch`。生产方和审核方的快照都必须仍然匹配；只检查生产方版本不能防止已恢复审核团队的旧回包。

`changes_requested` 可以是格式正确、有效的审核交付，但不是产品验收通过。模型 evidence、passed、自检、交付 manifest acceptance 字段都是待核验内容。`require_product_acceptance()` 另外要求宿主保存的确定性检查、执行者/证据引用和同一 ticket/候选/版本，失败或未执行不能用 Schema pass 替代。用户豁免只能来自宿主已认证用户，绑定当前具体问题，不能由 Dispatcher 或模型自行声明，也不能覆盖确定性验收失败。

### 记忆提案与使用证据

完整 Submission/ExperienceReport 的使用项必须包含 `memory_id/revision/load_id`。模型回显加载回执的 load_id；宿主从该不可变记录补 revision，并核对任务/Agent 实例、实际全文加载及动作证据，禁止用当前 memory_meta 版本替换。展示卡片或 recall 提示不能当成实质使用成功。

`kind=revise` 的提案还必须有 `target_memory_id` 与 `expected_base_revision`。Schema 只能要求格式，是否真基于已加载版本、是否允许写入、内容安全及 CAS 都属于宿主 Writer。参考 SQLite helper 演示当前版本 CAS 和按版本统计；Git 提交、持久冲突草稿、来源独立性判定和人工合并仍是未来 S3 工作。

### 路径和交付

交付文件名拒绝 `.`、`..`、路径分隔符和 NUL，但 `/work/` 前缀或一个合法文件名不证明文件安全。真实执行器仍必须核对工作区边界、符号链接、文件类型、内容摘要与大小，并用宿主产物快照填充交付元数据。验证脚本不会执行例子中的命令字符串或访问这些路径。

## 示例与验证入口

```sh
/usr/bin/python3 scripts/validate_contracts.py
/usr/bin/python3 tests/contracts_tests.py
```

`examples/` 有 15 个完整消息／工具参数文件，当前检查全部通过，Plan 内嵌的 TaskBrief 也单独验证。`request_texts.json` 对应 RequestSnapshot；它包含原始需求版本 1、修改版本 2 和五条目录约束。初始 Plan/TaskBrief/WorkOrder 属于版本 1；两层 ChangeOrder、Submission、Review 属于版本 2。所有摘要、证据路径、用量和内容都是合成夹具，没有真实模型或业务执行。

示例描述销售数据处理中的不同交接时点。ReviewVerdict 特意返回 changes_requested；这份否定报告不授权交付。DeliveryManifest 是交付消息的格式示例，不能与前面的否定报告拼接成“已通过”的运行证据。测试用单独的宿主证据夹具验证通过/失败/豁免分支。

实际通过的测试数和完整构建命令以本轮 `docs/reports/` 记录为准。历史交接包中的计划验收表保持原状，Schema 或参考事务通过也不会把“未执行”的产品验收行变成已完成。
