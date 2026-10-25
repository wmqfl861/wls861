# R2.1 被拒提交的有限返工与用户出口

版本 **0.8.0-r2.1**，蓝图 v0.4.1。设计：[ADR-070](../adr/070-bounded-rejection-followup.md)。
本页补充 S1 问答/验收契约；原拒绝决定仍只使提交BLOCKED，不自动执行返工。

## 输入和捕获

仅在已接入验收的Store中启用。Dispatcher宿主选择当前BLOCKED任务及其唯一有效BLOCKED提交，
附原提交/检查/最近拒绝、任务与提交CAS、原绑定、冻结上限和累计次数。两种请求格式都使用
同一个管理接口，不修改旧冻结配置。Worker窗口和原证据读取不改。

模型参数仅为 `{action: "rework" | "ask_user", reason: "..."}`。禁止目标ID、版本、
新任务Brief、豁免、额度等额外字段。一次管理调用仍只有一个捕获动作；调用预留、响应结算、
原反馈和迟到处理沿用原账本。模型不获得业务文件/进程权限。

## 返工事务

Store重查当前Dispatcher身份、请求/执行/政策代次、原提交语义绑定、任务和提交CAS、有效计划、
团队ACTIVE及当前等待/阻塞问题。返工还要求所属团队操作与模型用量已结算；UNKNOWN不自动重试。
读取已冻结 `team_internal_rework_max`，与TeamLead检查复用 `team_workflow_state.rework_count`。

有额度时增加任务修订，将原提交标记RETURNED_FOR_REWORK，暂停团队，增加累计次数，并保存
原source/reason/next_binding的不可变事件。原Brief、需求、计划、验收标准、旧文件字节和消耗不变。
事件通知TeamLead；其上下文包含原拒绝及修正理由。新说明独立增加instruction_revision，
Worker重新ack后才允许业务。旧说明无法确认新任务修订。

这是一种受限change_task，不支持任意改写Brief、拆团队或增加预算。所有者AMEND仍要求逐任务
显式处置，且不重置这个团队累计次数；耗尽时可取消或在明确的新请求授权下重新开始。

## 用户问题

每份被拒提交最多创建一个Dispatcher→user的原生阻塞问题。同一事务保存任务原绑定、原提交
哈希、问题和精确等待代次。上报不需要假装Worker提问；仍受问题数、控制事件、模型调用和预算限制。
现有 `wls questions` 查询后，使用原 `wls answer --question ... --expected-version ... --text-file ...` 回答。

答案只完成该问题的等待，不修改任务修订、额度、UNKNOWN或AMEND。若还有次数，Dispatcher
重新捕获原提交与答案，选择有限返工；若耗尽，保持BLOCKED供所有者AMEND/CANCEL/NEW，
不重复问同一个问题或自动再调用模型。普通wake不能代替答案。

## 事件、恢复和幂等

否定验收为Dispatcher保存独立待办。返工只完成对应提交；上报只有精确对应submission_id的
已保存问题才可消费原拒绝通知，不能拿同任务的其他问题冒充。答案尚未完成返工时待办保留；
缺脚本、预算、调用上限、存储错误都不意味着处理完成。一个任务的问题不阻止兄弟任务验收。

命令重放返回原结果，不重新增加次数或创建问题；不同内容复用命令ID被拒。最终事件写入失败
回滚任务、提交、计数、问题和等待。原反馈/来源不可变，旧响应仍计量且不能写回新任务。
重开沿用既有代次围栏且不清零；本切片不改变Takeover仍恢复全库活动请求的范围。

## 容量、兼容和边界

返工处置计入业务事件池；用户问题计控制池但没有无限结算豁免。实际模型预约仍受已有capacity
暂停保护，需要Root检查点时不从新动作绕过。旧schema、SQL、vendor和原审核字节未改，
新增一份模型动作Schema；已有工作流表和不可变事件足以保存本切片，无数据库升级。

测试以FakeModel驱动真实临时SQLite、文件及跨进程CLI。不证明模型自主判断质量、并行收益、
供应商调用或强沙箱部署。下一R2切片为按请求恢复、通用任务变更和模型AMEND处置。
验证结果见[本批报告](../reports/r2-rejection-followup-validation.md)。
