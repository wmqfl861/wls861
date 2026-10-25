# R2.1 拒绝成果的有限纠正和用户升级

实现 0.7.6-r2.1，蓝图 v0.4.1，基线 `19ea603`。ADR-070 是本批决策；
[实际验证](../reports/r2-rejected-task-validation.md)与发布状态单独记录。

## 捕获与工具

Dispatcher 的下一管理目标可为 `rejection`。目标是一个当前 BLOCKED 的生产成果，
附原提交字节/哈希/绑定、原否定验收或返工耗尽的 TL 检查、任务和成果 CAS、冻结额度和已用次数。
目标先写入持久 ModelRequest，模型不能提供 task_id、submission_id、版本、身份或新的 TaskBrief。
运行时在响应结算后验证 Schema，并在 Store 事务内重新校验当前权限、政策、语义目标及两份 CAS。

| 工具 | 模型参数 | 原子效果 |
| --- | --- | --- |
| change_task | rationale、correction，各1–2000字符且最多8000 UTF-8字节 | 原 TaskBrief 字节不变；task_revision+1、READY、applied清零，团队暂停；原成果RETURNED_FOR_REWORK；共享计数+1及TL待办 |
| escalate_task | text；可选context/options，沿Question相同文本边界 | 当前任务保持BLOCKED；宿主创建一个user持有、asked_by=dispatcher的阻塞问题及精确等待，记录原成果关联 |

change_task不会代写新WorkOrder。当前TL收到原拒绝与纠正方向后发布新说明；Store额外要求
新说明保留原 TaskBrief 和旧说明约束，Worker必须重新确认后才能执行业务。旧说明行与提交字节不改。
这是原目标内的纠正，不是任意change_task/完整ChangeOrder协议；改变需求/验收项仍须显式AMEND。

## 次数、等待与安全

TeamLead 与 Dispatcher 使用同一 team_workflow_state.rework_count 和原冻结
team_internal_rework_max。重开、回答、普通新说明、AMEND及容量检查点不重置计数。
剩余0时不声明change_task，Store同样拒绝；每个拒绝成果最多升级一次，换command_id不能重新提问。
已回答且额度0时保持BLOCKED，要求所有者另行AMEND/CANCEL；回答不是额度或验收豁免。

纠正前须本团队所有副作用/用量已结算，且无等待或未答阻塞问题。UNKNOWN不能被修订清除。
升级问题可以说明未决操作，但不修改或核对其结果。STOPPED团队、终态请求、旧AMEND目标均不能借纠正恢复。
当前政策可处理旧生产政策下的负面结果；旧政策捕获的模型响应仍被围栏，原权限不会自动恢复。

精确同命令重放只返回原结果，不再计数或创建问题；并发不同命令最多一个原目标CAS胜者。
新模型尝试照常收费账本计数和预算预留（本轮只有FakeModel），坏参数/迟到结果保留原响应及用量。
模型调用本身没有控制额度豁免；业务池到限或预算耗尽时保留待办，需既有Root检查点或显式所有者动作。

## 事件与恢复

拒绝检查和否定验收唤醒Dispatcher；change_task唤醒对应TL；升级问题交给用户。
用户答案只释放自己的等待代次，并产生同一任务的Dispatcher待办，不跳过新说明、UNKNOWN或AMEND。
缺少脚本、坏响应或预算不足不能提前确认该答案待办。原拒绝已委托用户时可确认旧通知，
有剩余额度的答案必须等新纠正真正落盘后才完成消费。任务alpha升级不阻断独立beta。

采用既有表和不可变events，不新增迁移、不改已发布SQL。纠正和升级的状态、计数、待办与幂等记录
在同一个事务中提交；事件写入故障回滚。没有引入新网络服务、业务进程或第三方依赖。
旧SQLite与v1/v2冻结上下文可继续使用；旧版本二进制不能理解新工具，这是向前运行时能力，不能降级重放。

## 当前边界

只接入同步有界EventRuntime与宿主Store；旧run-fake仍不能绕过待审核成果自动发说明。
按请求隔离恢复已在 R2.2 联合接入，见 [ADR-071](../adr/071-request-scoped-recovery.md)。
未实现由模型产出的AMEND处置、任意任务重定义、返工额度扩充审批、DAG或独立审核。
真实模型协议/模型判断质量和bwrap+cgroup强环境部署正向验收仍按原任务卡，绝不以Fake成绩代替。
