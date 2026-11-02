# ADR-094（提案）：持久 inbox 与原调用的完整关联

日期：2026-11-02 UTC。状态：**PROPOSED / NOT_ADOPTED**。
读取基线：`aa2c592b8aed6509189710cc6f6d69e0959098a8`，Store 410。
本地执行 Agent 提出；架构采纳、存储批准和最终验收属于主协调者。

N5-1 要求两个请求各两个独立节点的 FakeModel plan/instruction/ack/note 在单 Store owner 下
重叠等待，并在崩溃后逐 inbox ID 追溯原 claim、attempt、业务效果与反馈。现有 claim 会覆写，
attempt/feedback 未保存 ID 关联；内存 future 和仅当前 plan/instruction 不足以补齐此证明。

建议选择附加不可变 claim/link/member/receipt 事实、开放 ID 唯一索引和 410→411 显式迁移。
原 SQL/Schema/hash、Actor 的临时 writer_session、预算、UNKNOWN、首次派发与旧同步默认不变。
新模式为显式请求选择，旧程序因未来版本拒绝，不能静默运行旧 pump 消费新关联。

协调者选择：

| 选项 | 具体交付代价 | 必须通过的验收 |
| --- | --- | --- |
| A：追加事件事实 | 新 typed 事件协议／有界折叠、唯一未决 ID 校验、事件尾部容量；完整 Worker 范围还须最小 411 元数据门禁，不能只称“无迁移” | 每 ID 唯一未决关联、全部崩溃窗口、不重派、完整 binding、旧客户端拒绝／有限扫描 |
| B：附加表＋013/411（推荐） | 六个附加关系、唯一索引／不可变约束、新资产注册与内部事务提取；升级后不能直接回旧二进制 | T1 预约关联零半写、T2 唯一派发、T4 效果／反馈／proof／finish 零半写、迁移失败回滚、旧库解释、启用模式防绕过 |

同时需采纳的最小约束：只启用无旧 attempt/ops 的新请求；原 64 result／16 control、4/8 池及 1/1
reserve 不扩大；新关联终身≤64、claim snapshot≤128、每 claim≤64 IDs，原冻结上限更小时从小；
收尾逻辑字节与原 business/control 尾部在预约时预留，物理存储失败仍保留原件并停新业务。
不批准迁移时只做只读管理完成分类辅助，缺关联 Worker 明确 UNLINKED_HISTORY，不宣称 N5-1 完成。

备选是把新事实写进原 `events`，但仍须原子关联、严格折叠、容量和旧客户端门禁。
纯 410 追加无法阻止旧程序忽略新协议；完整备选至少需要元数据／411 门禁。
比较和具体协调者选择见 [选择包](../implementation/n5-persistent-events-20261102/README.md)。

提议事务为：T0 原 claim+不可变快照；T1 原预约+link；T2 原唯一首次派发+receipt；
T3 原不可变结算+receipt；T4 数据库内业务效果+原反馈+逐 ID 证明+原 finish 子集同事务。
callback、模型等待和文件／进程执行均不进入事务。T4 需要经过审阅的内部步骤组合，不能
嵌套现有公开方法的 BEGIN，也不扩展到 WorkerTools 或 live。旧 API 默认路径保持原语义。

恢复只读原事实并核当前 owner；旧 Actor/claim/attempt 不续权、不重新派发。
缺 settlement/feedback/确认、UNKNOWN 或投影冲突保持 HOLD；迟到原响应只记账。
新请求的 T4 失败必须保证业务尾部和反馈零半写；旧路径已提交效果而反馈缺失仍明确阻塞。

容量、旧 410 解释、无原地降级的回滚、逐窗口验收和 N5-2 依赖以
[事务契约](../implementation/n5-persistent-events-20261102/storage-contract.md)及
[故障 oracle](../implementation/n5-persistent-events-20261102/crash-and-acceptance.md)为提案不可分割部分。
迁移文件名、表名和拟议签名不是现有接口或已批准修改。N5-2 仍等待方案稳定和单独排程，
图／独立审核／真实模型／付费／第二 Store writer／生产数据／合并均未因此获准。

本提案没有执行产品测试或实现生产行为；全部新增验收 PLANNED / NOT_RUN。后继需独立审核、
精确提交适用门禁和协调者采纳记录。未采纳时不升级数据库，也不将管理辅助称为完整 Worker 闭环。
