# schemas/

层间交接消息的 JSON Schema（draft-07，选这个版本是为了兼容 C++ 校验库）。字段名和枚举值以这里为准。

| 文件 | 消息 |
| --- | --- |
| plan.schema.json | Dispatcher 提交的计划（DAG） |
| task_brief.schema.json | Dispatcher 给团队的任务说明 |
| work_order.schema.json | TeamLead 给 Worker 的执行说明 |
| change_order.schema.json | 变更与返工指令 |
| submission.schema.json | Worker 提交的成果 |
| review_request.schema.json / review_verdict.schema.json | 审核能力的输入与结论 |
| question.schema.json | 逐级上报的问题 |
| experience_report.schema.json | TeamLead 的经验报告 |
| delivery_manifest.schema.json | 交付清单 |
| memory_frontmatter.schema.json | 记忆文件（规则、事实、经验）的文件头 |

## 约定

- **运行时字段：** 描述里带"（运行时填写，模型不要提供）"的字段（各种 ID、版本号、时间、哈希、用量），不出现在给模型的工具参数里。实现时从完整 Schema 去掉这些字段，得到工具的 input_schema；运行时补全后再按完整 Schema 校验并存储。
- **嵌套消息：** Plan 中的 `brief`、`input` 只声明为 object，由运行时再用 task_brief 或能力的 input_schema 单独校验（避免跨文件 `$ref`）。
- **逐字摘录：** `constraints_verbatim` 的每一条必须是请求原文或修改原文的子串（去首尾空白后），这一条 Schema 表达不了，由运行时检查。WorkOrder 必须包含 TaskBrief 的全部条目；TeamLead 发出的 ChangeOrder 必须包含上级 ChangeOrder 的全部 `constraints_verbatim` 和 `findings`。

## examples/

一个连贯的示例场景（清洗销售数据 → 中途改为按月份 × 地区汇总 → 审核发现退款口径问题），每条消息一个文件，可直接用作单元测试的夹具：

- `request_texts.json`：请求原文（rev 1）与修改原文（rev 2），逐字摘录检查以它为准。
- `change_order.dispatcher.json` 与 `change_order.team_lead.json`：同一次修改在两层的写法，可以对照看 TeamLead 如何"翻译"变更。

所有示例都通过了对应 Schema 的校验；另有 13 个反例（缺必填项、越出 /work、no_new_experience 却有条目等）确认会被拒绝。
