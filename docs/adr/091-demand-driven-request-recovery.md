# ADR-091：R2.4 按需请求恢复与写者会话隔离

日期：2026-10-29。目标蓝图 v0.4.1，承接 ADR-064/071，Store 410。
范围是离线开发和本地工程验证；不授权真实模型、付费调用或生产操作。
实际实现与门禁以 STATUS 和本批验证报告为准。

## 决定

`StoreRecoveryScope::request(id)` 在取得原数据库独占写者锁之后，在同一个
`BEGIN IMMEDIATE` 事务里读取目标请求、判定恢复需要并提交恢复。
不在 CLI 的只读预检中决定跳过，不新增并发写者或请求级分布式租约。
`AllRequests`、默认构造和显式 `recover_request(id, expected_version)` 保持强制恢复语义；
`AdmissionOnly`、ReadOnly 和带数据的结构升级边界保持。

需要恢复的事实是尚未围栏的操作（PREPARED/RUNNING/CANCEL_REQUESTED）、
PENDING 模型尝试、ACTIVE 事件领取或 CLAIMED inbox，以及未结束作业中
RUNNING/FAILED/非空 claimed_by 的经验或清理租约。
FAILED/CANCELLED/SUPERSEDED 若仍缺留存收尾事实，也走原恢复出口。
这些判定都按请求限定并检查当前实际支持的表版本。

普通 PENDING inbox、等待用户、未处置 AMEND、已围栏 UNKNOWN、无领取的 PENDING
经验/清理作业、PREPARED 分配意图和原 workspace 清理清单，本身不等于仍有效的执行权。
它们继续按原状态、对象 CAS、来源、UNKNOWN、工作区和原进程证据门禁处理。
恢复不会消费这些普通待办，也不从资源目录存在与否推导执行或清理成功。

## 会话与历史身份

每个 Store 实例给 `host_actor` 发行的 Actor 增加仅在内存中的写者会话绑定。
所有 Store Actor 授权路径，包括精确重放前的 `authorize(..., false)`，均核验本实例绑定。
旧 Store Actor 和 ReadOnly Store Actor 不能在另一个写者中仅凭相同 request ID、主体及
相同持久 epoch 获取权限。真实代次切换仍继续检查原 dispatcher/execution epoch。
WorkerTools 在绑定工作区和控制限流之前同样检查会话，防止外层将旧 Actor 换为当前身份。

新增 `writer_session` 字段不进入 `actor_json`、原执行 tuple、模型输入、事件命令、结算或历史指纹。
绑定值复用既有 `writer_instance`；原 live attempt/send claim 的该实例标识列继续按原格式
持久化，本批没有新增持久会话字段或把实例标识改成秘密认证凭据。
原 Actor 序列化只描述历史事实，不能反序列化为当前权限。可信宿主必须重新调用
`host_actor`；这仍是本地可信宿主 API，不是新增网络认证或防恶意同 UID 宿主的边界。
同一写者会话内原命令的精确重放规则保留；新会话须先取得自己的 Actor。

按需重开可能保留原 epoch，因此旧会话的内存故障不能仅凭相同团队/epoch 阻断新会话。
结算写入失败的进程内围栏键由 `(team_id, execution_epoch)` 细化为
`(writer_session, team_id, execution_epoch)`，在业务准入及首次派发领取前使用同一键，
故障登记与首次派发仍共享原互斥边界。相同 Store、团队和 epoch 内重新签发 `host_actor`
或重建 WorkerTools 不会解除围栏；合法 Root 核对持久 UNKNOWN 也不删除该内存标记。
新 Store 会话只隔离旧会话的局部故障，即使 epoch 没变也不会继承旧标记；持久 UNKNOWN
仍独立拒绝新业务，必须按原 Root 身份、请求归属、操作 CAS 和宿主证据要求合法核对。
旧 Actor 不能转为新会话权限，原 operation/result、输出和历史事实保持完整。

旧会话的内存标记不随换会话清除。故障表仍最多保存 4096 条；登记故障时表已满，
或无法分配故障记录，均置进程级 `settlement_capacity_exhausted`，拒绝进程内所有
WorkerTools 的新业务。
重签身份、重建 WorkerTools 或新建 Store 都不能解除这个全局标记，必须重启进程；
重启后持久未决操作、UNKNOWN 和原恢复/核对守卫仍生效。会话切换、核对成功及进程
重启都不证明业务进程已经退出，也不授予清理许可。

## 恢复与收尾

有恢复需要时仍使用 ADR-064 的原说明投影、epoch 围栏、取消 PREPARED、
未知用量预留、claim 释放及原收尾 job CAS。原说明、响应、反馈、输出和历史事件不改写。
终态只恢复既有独立收尾，不恢复业务权限。清理领取被回收时推进原 job lease/CAS；
旧清理 Actor 与旧 claim 分别拒绝。原进程退出证明、交付字节、资源所有权和不可扩展清单
仍在物理删除边界检查，恢复事件不构成进程退出或删除证明。

完整验证还复现了既有 WorkerTools 并发精确重放竞态：两个调用都先查询不到 operation，
迟到调用的 prepare 重放却可能拿到已派发或结算的当前版本，导致同一内部 start 命令
被原指纹守卫拒绝。最小兼容处理是在 prepare 返回非 PREPARED 行时只读返回原事实；
不能再次派生 start 意图。原 prepare 的身份、binding、工具权限及指纹检查保留，
PREPARED 的结算故障围栏、派发 claim 和 CAS 保留。确定性回归仅在测试 TU 内设置
调度观察点，并核对原行、事件、用量、真实文件身份和不同意图继续拒绝。

## 验收

独立 pristine 基线构建先执行同一回归源；记录失败断言、实际命令、退出码和原始日志。
修复后验证空闲重复打开的全表字节不变、真实未完成状态恢复、旧会话拒绝、显式恢复与
见证篡改、原模型证据、恢复事务回滚、两个写者竞争，以及实际交付后的清理租约/退出/所有权。
完整 Release 两种 umask、相关 ASan/LSan 与 UBSan、CI 对应 Clang 严格回归独立记证。
不得通过放宽期限、断言、权限、UNKNOWN 或修改发布 SQL 缩短验证。
结算故障回归还须实际注入结果写入失败，核对同会话重签/重建仍拒绝、在重新打开前后
分别合法核对 UNKNOWN、新会话拒绝旧 Actor，以及原事实、预算、文件身份和历史不变。
这些是实现及验收约束，候选红绿、完整门禁和发布是否完成以实际验证报告为准。
