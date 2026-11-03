# wls861 本地 Agent 下一阶段执行提示词（N5-0 / N5-1 / N5-3）

仓库：https://github.com/wmqfl861/wls861

## 1. 职责与权限
主协调 Agent 负责架构、优先级、独立审核与最终验收。你负责数据盘本地实现、构建、诊断、测试、正常提交/推送和证据归档。

不授权：合并 PR/main、强推、覆盖其他 worktree、真实模型或付费调用、生产数据操作、修改历史 SQL/v0.4.1/原失败日志、放宽权限/CAS/UNKNOWN/预算/deadline/断言/sanitizer。

## 2. 接手基线
- PR #25 产品候选：c2a5cea64aeea261b214cdfd9a968b53f491110a
- PR #25：https://github.com/wmqfl861/wls861/pull/25
- PR #26 报告：https://github.com/wmqfl861/wls861/pull/26
- 报告输入 f5d1fc77fb886a78bd80ccb901f7dc851ee00b7a
- PR #25 base：79654ca9cca80d5ae8f5204df580797b04963460
- PR #21 设计：4326ea5004889c4e14b31e26c6b86e6b0a8509b3（014/412 未授权）
- 当前 main：a6b8559018a599d7a806b05942857a56d69a00ce

先核验远端 PR/head/base、其他 Agent worktree、数据盘和未提交改动。源码/构建/临时库/私有证据放在实际核验的数据盘。不要只 pull main，也不要覆盖现有成果。

必读：
- docs/reports/n0-n4-final-validation-20261102.md
- docs/reports/n5-0-execution-validation-v2-20261102.md
- docs/reports/n5-1-persistent-event-store-20261102.md
- docs/reports/n5-persistent-event-process-crash-execution-20261102.md
- docs/reports/n5-windows-readonly-validation-20261102.md
- docs/implementation/n5-persistent-events-20261102/
- docs/implementation/next-cycle-20261102/ 三张卡
- docs/adr/094-persistent-event-call-association.md 及 adoption record
- PR #25 / #26 描述、机器索引、Actions 原日志
- AGENTS.md、STATUS.md、ROADMAP.md、Store/Runtime/Windows 当前契约。

## 3. N5-0：先处理验证与证据闭环（最高优先级）

### 3.1 Make-ASan IPC poll 四项失败
原结果为 Store 子程序 21 PASS / 4 FAIL，四项均命中原 5 秒 CHECK(polled == 1)。其余 74 条 Make 配方未执行。

对原输入建立单因素诊断：
- 记录 poll 的实际返回值、调用阶段、peer 创建/接受/读取/关闭顺序。
- 记录父子 PID/start_ticks、退出码、deadline、reap 状态。
- 区分 IPC 无消息、peer 提前退出、poll 错误、调度延迟和子进程生命周期问题。
- 原 5 秒期限和断言保持，不增加 sleep，不降低并发，不重复到绿色。
- 若找到生产缺陷，先同条件红例再最小修复；若根因仍不明，保持 OPEN。

### 3.2 ASan 38 timeout / 2 fail
按 suite 聚类，先取有代表性的 Store、runtime、CLI、TLS 失败。
保存完整私有 LastTest/JUnit、stderr、阶段日志、源码与 ELF identity。
不能用远端 CI、Release/UBSan、Make 或其他 SHA 替代本机 ASan。
不要无计划重跑完整矩阵。

### 3.3 hosted 私有证据
用户决定完整私有原件暂留本机。不要把私有 LastTest/JUnit 上传到公开 GitHub artifact。
先提交受限托管威胁模型和可选方案：
- 私有目的地及访问主体；
- ACL、retention、下载授权和审计；
- authenticated readback 与 SHA 校验；
- success/failure/timeout/interrupted 四种终态；
- 公开投影继续严格脱敏且不得声称等于原件。
若当前 GitHub 权限/保留策略无法满足，维持 BLOCKED，不擅自换外部存储或公开私有日志。

### 3.4 Windows image 来源冲突
同一 run/job/attempt/checkout 保存 header 与环境 ImageVersion 两份原始来源、字段来源、采集时间和可信度。查清 runner/平台字段生成机制。无法解释则 UNRESOLVED，不择一覆盖。

### 3.5 Compatibility02/03 sidecar
对旧 410 正确拒绝与 raw WAL/SHM inventory 失败分开处理。
检查 SQLite 连接生命周期、WAL/SHM 创建/关闭/checkpoint、文件身份、采集时点。
不得删除 sidecar、伪造 inventory、放宽原断言或更改历史迁移。
三 fixture control 只是有限机制证据，不等于严格兼容验收通过。

## 4. N5-1：持久 inbox runtime 强化

保持 013 / Store 411 和当前有限 FakeModel 范围。不要扩大 live、文件/进程工具、daemon 或第二 Store writer。

新增确定性 fault/crash 场景：
- T0 claim snapshot 前后；
- T1 reservation/link transaction 前后；
- T2 unique dispatch commit 前后；
- T3 response/usage settlement 前后；
- T4 business action / feedback / per-ID proof / finish / CLOSE receipt 前后；
- transaction commit outcome unknown；
- feedback 或 CLOSE 写失败；
- Store 重开与旧 Actor/claim；
- 部分 ID、迟到响应、AMEND/cancel；
- close 停准入、stop、join callbacks、最后释放 Store。

每个场景检查原 attempt、response、nullable usage、settlement、feedback、业务事件、operation、逐 ID receipts、open projection、claim/epoch、预算和兄弟请求隔离。

特别要求：
1. T4 所有业务 action helper、feedback、逐 ID proof、finish subset 必须在同一 SQLite transaction。
2. rollback 后不得存在业务事件、反馈、receipt、open projection 的半写。
3. 进程内 event_application_started/halted 只用于当前进程保护，不得作为跨重启授权。
4. 重启后只按持久 receipts 和原业务事实核验；缺证据/UNKNOWN 保持 HOLD，不重新派发原 attempt。
5. 精确 AMEND/cancel 重放无新 stop 副作用；真实状态转换只停止旧绑定。
6. 旧 Actor、旧 session、旧 claim 不得续权。
7. event_link_status 的快照、verify_event_link、metadata/tail capacity 增加损坏和边界测试。
8. 同步 API、旧 410 兼容、原权限、预算、CAS、claim 和 UNKNOWN 规则保持。

Store 411 migration 013 已经在当前产品候选中；不要重做迁移，也不要修改历史 SQL。新增迁移/014/Store 412 未获授权。

## 5. N5-3：Windows 只读身份目标平台准备与验收

先处理策略决策：
trusted_sids() 当前拒绝 Everyone/Users/Guests，但未显式拒绝 Builtin Administrators SID S-1-5-32-544。ordinary_token() 拒绝当前进程为管理员，并不能回答管理员组是否允许作为 trusted owner/writer。

请：
- 写清威胁模型和两种策略的后果；
- 优先建议将 Builtin Administrators 作为宽泛主体拒绝，除非主协调者明确批准其他策略；
- 建立策略单测与 ACE allow/deny 反例；
- 不擅自把该政策变更描述成已发现生产漏洞。

目标实机：
Windows 11 x64、NTFS、普通非提升用户、冻结 build/MSVC/SDK 和可信 SID。
实机检查祖先 HANDLE、对象 identity、ACL/reparse/hard-link、short-name/case alias、路径替换竞争、EOF/size/hash、token 变化和完整失败日志。
无 Win11 实机时保持 NOT_RUN；Server2022 elevated 不能代替。
不开放 Workspace 写、SQLite/VFS、进程、强沙箱或 cleanup。

## 6. N5-2 DAG 设计边界
PR #21 仅设计，014 / Store 412 未授权。先让主协调者独立审核 D1–D6：图版本、依赖凭据、artifact/submission 映射、producer/reviewer 双身份、父预算逐次借记、迁移和回滚。只提交选项、代价和验收，不实施迁移。N5-1 稳定后才开始三节点 a→c←b；之后才独立审核票据。

## 7. 执行和发布规则
- 每个方向独立 worktree，src/store.cpp、CMake、CI、公共头由单一集成人维护。
- 每项真实缺陷先在精确旧 SHA 建立确定性红例，再同条件修复绿例。
- 不把工具自检、内部方法数、CTest 注册数混算。
- GCC Release 两种 umask、Clang strict、UBSan、共享 GCC Debug ASan/LSan、Make-ASan、Windows 实机分别报告。
- 失败、超时、NOT_RUN、UNRESOLVED 保留原记录。
- 正常提交并推送独立 Draft PR；不合并 PR/main，不强推。

最终交付完整报告：精确 SHA/PR 依赖、修改文件、红绿复现、完整日志、各环境矩阵、私有/公开证据策略、Windows 实际 OS/token/filesystem/toolchain、剩余问题和后续计划。

分别登记 IMPLEMENTATION_COMPLETE、LOCAL_VALIDATION_COMPLETE、PUSH_COMPLETE、REMOTE_CI_COMPLETE、PR_REVIEW_COMPLETE、MERGED。

不调用真实 WLS 模型、不付费、不操作生产数据。先执行 N5-0，其他独立工作按依赖推进；需要架构采纳时提交选项等待主协调者决定。
