# N5-1 B 方案采纳与实现记录（2026-11-02）

本会话用户已明确采纳 PR16 中 B 方案以及完整的事务、逐 ID 证明、终身容量、兼容和回滚契约，
并授权新增 013 迁移与 Store 411。协调者将 Store 实现分配到
`feat/n5-1-persistent-inbox-20261102-csj777e9`，基于设计提交
`451b232a0dde08220a5e096d91209087b027b42e`。这是实施授权记录，不是独立代码审查、生产验收或合并批准。

PR16 发布时的 [PROPOSED 选择包](README.md)、[ADR-094](../../adr/094-persistent-event-call-association.md)、
[存储契约](storage-contract.md)和 [故障 oracle](crash-and-acceptance.md)保留其发布时状态；
本记录补充后续的采纳事实，不改写那些历史提案的时间范围。

实施采用六类新关系：请求启用政策、完整原 claim、call/attempt 关联、所选 ID、typed receipt、
未决 ID 投影。T0–T4 复用原 Store 校验和事务内步骤；T4 将支持的数据库 action、原 feedback、
每个捕获 ID 的处置和原 finish 子集放进同一事务。首片支持 FakeModel 独立 plan、instruction、
owner ack 和数据库 note，并保留原 AMEND 的显式子集语义。启用请求的旧预约、派发、业务和
finish 入口拒绝绕行；未启用请求和旧前缀维持 legacy 行为。

上限、不可变旧资产、410→411 显式维护、旧程序拒绝 411、UNKNOWN 与缺原件 HOLD、
close/join 后释放 Store 等边界按已采纳完整契约实施。新迁移不合成历史关联，不提供降级 SQL。
本任务不授权第二 writer、文件或进程业务、live/付费调用、daemon、生产数据操作或合并。

当前状态：**IMPLEMENTATION_IN_PROGRESS / LOCAL_VALIDATION_NOT_RUN**。
源码静态检查、构建、产品测试、sanitizer、独立评审、最终 SHA 的 CI 和发布分别记录。
编译与产品执行由协调者统一安排窗口；故障 oracle 中的 PLANNED 条目不能据此改记 PASS。
