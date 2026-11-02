# wls861 独立审核与下一阶段交接报告

审核日期：2026-11-02 UTC
对象：PR #15，aa2c592b8aed6509189710cc6f6d69e0959098a8
仓库：https://github.com/wmqfl861/wls861

## 1. 总体结论

**VALIDATION_BLOCKED；PR #15 保持 Draft；不批准合并或发布。**

原 R-A3-01（AMEND/cancel 精确重放错误停止新调用）与 R-A4-01（跨池资源 FIFO 循环等待）在 PR #11 中已修复，并在本轮限定范围内复核为 VERIFIED_FIXED_SCOPED。N1 SSE 变更范围小，原 14 项检查条件/限额/错误语义保持；N2 增加 AsyncCoordinator/Store 回归并改善 GCC ASan runner 证据；N3 Windows 组件证据改善但 Win11 普通用户实机仍未验收；N4 三卡仍是设计提案。

不能批准合并的原因：
1. 本机完整 UBSan 与 ASan/LSan 门禁仍失败。
2. hosted 完整私有 CTest 原件没有持久化副本，公开投影无法重建原始日志。
3. Windows image header 与环境 ImageVersion 来源冲突未解决。
4. 本机 Make-ASan、Clang ASan、Windows 11 普通用户实机验收未完成。

## 2. 精确输入

| 对象 | SHA | 含义 |
|---|---|---|
| PR #15 head | aa2c592b8aed6509189710cc6f6d69e0959098a8 | 当前审核对象 |
| N0 双父整合 | f3fc9bfaf345030ee994097d2a2443e3c290e9fc | 第一父 PR14，第二父 PR13 |
| 本机正式矩阵输入 | 7bf3a16d9683ed6a8de65d88023bae9687abd20d | 本机 92 项矩阵实际输入 |
| Windows 后继夹具输入 | b58f7b0e63ad68f67f124eddd303825ade817791 | Windows 夹具/报告后继输入 |
| PR #14 | 4289589384d75f29d42233cbaad256ceb711e9bf | 文档/报告分支 |
| PR #13 | ceaa9b3e18618c592345001dfc597bdaf579cf81 | billing 即时诊断分支 |
| main | a6b8559018a599d7a806b05942857a56d69a00ce | 未包含这些未合并成果 |

PR #15 base 是 PR #14，不是 main。PR #13/#14 原先为兄弟分支；N0 本地正常整合后形成双父候选。不得把临时 PR merge checkout SHA 当分支 head，也不得把不同 SHA 的测试相加。

PR #15：https://github.com/wmqfl861/wls861/pull/15

## 3. 本机正式矩阵

矩阵输入为 7bf3，不是最终文档提交 aa2 的本机重跑。

| 配置 | Passed | Failed | Timeout | Skipped/Notrun |
|---|---:|---:|---:|---:|
| GCC 13.3 Release umask 0022 | 92 | 0 | 0 | 0/0 |
| GCC 13.3 Release umask 0002 | 92 | 0 | 0 | 0/0 |
| Clang 18.1.3 Release umask 0002 | 92 | 0 | 0 | 0/0 |
| GCC Debug UBSan | 66 | 2 | 24 | 0/0 |
| GCC Debug ASan/LSan | 56 | 0 | 36 | 0/0 |

矩阵父进程实际退出码 1，collector COMPLETE/FAILED；源码和二进制身份在测试前后保持一致。独立 Make-ASan、Clang ASan 本轮 NOT_RUN。工具自检不是新增产品 CTest，不得与 92 项混算。

## 4. 审核发现与整改

### V-01 / P1：本机 sanitizer 失败
- UBSan HTTPS partial upload：peer.prefixes 实际 0、预期 1；原 3500ms timeout 与 900000 padding 不得放宽。
- ASan live_model_store_tls：容量子进程原 18 秒期限超时；大 escaped output 原 16 秒断言失败。
- UBSan structured CLI：原 50 秒子进程超时。
- 多项 runtime timeout 需阶段级证据，不得一概归因于 CPU、ext4、编译器或调度器。

按单个失败建立可证伪假设和最小对照，区分夹具、传输、性能回归、死锁与环境资源。保留原参数、期限、断言和失败日志。无确定根因就保留 OPEN，不得重跑到偶然绿色。

### V-02 / P1：hosted 私有 LastTest/JUnit 未持久保存
runner 在 job 内生成完整私有日志，但远端 artifact 只有审查后的公开投影。公开投影不能重建被省略的原始字节。

增加独立、限权、短期保留的私有证据 artifact 或受控存储，只允许授权审阅者访问。公开 artifact 仍只上传严格投影。success/failure/timeout/interrupted 四种终态均须保存原件、SHA、命令退出和前后身份。不得公开私有原件，也不得称投影等于原件。若权限/保留策略不满足，报告阻塞。

### V-03 / P1：Windows image 来源冲突
捕获同一 run/job/attempt/checkout 的两个字段来源、原始文件、采集时间与可信度。不能解释则 UNRESOLVED，不得择一覆盖。

### V-04 / P2：历史 sanitizer 根因
历史 GCC/Clang/live_workflow 根因仍 OPEN，historical_clang_asan_failures_closed=false 保持。新 SSE 改动或远端成功不是历史根因修复证明。

### V-05 / P2：异步异常组合覆盖
adapter 抛异常且存在同资源跨池后继的组合已增加覆盖。继续检查异常退出、保留结果、关闭交错、Store owner 生命周期和控制可达；只补缺失真实组合。

## 5. N4 三卡评价

三卡均为 DESIGN_PROPOSED / IMPLEMENTATION_NOT_STARTED，验收 PLANNED / NOT_RUN。

- N4-A：持久 inbox→attempt 关联、部分 ID 完成、崩溃恢复、精确 AMEND/cancel、UNKNOWN 与 retained 事实。方向正确，但先决定追加事件事实还是新附加表/迁移；内存 future map 不足以支持崩溃恢复。
- N4-B：最小 DAG a→c←b、版本化图、候选与 artifact 映射、producer/reviewer 双边票据、有限返工。M1 Journal ReviewRecord 或 WorkClass::Review 不是 S1 独立审核授权。先冻结图版本、持久依赖凭据、审核身份和预算归属。
- N4-C：Windows 11 NTFS 普通用户只读根对象身份、根 Handle、祖先固定、ACL/reparse 竞争和公共资源捕获。Server2022 elevated 不等于目标平台验收；不支持的 Workspace/Store/进程/沙箱能力必须拒绝。

## 6. 下一阶段计划

### N5-0：集成输入与证据闭环（最高优先级）
冻结包含 PR13/14 的唯一集成 SHA；补齐本机 UBSan/ASan 阶段日志与最小因果实验；修复 hosted 私有证据持久化且保持公开投影隔离；解释或保留 Windows image 来源冲突；对修复建立红绿和最终 SHA CI。

退出：本机受影响门禁通过，或每个失败项有确定原因并提交协调者决策；私有证据生命周期完整；Windows 来源一致或明确 UNRESOLVED。未满足不得发布。

### N5-1：N4-A 持久 inbox 异步纵向闭环
主协调者先采纳事件关联持久化方案，再实现最小两个请求、每请求两个节点的 FakeModel 流程。原 claim/attempt/settlement/feedback/逐 ID ack 绑定；崩溃后不重派、不吞事件。首版不接文件/进程工具、live 网络或后台 daemon。

退出：真实 Store/EventRuntime 链的 plan/instruction/ack/note、部分完成、崩溃窗口、UNKNOWN、AMEND/cancel 和旧 Actor 负例通过；同步 API 兼容。

### N5-2：N4-B 最小 DAG 图事务
仅在 N5-1 持久关联方案稳定并获采纳后实现图版本和三节点汇合。先图发布、无环/容量/依赖证明、失败传播；后独立票据与审核子请求。保留旧 Schema/SQL 原字节；新增迁移须单独设计和批准。

退出：真实 Store 原子提交/失败零半写、旧图版本不可冒用、依赖失败/UNKNOWN 正确阻断、恢复不重派。

### N5-3：N4-C Windows 只读身份切片
冻结 Win11 build、MSVC/SDK、NTFS、普通非提升 token 与受信管理主体。实现根/祖先 Handle、文件身份、ACL/reparse 拒绝、公共资源完整捕获。缺能力拒绝，不开放 Workspace 写入、SQLite、进程或沙箱。

退出：Win11 普通用户实机正反例、路径替换竞争、对象身份/完整 EOF/hash、完整 LastTest 和非作者权限复核通过。

### N5-4：之后再评估 S2 与真实 S1
N5-1/N5-2 完成后才扩大 S2 DAG/独立审核。真实供应商质量/费用验收仍需用户明确端点、精确模型、可信计费来源、预算和调用授权。不得从持续开发授权推导付费授权。

## 7. 权限与发布边界
- 不写 main，不合并 PR #15 或其依赖。
- 不强推、不覆盖其他 Agent worktree。
- 不改已发布 SQL、v0.4.1 历史蓝图和原始失败日志。
- 不放宽权限、deadline、断言、UNKNOWN、预算或 sanitizer。
- 不调用真实 WLS 模型、不产生付费请求、不访问生产数据。
- 每模块分别登记 IMPLEMENTATION_COMPLETE、LOCAL_VALIDATION_COMPLETE、PUSH_COMPLETE、REMOTE_CI_COMPLETE、PR_REVIEW_COMPLETE、MERGED。
- 本地 Agent 负责实现、测试、提交和推送；主协调者负责架构采纳、独立审核与最终验收。

## 8. 本轮状态
IMPLEMENTATION_COMPLETE=true（仅限 N0–N4 声明范围）
LOCAL_VALIDATION_COMPLETE=false
PUSH_COMPLETE=true
REMOTE_CI_COMPLETE=true（仅限 aa2 对应远端 7 workflows / 17 jobs）
PR_REVIEW_COMPLETE=false
MERGED=false

本报告不授予合并、真实模型、付费或生产操作授权。
