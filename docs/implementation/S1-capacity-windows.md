# 冻结存储准入窗口与可恢复容量（ADR-066）

本契约说明 2026-10-24 审核 P1-2 的实现；不代表 R1 上下文压缩、完整 R2 或 S1 已完成。
已发布 SQL、冻结快照、原事件、命令和操作结果不改写。新 API 没有新增表或 schema 版本。

## 配置与旧请求

新配置在 `system.limits.store_capacity` 明确给出下表全部 11 个整数。
显式块不得缺项/多项，每项 1..10,000,000，事件池至少 64。整个块缺失表示旧请求，使用
下表历史值；恢复绝不读取新 YAML 补值，也不把默认值写回旧快照或改变其哈希。

| 字段 | 新样例 | 缺字段的历史等价值 |
| --- | ---: | ---: |
| business_events | 32768 | 4096 |
| control_events | 32768 | 8192 |
| operations | 4096 | 1024 |
| questions / open_questions | 128 / 32 | 128 / 32 |
| submissions_per_task / submissions | 64 / 256 | 64 / 256 |
| acceptances / adverse_acceptances | 256 / 512 | 256 / 512 |
| deliveries | 64 | 64 |
| requirement_revisions | 100 | 100 |

原共享事件总数错误已修正为独立业务/控制计数；保留的是两个历史数值，不保留互相挤占的缺陷。
准入估算 `N=worker_max_tool_calls_per_order * max_teams_per_request * (team_internal_rework_max+1)`。
operations 至少 N；每个事件池至少 `3*N + teams*(rework+1)*manager_max_tool_calls_per_wake + 32`。
因此覆盖每次工具约 3 条准入/派发、3 条结算/反馈及管理余量。提交/验收容量也必须覆盖冻结团队和
返工轮数。显式配置在加载时即校验，Store 创建新请求时再次校验；旧准入幂等重放先于新准入一致性
检查。旧快照能读取/恢复，不意味着其不一致参数可以再用于创建新请求。

## 独立计数、预留与暂停

业务和控制按命令类型独立计数。recover、恢复待办引导、capacity_warning/hold/checkpoint 是
单独的宿主维护审计通路，不占业务或控制池。业务工具/模型不能把命令改报成控制或维护。

新准入保留已 PREPARED 操作的派发槽，及每个未决操作/模型尝试的控制余量。所有已承诺的派发、
结算、取消、ack、宿主核对保持原状态/身份/版本门禁，不因新准入水位而丢失收尾能力。
容量是准入水位，不是物理磁盘配额或对宿主恢复次数的无限存储保证；必要控制尾部允许使用预留，
绝不能在副作用后用存储配额代替真实结算。磁盘满和写入失败仍按原 UNKNOWN/同进程围栏处理。

接近水位时，在同一事务写一次 capacity_warning；达到资源上限或剩余槽不足以安全接新工作时，
再写一次 capacity_hold。查询返回持久容量状态 `WAITING_INPUT`；它是独立的准入暂停，不篡改请求的
业务状态。`pending` 列出 capacity_holds。新普通动作返回带恢复指引的 not_ready，pump 不再预约
新模型调用。既有操作 start/finish、取消、核对仍须通过原安全门禁。open_questions 是当前未关闭
问题数，不能通过窗口切换清零；回答或转发现有问题不被业务池耗尽阻断。新问题/等待只消耗
独立控制池及其预留，业务池到限不会占用它们的槽；控制池到限停止新的问题/等待。已捕获的
经验管理动作可沿原预算受限控制通路完成，不让容量暂停挡住检查点所需的结算。

计数按请求/窗口查询。写者缓存不可变政策和事件计数；事件提交递增、事务回滚全部失效，外部
SQLite data_version 变化也失效；只读连接不缓存动态计数。缓存有 1024 请求边界，不代替持久事实。
这是局部计数优化，未移除 quick_check，也未实现按请求写租约或完整解析性能优化。

## Root 检查点

`wls capacity --db DB --request REQUEST` 为只读，不接管写者。

`wls capacity-checkpoint --db DB --request REQUEST --expected-window N --evidence-file FILE --key KEY`
要求受信宿主 Root、非空证据、独立窗口 CAS、已发出告警/暂停，以及所有操作与模型用量已结算。
CLI 先只读预检，取得写者后 Store 再检查；错误 CAS/空证据/未知副作用不会提前围栏其他请求。
精确重复 key 返回原结果，换证据/对象/CAS 冲突。事件保存旧窗口计数、各类累计水位、原冻结上限和
新窗口号；事务失败不改变计数或暂停，重启从该原记录恢复。

检查点不删除/归档审计行，所有 command_id、command_sha256 和 result 仍可精确重放；既有成果、
旧说明和操作也保留。只分段新工作准入的计数，不改变 requirements/plan/task/instruction 修订、
工具每说明消耗、返工次数、模型尝试/费用/未知用量、AMEND 处置、等待或 UNKNOWN。
未处理 AMEND、工具额度耗尽、未知副作用不能借此恢复业务；原有门禁照常拒绝。

## 验证和边界

原 event_cap 程序已转为 Make/CTest 回归：1100 次操作及 3600 次额外控制后仍可发布新计划。
另测小窗口告警/暂停、Root 检查点、重开重放、缓存回滚、旧快照、错误配置、未知效果/费用与 AMEND。
这些是 Store 和 CLI 的真实临时 SQLite 验证；工具效果在长计数用例中是明确标注的合成结果。

原始历史继续增长，本实现不声称完成物理归档、磁盘保留策略、内容寻址、并发服务化或内存压缩。
单次 JSON/上下文/输出字节、256 条历史重建和工具文件边界未在本批放宽；R1 仍须解决上下文触顶。

## R2a.2 分配续进

[R2a.2](R2a-allocation-recovery.md)在408能力启用时新增独立 owned_allocation 诊断与控制预留。
v2 PREPARED 无收据预留2、已有收据预留1、旧v1 PREPARED预留1。新分配在文件效果前检查，
原分配的收据/登记/弃置沿控制结算续进；新增事件名称不重分类原历史事件。
64有效/256历史是终生上限，不进入可清零窗口totals；旧checkpoint和冻结政策原值保持。
