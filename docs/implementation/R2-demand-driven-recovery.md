# R2.4 按需恢复与执行身份隔离

契约依据 [ADR-091](../adr/091-demand-driven-request-recovery.md)，承接
[请求限定恢复](S1-request-recovery.md)与[说明代次恢复](S1-epoch-recovery.md)。
目标蓝图仍为 v0.4.1，数据库仍为 Store 410；实际门禁另见本批验证报告。

| 入口 | 本批行为 |
| --- | --- |
| OneRequest / `StoreRecoveryScope::request(id)` | 独占写者锁、SQLite 写事务内判断；只有未围栏执行或失效领取需处理时运行原恢复 |
| 默认 AllRequests / `wls init` | 保留明确的强制全库维护，活动请求照常增加 epoch |
| `recover_request(id, expected_version)` | 保留原活动请求 CAS、强制 epoch/说明见证和失败回滚 |
| AdmissionOnly / ReadOnly | 原准入/只读边界；不提供执行接管权限 |

空闲的含义是没有原恢复事务需要处理的在途操作、PENDING 调用或遗留领取。
请求仍可有正常待办、等待、AMEND、原 UNKNOWN 或未领取的收尾；这些状态继续独立阻止
相应业务，不能因跳过恢复而跳过现有检查。

`StoreActor` 的会话绑定仅供当前 Store 授权。跨 Store 保存 Actor 的宿主需要重新通过
`host_actor` 获取身份；保存的原执行绑定仍用于原响应/输出记账，不能改盖当前版本。
新增会话字段不进入持久 Actor 事实和历史 hash；绑定值复用原 `writer_instance`，
原 live 预约/发送记录中的实例标识继续保留，不新增数据库会话列。
既有精确重放需要新会话的当前 Actor，
同一会话内的旧 epoch 精确重放仍按原规则只返回原结果。

结算结果写入失败的内存围栏按 `(writer_session, team_id, execution_epoch)` 记录，
替代仅有团队和 epoch 的键。业务早期准入、首次派发前复查和故障登记使用相同键；
故障登记与派发 claim 继续共享原互斥边界。其行为与持久 UNKNOWN 分别受以下约束：

| 执行上下文 | 新业务约束 |
| --- | --- |
| 同一 Store、团队和 epoch，重新 `host_actor` 或重建 WorkerTools | 会话键相同，继续受内存故障围栏阻断；合法 Root 核对也不清掉该标记 |
| 新 Store 会话，原操作仍是 UNKNOWN | 旧局部故障不跨会话生效，Store 的 UNKNOWN 守卫仍拒绝新业务 |
| 新 Store 会话，已在打开前或打开后合法 Root 核对原 UNKNOWN | 仍须通过当前 Actor/epoch、请求/任务、Workspace、CAS、权限及预算等原准入守卫，才可开始独立的新操作 |
| 进程级 `settlement_capacity_exhausted` 已置位 | 进程内所有 WorkerTools 的新业务继续阻断，换 Actor、WorkerTools 或 Store 无效，须重启进程并按原规则处理持久未决事实 |

失败围栏表最多保留 4096 条记录，旧会话条目不在换会话时删除；登记故障时表已满
或记录分配失败即置上述全局标记。新会话不要求为空闲请求人为推进 epoch，也不改写
原结果、输出、反馈或历史。旧 Actor 在新 Store 中仍被拒绝。新会话、Root 核对或
进程重启均不构成原进程退出证明，也不跳过资源所有权、清理租约或终态守卫。

WorkerTools 的并发 prepare 精确重放可能返回已被另一调用派发或结算的当前 operation。
这时只读返回原 operation/result，不再使用变化后的版本派生同一个内部 start 命令。
Store 仍先核验当前身份、binding、工具权限及原命令意图；PREPARED 路径继续执行原
结算故障围栏、首次派发 claim 和 CAS。非 PREPARED 返回不推断 UNKNOWN、取消或退出。

所有业务、管理、模型准入、派发、反馈、结算、工作区、清理和终态守卫保留原入口。
本批不新增 SQL 迁移，不执行模型供应商请求，不扩大清理授权或并发写者范围。
结算故障会话隔离的候选红绿及完整门禁须另行记录实际结果；本契约不声明测试或发布完成。
