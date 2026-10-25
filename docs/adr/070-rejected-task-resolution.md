# ADR-070：当前拒绝成果的有限纠正与用户升级

日期：2026-10-25。目标蓝图 v0.4.1，基线 `19ea603`。属于 R2 的第一切片；
按请求隔离恢复与模型 AMEND 处置单独实施，不修改已发布 SQL 或原审核材料。

## 决定

否定 Dispatcher 验收与 TeamLead 返工耗尽保留当前 `BLOCKED` 成果、原字节和原判断。
Dispatcher 从宿主捕获的同一成果（任务/成果 CAS、当前请求 binding）选择：

- `change_task`：仅开启纠正修订，模型提供非空 rationale/correction；宿主保持原 TaskBrief
  目标、验收项、约束及请求/计划版本原样，增加 task_revision 并清除 applied 版本。
  原成果变为 RETURNED_FOR_REWORK；TeamLead 读取修正理由并发布新 WorkOrder，Worker 重新 ack。
  这不是任意重定目标、创建新团队或放宽验收标准的通用变更接口。
- `escalate_task`：向用户创建一个带原绑定的阻塞问题和精确等待，原任务仍 BLOCKED。
  只有 Root 接收用户答案；答案仅结束该问题的等待，不自动通过验收、返工或追加预算。

TeamLead 和 Dispatcher 的纠正共用既有 team_workflow_state.rework_count 及请求冻结的
team_internal_rework_max。重开、重发说明、回答、AMEND 和容量检查点不重置计数。
可用次数为0时不向模型声明 change_task；Store最终检查仍拒绝。每个拒绝成果最多升级一次，
更换 command_id 不得制造重复用户问题。已回答而额度耗尽时保持明确的 AMEND/CANCEL 提示，
不再自动唤醒模型反复提问。用户回答后有剩余额度时允许新的当前 Dispatcher 决定。

## 授权、原始证据与事务

动作不套用 Worker 业务就绪门禁，但必须核真实 Dispatcher、当前请求/政策、目标归属、
任务与成果 CAS、原生产语义版本及活动团队。真实 AMEND 未处置或旧调用代次均拒绝。
change_task还要求本团队操作/调用结算，无WAITING或未答阻塞问题；UNKNOWN不能借返工消除。
escalate_task保留只上报的控制通路，不修改操作结果或伪造进程退出。

状态、共享计数、命令幂等结果与事件在同一事务；失败回滚。精确旧命令只重放原结果。
原拒绝验收、TL检查、原说明、原文件及调用反馈不改写。原 SQL 表已能表示上述事实：
纠正和升级的额外关联存入不可变事件，问题复用既有 Question/等待结构，无新增迁移。
所有模型尝试仍预留并结算预算；缺脚本/错误/唤醒额度到限保留具体待办。

## 验收

先在基线上复现否定验收后再次pump为0次调用。新回归要求拒绝→显式change_task→
TL新说明→Worker ack/新提交→重新验收；失败/耗尽→用户问题；回答不重置返工计数。
覆盖重放/重开、错误身份/任务/CAS、迟到响应、AMEND、UNKNOWN、事务失败与兄弟任务继续。
普通用户 Release/UBSan/ASan/Make运行完整门禁；Fake判断、真实系统效果与未做的真实模型
验收分别报告。不新增弱隔离、付费调用或内容清理。
