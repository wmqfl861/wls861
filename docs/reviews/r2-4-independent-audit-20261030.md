# R2.4 交付源码独立工程复核（2026-10-30）

审查基线为 `a6b8559018a599d7a806b05942857a56d69a00ce`，实际交付源码为
`55dc989e02f7e3e2cd1806e53d47298dd8742376`。本轮在数据盘独立 worktree
`/mnt/d/codex-workspace/wls861-blueprint-20261030/feature` 只读检查生产实现与测试，
没有编译、执行测试、调用供应商、访问生产服务、修改 Git 引用或操作 PR。
审查者为与实现执行者分开工作的 `independent_recovery_review` Agent，仍属于同一模型体系。

**结论：所列十一项范围的静态复核完成，未确认阻断性生产代码缺陷。**
这不是全项目无缺陷证明，也不把阅读测试等同于本轮运行通过。
[R2.4 PR #1](https://github.com/wmqfl861/wls861/pull/1) 的地址及创建状态由本地执行主 Agent
提供；本审查者未访问远端核验。主协调 Agent 的正式 `PR_REVIEW_COMPLETE` 保持
`pending`，`MERGED=false`。工程复核不能代替该正式批准。

## 输入、方法和可信边界

先按安装的 `codebase-memory` 技能查找恢复和授权关系，再检查路径覆盖，最后直接读取
实际源码和所有相关 `.inc` 的守卫。图项目为
`mnt-d-codex-workspace-wls861-r2-4-20261029-feature`，索引代次
`2026-10-30T02:07:47Z`。图指向先前交付 worktree；本轮逐字节核对了实际引用的
16 份源码/测试与该 worktree、交付提交，全部相同，随后才使用其结构线索。

`check_index_coverage` 一页返回全部 19 个请求路径，`path_has_more=false`。
其中 `store_cleanup.inc`、`store_live.inc` 整体不可解析；Store 头文件、`store.cpp`、
capacity/history/rework/single_agent 内嵌实现及两个测试文件有部分解析缺口。
`store_workflow.inc` 和 `store_events.inc` 两个探查名称实际不存在：工作流和事件代码
位于 `src/store.cpp`，没有把图的 missing 结果当作实现缺失。补充覆盖查询及实际路径
记录在机器证据中。`needs_recovery` 图中对 `optional` 的一个调用错误解析到配置加载
辅助函数；实际依赖是 `Store::Impl::optional` 的 SQLite 查询，以源码为准。
本轮不声称全项目图覆盖。

Store 是**受信本地主机 API**。`StoreActor` 的字段公开，`host_actor` 是受信宿主的
签发接口；新增 session 检查防止保存的旧 Actor 意外跨 Store 复用，并非对任意同 UID
恶意代码或同进程攻击者的密码认证。已经能够调用当前 Store 的受信宿主可以重新签发
当前 Actor，仍须经过原 owner、scope、epoch 和业务守卫。不能把公开 session 字符串
描述为安全秘密，也不能声称任意数据库所有者不能直接重写事实。

本轮全文语义阅读了交付提交中的 `AGENTS.md`、26 份实际 ADR、`docs/reviews/`
全部 10 份原文本文件（7 Markdown、2 C++ 反例、1 patch）、分配的六份恢复/清理契约，
并重读 `S1-worker-tools.md`，共 44 份文件。用 `cat` 或覆盖全行的有界 `sed` 阅读；
输出截断处补读。逐文件实际路径、原提交 SHA-256、阅读类别与目的在任务目录
`evidence/r24-code-review.json` 的 `full_document_reads` 中，哈希不是语义阅读的替代。
并行文档工作随后修改的 `AGENTS.md` 不混入该原始阅读身份。

源码采取补丁全文及指定函数/守卫的定向阅读，测试采取三个 R2.4 文件全文与
WorkerTools、原 R2.2 相关回归定向阅读；未声称逐行审核整仓全部源码或测试。
核心交付身份如下，其他引用输入摘要在同一机器证据中：

| 文件 | SHA-256 |
| --- | --- |
| `include/wls/store.hpp` | `237eeb533e060d04666353fb0197ef64af8bd324944874f75898a0ce809f046d` |
| `src/store.cpp` | `f196be42cd740f9484bf2f8b39f8ea668dd35ce6779ade31d4c59145216b3d7c` |
| `src/worker_tools.cpp` | `aedf1cfc9c428ae74b68da7922a57558996733ebe08832f809fa8c15467b689a` |
| `tests/worker_tools_tests.cpp` | `9abbac0b086d8e037ca3679fbe4e3c6f40e700afbcf331de8e790386c288e8ce` |

## 十一项检查

以下行号均指 `55dc989e…` 的实际源码。每项结论均为静态判断；所列测试表示已阅读的
对应断言，不表示本审查者重新执行。三份 R2.4 文件的 17+6+3 子例是**三个套件、26 子例**；
WorkerTools 的 late-prepare 与 session 结算故障是原套件的两个新增子例，18→20。
同一子例的两个核对顺序没有重复算作套件。

### 1. 按需判断位于原写者锁和同一写事务

`src/store.cpp:466–474` 在任何恢复前持有数据库级 `LOCK_EX | LOCK_NB`；
`712–718` 的 `BEGIN IMMEDIATE` 同时包含 `request_row`、`needs_recovery`、原恢复及
`COMMIT`，异常调用 rollback。没有事务外缓存判定，也没有仅在 CLI 外层跳过。
`789–800` 的普通写事务继续检查 Takeover/AdmissionOnly/OneRequest；
`806–818` 的请求、任务、团队和操作行仍核对请求范围。

对应 `tests/r24_recovery_tests.cpp:305` 的 `simultaneous_open_keeps_exclusive_writer`、
`:322` 的 captured prepare 竞争、`:333` 的 claim 竞争及 `:343` 的全表回滚检查。
普通并发写由同一 Store mutex 串行；跨实例仍是全库唯一写者。限制：这些竞态子例不
穷尽所有调度，且不是按请求多写者或恶意绕过 flock 的验证。

### 2. 空闲重复打开不推进 epoch

`src/store.cpp:1434–1450` 不把普通待办、WAITING、AMEND、已围栏 UNKNOWN、
没有持有人/失败状态的 PENDING 收尾或裸分配意图算作执行租约；`716` 只有判定为真
才调用原恢复。原 epoch 写入仍位于 `1468–1505`，并非删掉 fencing 检查。

对应 `r24_recovery_tests.cpp:157` 连续四次打开逐表/类型/长度/原字节比较、`:168`
保留已结算 FakeModel 响应/反馈和操作历史，以及 CLI `r24_recovery_cli_smoke.py:32`
与 `:46` 的独立进程重复打开、恢复事件及兄弟请求检查。限制：“不变”指业务持久行，
不承诺 writer-lock 权限规范化、SQLite WAL 等外围文件逐字节静止。

### 3. 未围栏执行、PENDING 调用和遗留 claim 仍恢复

`src/store.cpp:1439–1448` 明列 PREPARED/RUNNING/CANCEL_REQUESTED、PENDING
attempt、ACTIVE actor claim、CLAIMED inbox、未完成 RUNNING/FAILED/claimed job，
以及缺少保留回执的非 DELIVERED 终态。`1452–1466` 推进所选 job 租约并释放旧 claim；
活动请求经 `1390–1401` 和 `1468–1505` 取消未派发操作、把在途操作/调用围栏 UNKNOWN，
保留原预约、预算和说明见证。没有把恢复成功当作效果已撤销。

对应 `r24_recovery_tests.cpp:181`、`:200` 和 PENDING-only 的 `:211`；
`r24_cleanup_recovery_tests.cpp:136`、`:176` 分别检查原清理领取失效和恢复失败回滚；
原 `r2_recovery_tests.cpp:249` 检查合成的 RUNNING experience job 定向恢复。
限制：CANCEL_REQUESTED、孤立的 CLAIMED inbox 和 FAILED job 没有各自独立的新增
R2.4 子例，当前对此依靠明确 SQL 分支及相关综合回归，不能宣称每个 predicate 分支均有
独立红绿运行证据。合成 job 只证明存储恢复，不是业务验收或实际退出证明。

### 4. 已围栏 UNKNOWN 不获得再次执行权

UNKNOWN 可以使恢复判定为假，但 `src/store.cpp:1229–1247` 的 `ready` 继续拒绝同团队
UNKNOWN，并保留请求/任务状态、版本、applied、暂停、等待和工具权限。
`3241–3272` 只有首次 PREPARED→RUNNING 才返回 `dispatch_claimed=true`；
`WorkerTools::execute` 在 `src/worker_tools.cpp:504–506` 没有 claim 就只返回原事实。
`store.cpp:3534–3589` 保存迟到原始执行事实时核对原 binding，恢复后的 UNKNOWN 不能被
迟到成功写回 SUCCEEDED，`accepted` 也不能由原结果冒充。

对应 `r24_recovery_tests.cpp:181`、`:211` 与 `worker_tools_tests.cpp:436` 的 UNKNOWN
控制/诊断、`:763` 的新会话仍触发持久 UnknownOutcome。限制：Root 合法核对是原有受控
动作；核对解除持久 UNKNOWN 后的新业务仍需全部准入，不代表原操作可以再执行。

### 5. session 检查覆盖 Actor 授权及精确重放

`src/store.cpp:1133–1157` 的 `authorize` 在任何 `current_epochs` 条件外，于 `1140`
核验 `writer_session`；省略当前 epoch 只读识别原命令时也必须满足当前 session。
对照 `include/wls/store.hpp` 所有 Actor 参数入口，直接调用或委派至该方法：
Store 中的工作流/事件/容量/操作/模型入口，`store_rework.inc:20,78`，
`store_history.inc:38,97,131,185,202,247,382` 及其 management 委派，
`store_single_agent.inc:47`，`store_cleanup.inc:111,156,223,243,296,306,321,361`，
`store_live.inc:213,382,423,505`。精确 replay 均位于相应 authorize 之后。
例如 `store.cpp:3168–3176,3247–3254,3685–3691` 分别是 prepare、start 与预约重放。

对应 `r24_recovery_tests.cpp:267` 的四角色/精确重放、`:223` 和 `:231` 的相同 epoch
旧 writer/reader 拒绝，以及 `worker_tools_tests.cpp:884` 的旧 session 原 prepare 指纹拒绝。
`actor_json`（`store.cpp:102–105`）不加入 session，原 command/记录 hash 保持。
限制：没有给每一个公开 Actor 方法新增单独 session 子例；覆盖公共授权函数及入口追踪，
不是对任意未来入口自动成立的证明。

### 6. WorkerTools 在工作区绑定、工具和控制前核验身份

`src/worker_tools.cpp:275–283` 核验 Worker scope/session/两个 epoch；`bind:290–302`
先检查 Actor 才可能调用原 actor-free `Store::bind_workspace` 写绑定。
业务 `421–438` 先验证 Workspace/Actor；诊断 `383–387` 先经 Store 授权再限流和访问文件；
控制 `400–412` 先核验传入 session，再用当前身份核权限/限流，原身份只用于 Store 的
原命令精确重放判定。它没有先对外来 session 消耗当前团队限流额度。

对应 `r24_recovery_tests.cpp:238` 的 reader Actor 无绑定写、`:246` 的超过限流次数外来
控制仍被拒绝且当前控制可用；`worker_tools_tests.cpp:333,410,1039` 保留角色、只读和
旧 ack 不应用新说明的检查。限制：WorkerTools 构造器可在获得 Actor 前打开宿主已选
Workspace 句柄；这里核验的是绑定/工具副作用和受限读取，并非声称构造器从不接触文件系统。

### 7. 原 Actor 不能只靠 request ID 恢复新权限

`StoreActor::writer_session`（`include/wls/store.hpp:25–35`）默认空；
`host_actor`（`src/store.cpp:2755–2771`）从本实例原 `writer_instance`（`:1720`）签发并
再次 authorize。仅重填 request ID、原 owner 和原 epoch 不满足 `authorize:1140`。
reader 和 writer 即使共享当前数据库/epoch，也属于不同实例 session。

对应 `r24_recovery_tests.cpp:223,231,267`、`worker_tools_tests.cpp:877–895`。
限制：重新调用受信宿主 `host_actor` 获得当前身份属于授权 API 的正常使用；公开字段
不是不可伪造 capability。本项不对拥有当前实例任意代码执行权的攻击者作认证承诺。

### 8. 结算失败围栏使用完整三元组

`src/worker_tools.cpp:124–132` 的静态共享表键为
`(writer_session, team_id, execution_epoch)`；业务早期 `437`、派发前 `500`、
失败登记 `569` 使用相同键。派发前检查/首次 claim 和结果保存/失败登记分别在
`495–503`、`558–572` 持有同一个 `ActiveRuns::mutex`，早先通过检查的 PREPARED
调用不能越过后来登记的失败。正常重签 Actor/重建 WorkerTools 不改变本 Store session，
Root 核对也不删除旧标记；新 Store 仍独立受到持久 UNKNOWN 约束。

对应 `worker_tools_tests.cpp:763` 的两个核对顺序、原文件/记录/预算及旧 Actor 检查，
`:945` 确定性地阻塞已 PREPARED 调用且让全部持久兜底失败，从而检验实际内存围栏。
限制：这不是物理磁盘预留或断电保证；新 session、进程重启或 Root 核对均不证明原进程退出。

### 9. 4096 上限与进程级耗尽仍失败即阻断

`src/worker_tools.cpp:567–570` 在故障表已有 4096 条或插入分配抛异常时置
`settlement_capacity_exhausted=true`；静态表没有按 session 切换删除条目的路径。
`437` 和 `500` 均先检查全局标记，故新 Actor、WorkerTools、Store 或 epoch 不能
解除进程级耗尽；精确只读重放仍可以返回原事实而不会派发。

对应 `worker_tools_tests.cpp:736,763,945` 覆盖失败登记、局部键和派发前复查。
**覆盖缺口：未找到直接生成 4096 条故障、触发第 4097 条或注入容器分配异常的运行回归。**
本项对容量耗尽分支的判断来自源码，不能写成触顶测试已通过；可另立确定性边界回归任务。
未发现有证据表明该缺口已形成生产绕过，故未定为已确认缺陷。

### 10. 并发 prepare 重放不派生另一 start 意图

`src/worker_tools.cpp:465–476` 在 Store 完成原 prepare 身份、binding、grant 和指纹检查后，
对非 PREPARED 的当前 operation 只返回原 operation/result，不插 active owner、不构造
变化版本的 `command:start`。仍为 PREPARED 时继续原 mutex、故障围栏和 Store CAS；
Store 原重放 `src/store.cpp:3252–3254` 始终不给第二个派发 claim。

对应 `worker_tools_tests.cpp:579` 的确定性 before-prepare 调度：两次初查都为空，第二个
prepare 等到第一调用完成并移除 owner 后继续；`:648–720` 同时核对一次真实文件效果、
operation/result、工具计数、task version、三个原事件、预算/attempt 与 inode/mtime/ctime。
改变原 start CAS、prepare 参数或 wrapper 参数仍被拒绝。原 `:549` 并发用例保留。
限制：新确定性子例针对已结算 SUCCEEDED 的迟到 prepare 时序；其他非 PREPARED 状态
使用同一只读分支，未把每一种状态称为独立的确定性红绿用例。观察宏只在测试 TU 定义。

### 11. 原权限、CAS、预算、退出、所有权和终态守卫保留

生产 diff 只修改 Store 头文件、Store 与 WorkerTools。`store.cpp:1182–1209,1229–1247`
保留原 binding/grant/readiness；`:785–787,3191–3199,3265–3269` 保留工具额度/CAS/首次派发；
`:3685–3695,3794–3807` 保留模型准入、终态和原预算预约。
`store_capacity.inc:142–165,216–240` 继续预留 UNKNOWN、五维未结算及分配结算尾部。

清理 `store_cleanup.inc:11–36` 核原结果 hash/实际退出字段，`:42–68` 核 DELIVERED、
精确回执、经验、操作与模型结算、完整分配/所有权，`:108–120` 核当前 Root 与原
lease_epoch/job_version/claim_id/manifest。`:359–427` 保留固定库存及原证明；
`cleanup_runtime.cpp:115–138` 在删除前复核权限和已存在的精确交付包。
新 session 不清 UNKNOWN、不采纳外来目录、不补造退出证据，也不授予终态业务权。

另查原 actor-free 保存事实入口：`store.cpp:3534–3589`、`:3829–3915` 和
`store_live.inc:273–320` 保留原 operation/attempt binding、原 send claim、请求范围、
原始事实不可变及迟到 accepted 判断。旧可信宿主 `reconcile_operation(id,…)` 重载
（`store.cpp:3315–3329`）仍存在，不新增模型工具权限；当前 Root 重载由
`:1379–1388,3332–3352` 核 session/owner/CAS。它们不能被描述成所有 Store 写接口均要求 Actor，
也没有因为缺 Actor 参数而误判为新业务派发漏洞。

对应 `r24_cleanup_recovery_tests.cpp:130,136,152,158,164,176`、
`r24_recovery_tests.cpp:282,296,351`、`r2_recovery_tests.cpp:164,179,249` 及
`worker_tools_tests.cpp:227,272,312,333,363,381,436,534,763`。
显式强制 `recover_request`（`store.cpp:3616–3623`）仍做活动请求/CAS并执行原恢复；
AllRequests（`:720–744`）保持强制维护。CLI `:64` 核 init 递增，恢复见证篡改仍由
`r24_recovery_tests.cpp:282–295` 拒绝。限制：合成退出记录仅检验消费方门禁；本轮没有
强沙箱正向、残留 cgroup 接管、真实供应商或任意同 UID 对抗验收。

## 已有执行证据的引用及范围

本轮完整阅读的 [既有 R2.4 工程复核](r2-4-demand-driven-recovery-review.md) 记录了分阶段
反例、最终源码和补充审计。该文末明确保留早期 ASan 超时/内部失败、未采用性能候选与
未运行 oracle，随后才追加同源完整 ASan/LSan 31/31、exit 0 的独立证据审计。
不能引用中途状态掩盖最终补充，也不能删除早期失败。

该既有文档记载 pristine 基线核心 6/17、清理 2/6、CLI 1/3（均 exit 1）；
late-prepare 的基线加私有观察点为 18/19，修复候选 19/19；结算会话反例是 v2 的
19/20 对 v3 的 20/20，后者不是 pristine 基线。完整执行命令、退出码和日志真实性由
本批独立证据审计另核；本审查者本轮只读了这些记载，没有重新执行或把摘要核对冒称
二进制运行前后身份审计。最终本地门禁、发布与远端 CI 状态应以实际验证报告、冻结
证据及主协调 Agent 的远端核验为准，本文件不重新认证它们。

## 结论状态与后续

`CODE_REVIEW_COMPLETE=true`，`blocking_findings=[]`，`confirmed_defects=[]`。
`PR_REVIEW_COMPLETE=pending`，`MERGED=false`。本审查未执行测试、未提交/推送、未核验
远端 Actions，因此不由本文件更新 IMPLEMENTATION/LOCAL_VALIDATION/PUSH/REMOTE_CI
的完成标记。4096/分配失败触顶、部分恢复 predicate 的孤立分支与更广并发排列仍是覆盖
限制，交主协调 Agent 决定后续回归任务；没有将它们虚构为已复现产品缺陷或扩展本批实现。
