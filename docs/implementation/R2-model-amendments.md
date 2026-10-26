# R2.3：Dispatcher 模型的逐任务 AMEND 处置

实现 0.8.2-r2.3；蓝图 v0.4.1；设计见 [ADR-073](../adr/073-model-amendment-dispositions.md)。
本项承接 R2.2 的请求限定恢复和拒绝返工；实际验证见[版本报告](../reports/r2-model-amendment-validation.md)。

## 入口与工具

当前工作流 `pump-fake` 在没有非空宿主 `--dispositions-file` 时启用模型处置。
Root 的 `amend` 仍只追加原需求文本/当前约束目录并暂停任务，不能替 Dispatcher 作业务决定。
首次请求的普通计划仍使用 `submit_plan`；有已分配任务的 AMEND 使用下列专用工具：

| 当前状态 | 唯一声明的模型动作 | 参数和效果 |
| --- | --- | --- |
| 尚无基于当前需求修订的计划 | `submit_amendment_plan` | `plan` 加 `dispositions`；新计划和选中的节点决定在同一事务提交 |
| 已有当前计划，部分任务尚未处置 | `resolve_amendment` | 仅 `dispositions`；沿用当前计划，不重新围栏已处置任务 |

`plan` 只含原 Plan Schema 的模型字段，保留所有已分配节点和团队模板，不能新增/删除/改名或换模板。
目前仍只执行独立 `team_task`，不新增 DAG、跨功能调用或独立审核。
`dispositions` 是 1–20 个现有待处置 node_id 的映射，仍受请求冻结团队数限制。例如：

```json
{
  "dispositions": {
    "beta": {
      "disposition": "keep",
      "rationale": "已核对本次修改，保留该节点在当前计划中的目标和适用约束。"
    }
  }
}
```

每项只能包含 `disposition`（update/keep/cancel）与非空 `rationale`（最多1000个字符，
Store 另限4096 UTF-8字节）。空集合、空白理由、未知节点或模型提供的版本/CAS均拒绝。
首次可以只决定 alpha；beta 仍旧暂停且保留原 AMEND inbox。之后可单独决定 beta，
不再增加 plan_revision。update/keep 的 TaskBrief 取当前计划；模型不得另塞替代任务正文。

宿主显式处置文件继续使用原 `submit_plan` / Store 接口。旧 `run-fake` 和 schema404
兼容运行器不自动切换到本协议。`OfflineRuntime::reserve_call` 的最后可选布尔参数由
受信事件宿主选择，默认 false；既有嵌入调用保持原行为。

## 捕获、版本与事务

宿主先捕获原请求 CAS、完整请求 binding、原文、计划及任务快照，再保存完整模型输入和预算预约。
`workflow_action.kind=amendment` 的 target 含 `plan_required`、`tasks` 和 `pending_node_ids`。
这些是运行时原始输入，不从模型响应恢复。响应必须匹配原 attempt/request_envelope 和 workflow_action。

新计划会影响全部已分配任务，因此最终事务核对全部捕获的任务 ID、node_id、state_version、
team_id、execution_epoch。后补决定只核对实际选中的捕获待处置节点；已经当前的兄弟任务可以
继续说明/确认而不使无关节点决定失效。Store 在同一独占写者事务内完成核对和计划/任务/事件写入。
请求 CAS 不能代替任务 CAS。失败回滚，不允许从最新行重新填写版本来采用旧模型判断。

原响应和实际/未知用量先按原调用结算，非法输入或迟到响应仍保留费用和原反馈。
精确重复响应返回原结果；同调用改参冲突。命令记录增加宿主捕获守卫，仅模型路径写此字段；
原宿主路径不改变已发布 command payload 的哈希和幂等身份。

## 原门禁和事件

keep 仍推进任务修订，必须由 TeamLead 下达新说明，再由 Worker 确认。旧原说明行与文件字节保留。
处置不重置共享返工次数/模型预算，不清除 UNKNOWN。cancel 停止该任务的新业务，不表示回滚旧副作用。
下一次 AMEND、恢复、政策变化、目标任务变化或团队代次变化会阻断旧决定。

一次 pump 仍有冻结管理调用上限、总调用上限、Worker 上限和正常预算。
模型缺脚本、输出无效、预算耗尽或显式批次结束，不自动重试，也不确认未完成节点。
仅确认原 claim 中已经明确处置到当前要求和计划的 AMEND 项；下一批可处理遗漏项。
只恢复当前请求；同数据库其他请求的逐表行、等待、账本、原文件及收尾作业保持不变。

## 边界与后续

没有新增或修改 SQL 迁移，不重写冻结快照、原成果、说明、调用或审核档案。
固定 FakeModel 驱动真实 SQLite/临时文件/CLI，不能证明真实模型判断质量或强隔离部署。
管理角色仍受原上下文字节上限，不能容纳完整 AMEND 契约时在预约前失败并保留待办。

R2a 接下来处理 fs_mkdir 和 PREPARED 分配恢复/显式放弃；R3 先实现单协议离线回放，
真实端点/认证/预算明确配置后才进行付费验收。并发写者、按需提升执行代次、通用任务重定义、
独立审核、记忆、MCP 和服务化仍按各自任务卡推进。
