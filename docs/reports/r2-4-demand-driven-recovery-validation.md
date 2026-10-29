# R2.4 按需恢复与执行身份隔离验证

执行日期：2026-10-29 UTC。蓝图 v0.4.1，基线版本 0.11.1-r3.3c-audit，Store 410。
设计见 [ADR-091](../adr/091-demand-driven-request-recovery.md)，实现见
[按需恢复契约](../implementation/R2-demand-driven-recovery.md)，独立复核见
[审核记录](../reviews/r2-4-demand-driven-recovery-review.md)。

按需恢复、执行身份隔离、WorkerTools 并发重放及结算故障会话隔离均已落盘，并完成
限定范围的独立工程复核。最终输入已冻结为 **v3、312 项**，四种配置的全部默认目标
增量构建均已通过。**本地验证已完成**：v3 Release `0022`、Release `0002` 和严格
Clang 完整测试各 81/81、exit 0；UBSan 相关 31/31、exit 0；同一 v3 原二进制的
ASan/LSan 完整相关重验于 18:16 UTC 结束，**31/31、exit 0、1243.067519 秒**。
五项有效门禁使用相同的冻结输入，没有扩大期限、降低断言或关闭插桩。
早期 v3 ASan 的 19 通过/12 超时、exit 8，以及三个内部 FAIL 的原 60 秒 deadline
诊断均保留在原摘要和证据包；本次成功没有改写它们，也不能证明历史失败仅由环境导致。
v1/v2 结果及未采用性能候选保留各自范围，不混入 v3 验收。
本报告登记提交前的本地证据；未结束的运行不计为通过，最终提交、推送、Actions 和远端
源码快照核验由发布后的交付回执记录。本批仅使用临时数据库、临时文件、FakeModel 及
既有 loopback TLS 合成夹具，没有真实模型供应商、付费 API、生产数据或生产服务调用。

## 基线、工作目录与环境

| 项目 | 实际值 |
| --- | --- |
| 用户指定基线、初始 HEAD、初始 `origin/main` | `a6b8559018a599d7a806b05942857a56d69a00ce` |
| 开发分支 | `feat/r2-4-demand-driven-recovery` |
| 任务目录 | `/mnt/d/codex-workspace/wls861-r2-4-20261029` |
| 数据盘 | `/mnt/d`，`/dev/vdb2`，ext4 |
| 工作区 | `feature` 为独立 clone；`baseline` 为原提交的独立 detached worktree |
| 构建及测试用户 | ubuntu，UID/GID 1000，附加组 27/999 |
| 工具链 | GCC 13.3.0、Clang 18.1.3、CMake 3.28.3、Ninja 1.11.1、Python 3.12.3 |
| 依赖 | Boost 1.83、SQLite 3.45.1、OpenSSL 3.0.13、libcurl 8.5.0、LibYAML 0.2.5、jsonschema 4.10.3 |
| 测试解释器 | 任务目录 `test-venv/bin/python` |

初始检查未发现需要接管的既有仓库。没有执行 hard reset、clean、强制切换或强推。
初次 `baseline-configure` 因缺少 Boost 头文件 exit 1，16.333 秒；补齐依赖后的
`baseline-configure-ready` exit 0。系统缺少的编译依赖通过 apt 安装，安装日志保留；
构建和测试使用普通用户。
已完整阅读任务列出的当前项目文件；ADR-079 的实际路径为
`docs/adr/079-independent-boundary-review.md`。已按 AGENTS 读取 codebase-memory skill；
初期没有可用图索引能力，后续 `list_projects` 也返回零个索引，因此用定向源码与 `rg` 检查调用链，
不声明图覆盖或全项目形式化安全证明。

原 GitHub CLI 账号 Lordakee 只有 READ 权限。按用户明确指示退出并完成 wmqfl861
浏览器设备授权后，核验账号为 wmqfl861、仓库 `push=true`。仓库提交身份使用该账号及
其 GitHub noreply 地址；项目和证据不保存凭据。提交前 fetch 于
`2026-10-29T18:19:54.845193+00:00` 完成，随后核验 main 仍等于上述基线；目标远端
分支在初次检查时不存在。证据为 `fetch-before-r24-commit.json/.log` 与
`main-before-r24-commit.json`，先前核验记录保留。当前账号再次核验仍为 wmqfl861，
仓库 push/admin 权限均为 true；账号权限不是本批发布阻塞。若发布前 main 前进，
停止发布并交主协调 Agent 复核整合。

## 实际修改与兼容边界

原 OneRequest 构造取得数据库 writer-lock 后，无条件调用 `recover_selected_request`。
活动请求每次打开都会推进 dispatcher/team epoch、暂停并失效 ack、追加恢复见证；
终态未结束 job 也会推进 lease/CAS。若仅省去 epoch 变化，原 Actor 又可能凭相同的
持久主体和 epoch 跨 Store 使用，包括只读 Store Actor 和精确重放入口。

本批在原独占 writer flock 和同一 `BEGIN IMMEDIATE` 内完成读取、判定和恢复。
需要恢复的状态包括 PREPARED/RUNNING/CANCEL_REQUESTED 操作、PENDING attempt、
ACTIVE claim/CLAIMED inbox、RUNNING/FAILED/已领取但未完成的经验或清理 job，
以及缺少非交付终态留存事实的请求。命中时调用原恢复；空闲打开不改业务行。
普通待办、WAITING、AMEND、已围栏 UNKNOWN、裸分配意图、未领取的 PENDING 收尾
本身不触发额外恢复。判定不位于 CLI 包装层或事务外，原写者锁仍覆盖 Store 生命周期。

新增 `StoreActor.writer_session` 由当前 `host_actor` 签发，统一 `authorize` 无条件
核验，包括 `current_epochs=false` 的精确重放前置路径。WorkerTools 在工作区绑定和
控制限流前核验会话。跨 Store 的宿主必须获取新 Actor；原会话内的精确重放规则保留。
这是现有可信宿主 API 的会话围栏，不提供针对恶意同 UID 宿主的认证边界。

完整 Clang 与 Release `0002` 两轮运行还暴露了基线已有的 WorkerTools 并发重放竞态：
迟到的 prepare 重放可能取回已经派发/完成的操作行，再用变化后的版本派生同一个
内部 start 意图，被 Store 正确拒绝。最小修复在 prepare 返回非 PREPARED 行时只读
返回原 operation/result，不再重新占用执行 owner 或派生新 start。原 session、Actor、
epoch、Workspace、binding、tool grant、prepare 指纹和失败结算守卫仍先执行；
PREPARED 路径继续保留第二次结算检查、admission lock 和 Store CAS。UNKNOWN 或
CANCELLED 原样返回，不新增 claim、事件、业务权限或进程退出推断。

v3 进一步把结算写入失败的进程内键从 `(team_id, execution_epoch)` 改为
`(writer_session, team_id, execution_epoch)`，用于业务早期准入、首次派发前复查及
故障登记。按需重开保留 epoch 时，新 Store 会话不再误继承旧会话局部故障。同 Store、
团队及 epoch 内重新 `host_actor`、重建 WorkerTools，或合法 Root 核对 UNKNOWN，
都不会删除原内存标记。新会话仍受持久 UNKNOWN、合法 Root 核对及其他原 Store 守卫约束，
旧 Actor 仍被拒绝。4096 条故障表上限、分配失败时的进程级容量耗尽阻断保持，须重启
进程才能解除全局标记；换会话及重启都不构成原进程退出证明或清理许可。

新增字段不进入 `actor_json`、执行 tuple、模型信封、原命令/结算或历史指纹。
字段值复用既有 `writer_instance`；原 live 预约和发送记录中的实例标识列继续持久化
该值并保持原语义。没有新增数据库会话列，也没有把实例 ID 当作网络认证秘密。
默认 AllRequests、`wls init` 和显式 `recover_request` 继续强制恢复。请求/任务/角色/
Workspace、epoch、CAS、UNKNOWN、终态、进程退出与清理所有权守卫保留，原请求、
尝试/响应/反馈、历史事件和已发布 SQL 不改写，符合蓝图分层权限及原 ADR-064/071 的
事务、恢复见证和证据保留约定。

下表列出本批实际修改文件；结果摘要和证据归档已在五项最终门禁结束后生成。

| 文件 | 实际作用 |
| --- | --- |
| `include/wls/store.hpp` | 增加默认初始化的 `StoreActor.writer_session` |
| `src/store.cpp` | 在原事务内判定 OneRequest 是否需要恢复，并统一核验 Actor 签发会话 |
| `src/worker_tools.cpp` | 在工作区绑定及控制限流前拒绝旧会话；prepare 返回非 PREPARED 时只读重放；结算内存故障键加入 writer_session；私有 before-prepare 观察点仅在测试翻译单元定义宏时生效 |
| `CMakeLists.txt`、`Makefile` | 注册三个新增 R2.4 回归套件 |
| `tests/r24_recovery_tests.cpp` | 核心 17 子例：空闲、必要恢复、身份、并发、回滚及终态 |
| `tests/r24_cleanup_recovery_tests.cpp` | 清理 6 子例：租约、原 inventory、退出、所有权及原子性 |
| `tests/r24_recovery_cli_smoke.py` | CLI 3 子例：跨进程空闲打开、已结算批次及显式 init |
| `tests/worker_tools_tests.cpp` | 在原 18 子例上增加 late-prepare 和结算故障会话隔离两例，现为 20 子例；保留原子例、helper 和期限，核对一次派发、持久 UNKNOWN、旧身份拒绝及原数据库/文件证据 |
| `tests/store_actor_support.hpp` | 当前 Store 重新签发并核对主体/范围，仅恢复历史 epoch 的精确重放测试辅助函数 |
| `tests/store_tests.cpp`、`tests/workflow_store_tests.cpp`、`tests/acceptance_store_tests.cpp`、`tests/live_model_store_tests.cpp` | 先拒绝旧 Actor 并核对无副作用，再用当前会话验证原结果只读重放及历史 epoch 新键拒绝；reader 负例使用自己的有效 Actor |
| `tests/r1_window_tests.cpp` | 当前会话 reader 保留原历史和 SQLite trace 确定性并发 append 断言 |
| `tests/r2_recovery_tests.cpp` | WAITING 夹具加入真实 PREPARED 操作，继续验证必要恢复与取消/等待守卫 |
| `tests/r2_recovery_cli_smoke.py` | 空闲重开严格验证 epoch 和恢复事件不变，保留兄弟及原记录断言 |
| `tests/s13_cli_smoke.py`、`tests/r1_structured_cli_smoke.py`、`tests/r3_evaluation_tests.py` | 通过显式 init 触发原测试需要的真实跨代恢复，保留历史字节、反馈、CAS、见证篡改及 v2 解释断言 |
| `tests/single_agent_tests.cpp` | 同时验证 idle 和 forced，保持 logical_agent_id、原历史前缀及每次恰好追加两次调用 |
| `AGENTS.md`、`docs/STATUS.md`、`docs/ROADMAP.md` | 同步本批开发契约与任务状态 |
| `docs/implementation/S1-request-recovery.md`、`docs/implementation/S1-epoch-recovery.md` | 说明按需恢复及独立会话围栏对原契约的细化 |
| `docs/implementation/S1-worker-tools.md`、`docs/implementation/S1-process-settlement.md` | 明确结算故障的会话/团队/epoch 范围、持久 UNKNOWN、Root 核对、4096 条及进程级容量耗尽边界 |
| `docs/adr/091-demand-driven-request-recovery.md`、`docs/implementation/R2-demand-driven-recovery.md` | 新决策与实现契约 |
| `docs/reviews/r2-4-demand-driven-recovery-review.md` | 独立工程复核范围、输入哈希及结论 |
| `docs/reports/r2-4-demand-driven-recovery-validation.md`、`docs/reports/r2-4-inputs.sha256` | 本报告及最终运行输入清单 |
| `docs/reports/r2-4-results.json`、`docs/reports/r2-4-evidence.tar.gz` | 真实门禁结果摘要和原始证据归档，明确保留 ASan 失败；包状态见下文 |
| `docs/reports/r2-4-asan-profile-evidence.tar.gz`、`docs/reports/r2-4-serializer-probe-evidence.tar.gz` | 独立追加的性能采样与未合入候选证据；不覆盖最终门禁原记录或增加通过套件数 |
| `docs/reports/r2-4-resumption-results.json`、`docs/reports/r2-4-resumption-evidence.tar.gz` | 同 v3 原二进制重验后的有效五门禁摘要与补充证据；历史失败摘要和归档不覆盖 |

兼容测试没有直接把新 session 填回旧 Actor。辅助函数先从当前 Store 签发，核对
role/principal/request/task/team 和不同 session，再仅恢复历史 epoch 以重现原命令
指纹。reader 的 owner、policy 和旧 binding 负例使用 reader 自己签发的有效身份，
避免 session 先行失败遮蔽原安全断言。独立复核发现一项新增 live-send 断言需修正：
已有唯一 send claim 时，新命令键先返回 `Conflict`，并非 `Stale`。只修正该新增预期，
没有改变生产检查顺序、旧 Actor 拒绝或相同 claim 不获得新发送权的断言。

## 原版本反例及新增回归

在生产修改前，先构建原提交的 `wls` 和其两个原库，新增测试使用基线头文件编译，
仅链接 pristine 基线库；CLI 使用基线 `build-release/wls`。原库构建 exit 0，
1493.880 秒。初次稳定核心为 15 子例；独立复核增加 PENDING-only 与控制限流后，
再次对原库执行最终 17 子例。最终共有 **3 个逻辑套件、26 个子例（17 + 6 + 3）**，
不是 26 个独立套件。

| 最终新增回归源 | pristine 基线结果 | 修复后早期专项结果 | 原问题证据 |
| --- | --- | --- | --- |
| `r24_recovery_tests.cpp`，17 子例 | 6 通过/11 失败，exit 1，27.111 秒 | 17/17，exit 0，77.009 秒 | 空闲 epoch 1→2、全表行变化、旧/只读 Actor、工作区绑定、精确重放及控制路径未拒绝，UNKNOWN-only 再恢复 |
| `r24_cleanup_recovery_tests.cpp`，6 子例 | 2 通过/4 失败，exit 1，12.690 秒 | 6/6，exit 0，40.076 秒 | 空闲 closeout 行变化，无退出/无所有权夹具的打开副作用，裸分配 epoch 变化 |
| `r24_recovery_cli_smoke.py`，3 子例 | 1 通过/2 失败，exit 1，6.191 秒 | 3/3，exit 0，27.093 秒 | 空批次 epoch 2≠1，已结算批次 epoch 3≠2；显式 init 保留性子例原本通过 |

直接记录为 `baseline-r24-red-review`、`baseline-r24-cleanup-red-final`、
`baseline-r24-cli-red-review` 和 `feature-r24-native-green`、`feature-r24-cleanup-green`、
`feature-r24-cli-green` 各自的 `.json/.log`。在最终私有 tmpfs 条件下追加的三次原版本
反例结果仍为 6/11、2/4、1/2，均 exit 1，分别耗时 29.615、16.815、6.382 秒；
见 `baseline-r24-*-private-tmpfs` 和 `baseline-final-counterexamples.json`。
反例源、基线 CLI、库及独立回归二进制哈希保存在 `baseline-review-regression-inputs.json`、
`baseline-review-binary-identities.json`、`baseline-binary-identities.json`。
这三项回归源码在 v1→v2→v3 之间没有变化。表中的修复版本专项执行早于 v1 全部输入冻结，
当时完整输入以各自记录为准；它们证明对应修复通过专项，不能代替 v3 的完整重验。

必要恢复、显式强制恢复/见证篡改、并发写围栏和回滚在原版本中的通过是保留性回归，
不算作缺陷反例。原生测试逐表编码 SQLite 类型、长度及原始列字节，包含 FTS 内部表，
同时检查 epoch、原始请求/尝试/响应/反馈、恢复事件及实际文件副作用；不要求 SQLite
数据库文件或 WAL 的物理布局不变。早期夹具漏传 `finish_operation` 的原 binding
导致 `baseline-regression-compile` exit 1，410.515 秒；全表查询误假设 FTS 内部表有
rowid 的 `baseline-r24-red` 为 2 通过/13 失败、exit 1、16.526 秒，两者不计入产品
缺陷反例。修正后的初次核心 `baseline-r24-red-final` 为 6 通过/9 失败、exit 1、
20.392 秒，随后被上述最终 17 子例反例补充。初次 CLI `baseline-r24-cli-red` 为
1 通过/2 失败、exit 1、13.377 秒，结论与最终同源复跑一致。合法参数的旧清理 Actor
夹具也在正式反例前修正；全部早期记录保留，不与最终套件数或最终输入混算。

新增覆盖包括：

- 空闲四次重开、已结算 FakeModel 响应/反馈/操作的原字节稳定；真正未完成操作、
  PENDING-only attempt、活动事件 claim 恢复；UNKNOWN-only 再打开不写业务行。
- 旧写者及只读 Actor、WorkerTools 绑定/控制限流、四角色精确重放拒绝；显式恢复的
  epoch/见证语义及篡改拒绝；WAITING/AMEND 与终态业务拒绝。
- 两个恢复调用、恢复与捕获写入、恢复与 claim 的并发边界；故障注入后的全事务回滚。
- 真实交付/回执/经验完成后的空闲清理 job、中断清理的原 inventory 和真实文件，
  分别拒绝旧 Actor/旧 claim，保留进程退出缺失、无所有权目录及裸分配意图守卫。
- CLI 独立进程空批次重开、已结算 Worker 批次连续执行/兄弟隔离/原反馈，以及显式 init。

清理夹具中的进程观察明确标记为合成原证据，不声称实际生产强沙箱运行或退出已获证明。

## 并发问题的确定性反例、修复与追加复核

v1 完整 Clang 首轮和 Release `0002` 都在原 `concurrent exact commands have at most one actual execution owner`
子例中报 `command_id reused with different intent`。独立源码对照确认相关 prepare、start、
WorkerTools::execute 和原子例在基线中已存在相同时序。两个线程都先查询操作不存在，
第一线程完成真实派发并移除内存 owner 后，第二线程的迟到 prepare 可以返回版本已变的
现有操作，继而错误派生同一个内部 start 命令；Store 的原指纹检查阻止了该调用。
用例没有 Store 重开、恢复、epoch 或 session 切换，未发现双派发或旧身份越权。
Clang 随后完整重跑通过只说明那次运行成功，不能证明这个可达竞态已消失。

为实际证明并修复该问题，新增子例在迟到调用已完成空查询、尚未 prepare 时暂停它，
让首个 owner 完整返回并移除内存占用后再释放迟到调用。两侧使用完全相同的测试源码，
经真实 Store 验证，未伪造数据库状态。红例使用 pristine 基线头文件、资源及 Store 库；
测试翻译单元编译原 WorkerTools，仅加入私有 before-prepare 调度观察点，不能称为
完全未插桩的原二进制。绿例使用 feature 对应头文件、资源及库，避免 StoreActor ABI
混用，测试翻译单元提供修复后的 WorkerTools 实现。

| 同一新增子例的实际执行 | 结果 | 证据 |
| --- | --- | --- |
| 原基线 Store + 原 WorkerTools + 私有调度观察点 | **18/19，exit 1，4.862053 秒**；唯一失败为新 late-prepare 子例的原命令指纹冲突 | `baseline-worker-concurrency-red.json/.log` |
| 修复候选 + 相同调度观察点和测试源码 | **19/19，exit 0，17.203872 秒** | `feature-worker-concurrency-green.json/.log` |

两次运行均为普通 UID/GID 1000、umask 0002、CPU 2、私有 tmpfs、四项 capability 全零。
原始 argv、候选编译/链接命令、源、库和二进制身份集中记录在
`concurrency-counterexamples.json`。关键输入为：

| 输入 | SHA-256 |
| --- | --- |
| 红绿两侧相同的 `tests/worker_tools_tests.cpp` | `17d4fe1ef00e43819361dca4ac2dfcbd60df39d199f74eb066cbea890a89469c` |
| 原 WorkerTools 加私有观察点 | `3a733e2821b227025fe901b6eb41fabf70b91b7593dd08a15111fa8a710adcac` |
| 修复后的 `src/worker_tools.cpp` | `f5733950ed473c87a8da4e623ead70bac1410320d87c32db112376fb99a4845f` |
| 候选编译/链接 manifest | `7ffc3e616954a931f17fe85e15d40a69df4d23cf23dc74eaa286f39e7c376e32` |

新子例核对首次真实执行仅增加一个 operation/result、一次工具计数、三条原事件，
task 版本只推进一次；迟到重放不改变 operation/result、request/task、事件前缀、
预算/attempt 或真实文件的 device/inode、size、mtime、ctime 和内容。三种改变意图
的调用仍被原守卫拒绝。异常路径释放门并 join 线程后才重置 observer；原并发及
prepared-call failed-settlement 子例的函数体、断言和期限保持不变。

独立只读复核确认只读返回位于原授权/指纹守卫之后，PREPARED 派发和结算边界保留，
未发现阻断合入的问题；其精确输入与结论已追加到审核记录。候选已合入 feature。
此阶段在原 worker_tools 套件内增加 **一个子例（18→19）**；v3 另增加下述结算会话
子例，最终为 **20 子例**。项目仍是 **81 项 CTest**，R2.4 三个新增逻辑套件、26 子例
另计。候选红绿结果与后续 v2 完整门禁各自记证，不能代替 v3 验证。

## 结算故障会话隔离的 v2 反例与 v3 修复

按需重开可以不推进 epoch；原 `(team_id, execution_epoch)` 内存标记因此会跨 Store
会话继续拒绝新业务，即使持久 UNKNOWN 已经合法 Root 核对。新的回归实际注入
SQLite 结果事件写入失败，保留真实文件效果及 UNKNOWN 兜底，再分别检查“先核对后
重开”和“先重开再核对”。同 Store 重签 Actor、重建 WorkerTools 继续受局部围栏
阻断；新会话未核对时须被 Store UNKNOWN 守卫拒绝，合法核对后才可按原准入条件执行
一个新操作。旧 Actor 的业务和原 prepare 精确重放均被拒绝且没有副作用。

这次红绿两侧都使用 **feature v2 的头文件、资源及 Store 库**，测试翻译单元分别提供
v2 WorkerTools 与加入会话键的候选实现。它是对 v2 的行为反例，**不是 pristine 原始
基线反例**。两侧使用相同最终测试源；未 mock Store 权限、UNKNOWN 或核对条件。

| 实际执行 | 结果 | 证据 |
| --- | --- | --- |
| v2 WorkerTools + v2 Store/头文件 | **19/20，exit 1，20.590175 秒**；仅新增结算会话子例因旧局部围栏仍生效而失败 | `feature-v2-settlement-red.json/.log` |
| 会话键修复候选 + 相同 v2 Store/头文件和测试源 | **20/20，exit 0，23.777536 秒** | `feature-v3-settlement-green.json/.log` |

两次执行均为 UID/GID 1000、umask 0002、CPU 2、私有 tmpfs、四项 capability 全零。
原始命令、编译/链接 manifest 和候选二进制身份见 `settlement-counterexamples.json`。
候选二进制记录是链接后的身份，不冒充 CTest 的运行前后完整二进制哈希记录。

| 最终候选输入 | SHA-256 |
| --- | --- |
| v2 红例 `src/worker_tools.cpp` | `f5733950ed473c87a8da4e623ead70bac1410320d87c32db112376fb99a4845f` |
| v3 绿例 `src/worker_tools.cpp` | `aedf1cfc9c428ae74b68da7922a57558996733ebe08832f809fa8c15467b689a` |
| 两侧相同的最终 `tests/worker_tools_tests.cpp` | `9abbac0b086d8e037ca3679fbe4e3c6f40e700afbcf331de8e790386c288e8ce` |
| 候选编译/链接 manifest | `5b88a4c32c4b8dec9f47faa2b014eb43e6f3bdcc35809cfaf92a678b033dbe8e` |

回归保留原 operation/result、输出和历史前缀，验证 Root 核对只按原 CAS 推进状态和
证据引用，错误 Worker 角色不能核对；新业务只增加一个 operation、一次工具计数、
三条原事件，精确重放不再执行。原文件的字节、device/inode、size、mtime 和 ctime
不变，原预算和 attempts 不改写。两个核对顺序是同一新增子例的分支，不增加独立套件数。

静态复核发现并修正了新增夹具对 `evidence_ref` 的原操作投影；早期 C++ 编译又暴露
Boost.JSON value 与字符串直接比较的类型错误。`settlement-red-compile` 和
`settlement-green-compile` 均 exit 1，分别 28.096 和 28.402 秒；初始候选源及 manifest
以 `settlement-session-candidate/initial-*` 保留。修正后两侧重新编译成功，再执行上述
正式红绿；这些夹具/编译问题不算作产品行为反例，也没有更改原 19 子例、helper 或期限。
独立审查确认三处会话键一致、共享互斥/UNKNOWN/容量耗尽守卫保留；实现与四份契约已同步。

## 运行输入、命令与证据定位

最终运行输入 v3 于 `2026-10-29T07:30:07.674480+00:00` 冻结为 **312 项**，清单为
[r2-4-inputs.sha256](r2-4-inputs.sha256)，清单自身 SHA-256：

`59c8310d64a961d2ebac1159b93e8aa925499be61795b06831a96d79e2fa2050`

新记录为 `source-final-v3.json/.sha256`。前两版 312 项清单和原时间完整保留：

| 快照 | 冻结时间 UTC | 清单 SHA-256 | 记录 |
| --- | --- | --- | --- |
| v1 | 03:44:37.281422 | `b3d3127af9a44826d61f2974013282b5625865dd61cfe24643b97aef4df4ea09` | `source-final.json/.sha256` |
| v2 | 05:43:42.372914 | `8ce61b70f073b1a82f7b8298d6568dc57b498894c734eac918dfc6565f617beb` | `source-final-v2.json/.sha256` |

v1→v2 和 v2→v3 各自都只有 `src/worker_tools.cpp`、`tests/worker_tools_tests.cpp`
两个运行输入变化。所有旧快照的通过、失败及二进制记录保留，不改标为 v3。
已按 v3 启动四种默认目标增量构建，并在 `final-validation-plan.json` 固定五个最终门禁。

清单覆盖源码、测试、构建/CI、配置、SQL、Schema、提示词等运行资产。取样脚本排除
`docs/`、AGENTS、README、CHANGELOG 和 `.gitignore`，避免报告自指；这不是最终 Git
提交树的替代。三版清单均保留原时间，早期变动输入另存。
CTest 门禁另在运行前后记录首层可执行文件、静态库、CMakeCache、CTestTestfile 和
build.ninja 的哈希；安装生成资源不在这个集合内。`generated-install-assets-final.json`
曾于 05:55:18 UTC 补记 v2 四个构建目录共 12 个 runtime manifest/安装脚本的身份。
v3 对应的 `generated-install-assets-session-final.json` 已补记并独立核对同样的 12 项；这种补充记录
不能追认为这些文件已有运行前后哈希证明。
记录没有 `finished_at` 或 `exit_code` 时，一律视为尚未完成。

本地原始记录位于任务目录 `evidence/`；下文证据名均指其中同名 `.json/.log`。
JSON 保存 argv、cwd、UID/GID/组、调用 umask、时间、退出码和输入前后哈希。
最终 runner 还保存 CPU affinity、nice、capability、mount namespace 和 `/tmp` mountinfo。
当前产物状态如下：

| 产物 | 状态与用途 |
| --- | --- |
| [r2-4-inputs.sha256](r2-4-inputs.sha256) | 已存在；312 项 v3 最终运行输入 |
| [r2-4-results.json](r2-4-results.json) | 已冻结的早期 v3 历史摘要，保留首轮 ASan 失败及 `local_validation_complete=false`；不作为随后重验的当前状态 |
| [r2-4-evidence.tar.gz](r2-4-evidence.tar.gz) | 已生成并通过一致性验证；339 个条目，4,567,874 字节；原始日志、JSON、清单和复现脚本，不含二进制或临时数据库 |
| [r2-4-asan-profile-evidence.tar.gz](r2-4-asan-profile-evidence.tar.gz) | 独立性能诊断补充包；17 个条目、612,802 字节，原诊断仍为 Timeout/exit 8；不覆盖原总包 |
| [r2-4-serializer-probe-evidence.tar.gz](r2-4-serializer-probe-evidence.tar.gz) | 未合入序列化候选的文本证据；49 个条目、455,104 字节，两次原期限诊断均失败，oracle 未执行 |
| [r2-4-resumption-results.json](r2-4-resumption-results.json) | 后续同 v3 有效五门禁摘要，全部 PASSED；`local_validation_complete=true`，原失败摘要不覆盖 |
| [r2-4-resumption-evidence.tar.gz](r2-4-resumption-evidence.tar.gz) | 同源同二进制重验的补充日志、详情、身份和复现脚本；见下文实际摘要及独立审计 |

早期冻结机器摘要 SHA-256：
`78c718189cf951ac6a61352ee8390aa46d17502b91196a6377910abb1f7fb2fc`。
原始证据归档 SHA-256：
`45ac14ee3558dfb8f18054f4d543d0598297c7032a012e457943ed806bea4f6c`。

`verify_evidence_bundle.py` 逐项复算 INDEX、原始日志、计划、源码、timeout、配置和门禁状态，
339 个成员全部一致，没有未完成命令记录或 ELF/SQLite/ar 内容、危险路径、链接及常见凭据
格式命中。包内展开数据为 16,784,099 字节，只读核验没有执行归档代码。
验证器 exit 0 表示该历史证据包一致；其 `local_validation_complete` 保留为 false。
该次摘要生成器先落盘再 exit 1，真实表达 ASan 失败，没有用非零退出阻止失败证据归档。
归档包含四次后续 ASan 方法诊断；冻结的机器摘要专门描述最终门禁及其更早历史，后续
诊断原因以 `asan-diagnostic-findings.json` 和本节报告为准。

已执行命令的代表性记录如下，完整 argv 以对应 JSON 为准。基线 cwd 为 `baseline`：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --target wls --parallel 2
../tmp/baseline-r24-review
../tmp/baseline-r24-cleanup
../test-venv/bin/python ../feature/tests/r24_recovery_cli_smoke.py build-release/wls .
```

上述原生反例的编译及链接命令分别保存在 `baseline-regression-compile-review`、
`baseline-cleanup-regression-compile-v3` 及相应链接记录；这些命令使用基线 include 和库。
修复版本 Release 的 cwd 为 `feature`：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure --parallel 1
```

已执行的 v1 完整 Release `0022` 外层命令如下，cwd 为任务目录；
`run_validation.py` 记录真实测试子进程的状态及运行输入：

```sh
sudo -n unshare --mount --propagation private --fork ./run_in_test_tmpfs.sh \
  bash -c 'umask 0022; exec env TMPDIR=/tmp ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 taskset -c 2,3 python3 ./run_validation.py feature-release-0022-serial feature ctest --test-dir build-release --output-on-failure --parallel 1'
```

v1 `0002` 使用相同普通用户和临时目录条件、`umask 0002`、CPU 2；首轮 Clang 使用
`build-clang`、`umask 0002`、CPU 3。它们与后续 v2 三次完整原生测试均已实际结束，
结果列于下文；v3 的计划与完成记录使用单独名称，避免混用快照。

## 完整构建与最终门禁

v3 构建均使用 `cmake --build <构建目录> --parallel 2`，请求全部默认目标，
由 Ninja 按实际依赖增量更新。四次构建均于 07:30:08 UTC 启动；当前已核实结果如下：

| v3 默认目标增量构建 | 构建目录 | 当前实际结果 | 证据名 |
| --- | --- | --- | --- |
| GCC Release | `build-release` | **exit 0，502.606 秒**；输入前后一致 | `feature-build-release-session-final` |
| 严格 Clang Release | `build-clang` | **exit 0，478.806 秒**；输入前后一致 | `feature-build-clang-session-final` |
| Clang 优化 ASan/LSan | `build-asan-clang-opt` | **exit 0，758.090 秒**；输入前后一致 | `feature-build-asan-session-final` |
| Clang 优化 UBSan | `build-ubsan-clang-opt` | **exit 0，660.710 秒**；输入前后一致 | `feature-build-ubsan-session-final` |

`final-validation-plan.json` 固定 v3 输入、实际 argv、普通 UID/GID、umask、CPU、
完整注册名和原 timeout 映射；计划文件不是通过证据。其 SHA-256 为
`282d031865d920b7cbf97cd1969d2150355cc90a82ad53a50127242d09b12a2b`。
三项原生门禁用 CTest `--parallel 2`，两项 sanitizer 用 `--parallel 1`，范围如下：

| v3 首轮完整验证记录 | 范围及计划条件 | 该次实际结果 | 证据名 |
| --- | --- | --- | --- |
| Release，`umask 0022` | 全部 81 项，CPU 0/1 | **81/81，exit 0，1180.884 秒**；输入及二进制前后一致 | `feature-release-0022-session-final` |
| Release，`umask 0002` | 全部 81 项，CPU 0/1/2/3 | **81/81，exit 0，957.808 秒**；输入及二进制前后一致 | `feature-release-0002-session-final` |
| 严格 Clang Release | 全部 81 项，umask 0002、CPU 2/3 | **81/81，exit 0，1174.086 秒**；输入及二进制前后一致 | `feature-clang-session-final` |
| Clang 优化 ASan/LSan | 31 项，umask 0002、全部 CPU，独占运行 | **19/31，12 项 Timeout，exit 8，3145.397 秒**；输入及二进制前后一致 | `feature-asan-session-final` |
| Clang 优化 UBSan | 31 项，umask 0002、全部 CPU，独占运行 | **31/31，exit 0，1156.904 秒**；输入及二进制前后一致 | `feature-ubsan-session-final` |

后续 `feature-asan-v3-resumption-full` 在相同输入、相同 57 项二进制/构建身份、
相同 31 个注册项和原期限下完成 **31/31、exit 0、1243.067519 秒**。
当前有效门禁采用这次 ASan 与上表四项已通过记录，全部为相同 v3；上表首轮失败
完整保留。补充摘要明确列出这五份记录的实际范围，没有重复计算逻辑测试套件。

只有匹配 v3 输入并具有真实结束状态的记录，才可将对应门禁改为完成。此前 v2 的
四次全默认目标增量构建均完成 51 个实际动作，输入前后一致，但属于旧快照：

| v2 增量构建 | 实际结果 | 证据名 |
| --- | --- | --- |
| GCC Release | exit 0，342.979 秒 | `feature-build-release-r24-final` |
| 严格 Clang Release | exit 0，330.698 秒 | `feature-build-clang-r24-final` |
| Clang 优化 ASan/LSan | exit 0，633.102 秒 | `feature-build-asan-r24-final` |
| Clang 优化 UBSan | exit 0，592.507 秒 | `feature-build-ubsan-r24-final` |

v2 三次完整原生测试确实通过，输入和记录的二进制前后相同；sanitizer 的实际限制如下：

| v2 已结束验证 | 实际结果 | 证据名 |
| --- | --- | --- |
| Release `umask 0022`，全部 81 项 | **81/81，exit 0，2092.475 秒** | `feature-release-0022-r24-final` |
| Release `umask 0002`，全部 81 项 | **81/81，exit 0，1957.602 秒** | `feature-release-0002-r24-final` |
| 严格 Clang Release，全部 81 项 | **81/81，exit 0，2065.493 秒** | `feature-clang-r24-final` |
| ASan/LSan 31 项候选 | 中途停止，子进程 -2/外层 130，683.299 秒，未完成 | `feature-asan-r24-final` |
| ASan/LSan 四项独占诊断 | **0/4，exit 8，362.325 秒**；四项均达各自原 90 秒期限 | `feature-asan-v2-exclusive-diagnostic` |

v2 的 UBSan 相关测试没有完成记录，不能由其构建成功推断测试通过；v3 使用新的计划名
重新执行。v1/v2 的完成、失败或中止记录均不计为 v3 门禁。

以下已结束记录属于 v1 或更早的准备过程，保留其真实结果，不充当 v3 最终门禁：

| 旧快照已结束记录 | 实际结果 | 证据名 |
| --- | --- | --- |
| v1 Release `umask 0022` 全部测试 | **81/81，exit 0，2102.196 秒**；输入与二进制前后一致，04:57:53.776180 UTC 结束 | `feature-release-0022-serial`，原详情 `feature-release-0022-serial-detail.log` |
| v1 Release `umask 0002` 全部测试 | **80/81，exit 8，1821.703 秒**；worker_tools 原并发子例失败 | `feature-release-0002-serial` |
| v1 严格 Clang 全部测试首轮 | **79/81，exit 8，2240.899 秒**；sandbox 与 worker_tools 失败 | `feature-clang-serial`，原详情 `feature-clang-serial-detail.log` |
| v1 严格 Clang 全部测试独立重跑 | **81/81，exit 0，1097.203 秒**；输入与二进制前后一致 | `feature-clang-serial-recheck` |
| v1 Clang 优化 ASan/LSan 全量构建 | **141/141 构建动作，exit 0，5707.602 秒**；输入前后一致，尚未以此记录执行 sanitizer 测试 | `feature-build-asan-clang-opt` |
| v1 Clang 优化 UBSan 全量构建 | **141/141 构建动作，exit 0，5436.071 秒**；输入前后一致，尚未以此记录执行 sanitizer 测试 | `feature-build-ubsan-clang-opt` |
| v1 最终兼容专项 | **12/12，exit 0，438.577 秒**；输入及二进制前后一致 | `feature-release-compat-focused` |

前置 Release 与严格 Clang 都曾完成全部 141 个默认 CMake 构建动作，exit 0。
早期完整 Release 和其兼容重编译、完整 Clang 构建记录中的 `inputs_unchanged=false`
反映当时仍在编辑测试。v1 冻结输入上的两次补充构建均 exit 0、输入前后一致且 Ninja
无待构建动作：`feature-build-release-frozen` 为 0.205 秒，`feature-build-clang-frozen`
为 0.812 秒。Clang 首次构建用时 4952.366 秒，包含主动暂停，不能视为纯编译耗时。
这些属于 v3 之前的构建记录。

此前原 R2 CLI 兼容专项为 6/6、exit 0，见 `feature-r2-cli-compat`。v1 完整 Release
`0022` 已覆盖请求恢复、Store、工作流、进程结算、清理、计费、单 Agent、完整
`billing_baseline_e2e` 和历史兼容测试；v2 和 v3 的三次完整 81 项均已在相同注册范围
执行并通过。v3 ASan/LSan 首轮失败和随后同二进制完整通过分别见下文。
任何本地记录都不能替代实际远端 CI 结果。

本地优化 sanitizer 使用 Clang 18、Debug、显式 `-O2 -g`，**没有 NDEBUG**，
保留严格警告、Werror、全部地址/未定义行为插桩与泄漏检查。
ASan/LSan 为 `halt_on_error=1:detect_leaks=1`；UBSan 为
`halt_on_error=1:print_stacktrace=1`。配置命令分别启用 `WLS_ENABLE_ASAN=ON` 或
`WLS_ENABLE_UBSAN=ON`，完整 argv 见 `feature-configure-asan-clang-opt` 和
`feature-configure-ubsan-clang-opt`，二者配置 exit 0。
两种 sanitizer 的 v1 全量构建均已完成 **141 个默认 CMake 动作**，v2 的全默认目标
增量构建也已完成；v3 继续请求全部默认目标更新，只将测试选择限定为以下 31 个受影响的注册项。
不宣称本地 sanitizer 完整 81 项通过：

```text
store event_store workflow_store acceptance_store
event_runtime workflow_runtime acceptance_runtime worker_tools
cleanup cleanup_runtime audit24_reopen process_evidence audit24_reconcile
r1_window r2_recovery r24_recovery r24_cleanup_recovery allocation_recovery
live_model_store billing single_agent single_agent_runtime owned_capacity_runtime
live_model_store_tls live_workflow billing_baseline_e2e r3_evaluation
s13_cli_smoke r1_structured_cli_smoke r24_recovery_cli_smoke r2_recovery_cli_smoke
```

精确名称及正则保存在 `sanitizer-focused-scope.json`，`run_sanitizer_gate.py` 只执行
这一固定选择。31 是注册执行项数，其中原 single_agent 分组来自同一源码，不能按此
扩大独立逻辑套件数。原 CI（包括 GCC Debug ASan/UBSan 和 Make-ASan）未修改，
其结果必须在推送后按实际完整提交 SHA 单独核验。

## v3 首轮 ASan/LSan 的历史失败与诊断边界

`feature-asan-session-final` 完整结束为 **19/31，通过 19 项、Timeout 12 项，exit 8**，
runner 耗时 **3145.397442 秒**。源码输入精确匹配 v3，57 项记录的二进制/构建身份
前后相同；不是中断后遗留的后台验证。普通 UID/GID 1000、capability 全零、原断言、
原期限及 `halt_on_error=1:detect_leaks=1` 保留。12 项超时为：

| 原 CTest 注册项 | 实际超时秒数 |
| --- | ---: |
| `event_runtime` | 90.51 |
| `workflow_runtime` | 90.6 |
| `acceptance_runtime` | 90.4 |
| `worker_tools` | 90.5 |
| `cleanup_runtime` | 180.79 |
| `r24_recovery` | 90.33 |
| `single_agent` | 90.32 |
| `single_agent_runtime` | 90.48 |
| `live_workflow` | 300.08 |
| `billing_baseline_e2e` | 900.13 |
| `r3_evaluation` | 180.01 |
| `s13_cli_smoke` | 90.07 |

已通过项包括四个 Store 套件、cleanup、重开/结算审计、process_evidence、r1_window、
原 R2 recovery、R2.4 cleanup recovery、allocation recovery、owned capacity runtime、
live model Store、billing、loopback TLS、structured CLI、R2.4 CLI 与原 R2 CLI。
这些通过项不能替代未完整完成的 suite，也不能据此声明 ASan/LSan 总门禁通过。

`live_workflow` 在整体超时前出现
`test_model_exhaustion_escalates_original_rejected_submission ... FAIL`；计费 E2E 的
`test_02_two_task_partial_amend_pair` 和 `test_04_exhausted_rework_owner_question_pair`
在 `condition='single_agent'` 时也出现 FAIL。unittest 的最终 traceback 被整套超时截断，
这三条不能被描述为“仅外层超时、没有内部断言失败”。已完成按原文件、driver、参数、
断言及期限选择原方法的诊断，真实结果如下；它们不替代完整 suite。

| 原方法诊断记录 | 实际结果 | 失败位置与观察 |
| --- | --- | --- |
| `feature-asan-live-workflow-single-diagnostic` | exit 1，92.421895 秒 | 原第 322 行，第一阶段 blocked 中没有要求的 manager_limit |
| `feature-asan-live-workflow-details-diagnostic` | exit 1，94.988375 秒 | 相同断言；失败后只读观察原 result：阶段 0 为 deadline，60274/60000 ms，17 次调用；阶段 1 为 quiescent，30276/60000 ms，9 次调用；两阶段 provider_calls 均为 0 |
| `feature-asan-billing-amend-diagnostic` | exit 1，227.798127 秒 | 原计费测试第 354 行，single_agent 条件的 pump 返回 deadline |
| `feature-asan-billing-exhausted-diagnostic` | exit 1，263.402115 秒 | 同一原断言，single_agent 条件的 pump 返回 deadline |

两项计费诊断保留原 multi_agent/single_agent 两种条件和所有 phase，未删除任何断言。
原每次 pump 60000 ms、计费 CLI 150 秒、工作流 driver 240 秒、外层工作流 300 秒/
计费 900 秒均不变。外层 timeout 的 2 秒 kill-after 仅约束终止清理，不延长测试期限。
最后的工作流观察器只在原失败已记录后读取确切 traceback frame 的白名单字段，
没有改结果、Store、调度条件或测试方法，原失败仍以 exit 1 保留。原脚本和新观察脚本
各自的输入哈希、四次完整 argv/log/退出码均保留，结果汇总为 `asan-diagnostic-findings.json`。

上述结果明确了内部断言所观察到的 deadline，但不能证明执行过慢仅由宿主环境引起，
也不能替代被原 CTest 期限截断的完整 ASan/LSan 覆盖。诊断前后 v3 输入完全一致，
诊断序列中的 driver/脚本哈希一致；最终 `diagnostics-final-process-check.json` 未发现
仍运行的 sanitizer 构建可执行文件。没有通过提高期限、关闭 sanitizer、删断言或调用
真实模型来取得通过记录。

普通用户观察器 `monitor_sanitizer_gate.py` 每两秒只读取允许的 /proc 字段，不读环境
或完整 argv，不对测试发信号。观察器记录完整 1613 个样本，gate 结束后 exit 0。
独立只读 Agent 的 `asan-session-process-analysis.json` 按 PID/starttime 分开八个
原生超时进程：首末样本约 88 秒内各消耗 87.71–88.29 秒 CPU，cleanup_runtime
178 秒跨度内消耗 176.91 秒 CPU。状态多为 R，少数 futex/do_wait 样本保留。
这不支持将这些进程说成长期没有 CPU 推进的等待，也不能据此判断忙循环、性能根因
或单纯环境问题。采样首尾可能遗漏，不能当作完整进程寿命或补造进程退出码。

最终进程检查 `asan-session-final-process-check.json` 未发现仍运行的该 ASan 构建
可执行文件；原观察器的末次进程样本和完整结束状态一并保留。UBSan 全部 31 项
在这次 ASan 结束后才开始，现已 31/31、exit 0；`all-final-gates-process-check.json`
核对两种 sanitizer 观察器均正常结束，未发现残留的 sanitizer 构建可执行进程。

## ASan 性能定位补充

最终证据包冻结后，另以未改动的 v3 `event_runtime` 二进制执行原 90 秒 CTest，
外层 `perf record -e cpu-clock:u -F 49 --call-graph fp` 采样。CTest 为
**0/1、exit 8、90.705 秒**，原详情显示 90.60 秒 Timeout；外层 profiler runner
为 exit 8、202.415 秒，包含运行前后身份哈希等开销，不能当作测试耗时。
普通 UID/GID 1000、capability 全零、umask 0002、私有 tmpfs、原 ASan/LSan
选项和 57 项二进制/构建身份均保持；仅外层只读 profiler 使用 root 权限。
两份原记录为 `feature-asan-v3-event-runtime-perf`、`feature-asan-v3-perf-wrapper`。

采样包含 wrapper 的 Python 哈希阶段，因此默认 absolute 报告不能当作测试内比例。
过滤目标 comm 并使用 `--percentage relative` 后，`canonical_json` 为 19.44%
inclusive，`FrozenRequestConfig::restore` 为 19.83% inclusive；ASan 栈记录、
分配和快速展开分别为 12.31%、7.73%、5.94% self。inclusive 相互重叠，不能相加；
`main` 祖先只覆盖 54.90%，frame-pointer 栈有截断。这是一次有采样开销且超时的运行，
不能量化完整套件根因或证明环境是唯一原因，更不能替代验收通过。

独立静态复核没有发现 ASan 意外使用未优化构建：ASan 和 UBSan 都是 Clang 18、
`-O2 -g`、无 NDEBUG。原 `event_runtime` 夹具使用 AllRequests，不会进入新增
OneRequest 恢复谓词；新增会话比较和故障键也没有高阶循环或扩展历史选择范围。
这些事实缩小定位范围，不能单独排除性能回归。

上述诊断的文本补充包由 `evidence_audit` 生成并只读核验，SHA-256 为
`35c838fe112aa648b05f44f6f192fc0b3097fd39b7d356191dd1600898f710b8`。
INDEX、17 个成员、原始日志和两种百分比口径均匹配；归档时 312 项源输入、57 项
构建身份相同，普通及提权只读进程扫描未发现任务测试残留。原始 perf 二进制只在数据盘
保留，摘要 `a4a8358bcb41dcc3ffa96a60ba76fabbaa74cb033d1e99702c9111b138ae233a`
记入 INDEX，不进入 Git。补充包完整性通过不改变本地验证未完成状态。

随后在独立 `json-serializer-candidate` 中制作了仅复用单次调用 serializer/scratch、
预分配对象成员指针数组的候选。原字段排序、finite/depth 检查和最终 DOM 校验保持，
独立静态审查未发现阻断性语义问题。两侧均按原 Clang/ASan flags 编译唯一替换 TU，
链接相同 v3 原测试对象和库；四步均 exit 0。nm/map 确认各只有一个 canonical 定义，
未从原库再次提取旧 runtime_types 对象。该候选不修改 Store 或测试期限。

同一原 `event_runtime` 逻辑套件的两次顺序执行均失败：原实现 **0/1、exit 8、
90.506768 秒**，候选 **0/1、exit 8、90.529404 秒**，均为原 90 秒 Timeout，
不是一组通过的红绿验证。两边 14 个原子例及原对象相同，运行输入/二进制前后一致。
受期限截断的时长不能用来计算完整性能收益。候选没有解决门禁，标记 `NOT_ADOPTED`，
没有合入 feature；v3 的 312 项运行输入保持不变。额外字节 oracle 已编写但
**未编译、未链接、未执行（NOT_RUN）**，不计入测试套件数或通过证据。
正式记录为 `serializer-event-probe.json`、`serializer-event-{red,green}.json/.log`，
候选结论为 `json-serializer-candidate/conclusion.json`。未继续扩大核心优化范围。
独立候选证据包 SHA-256 为
`92bedd5a7547f3792f7b1d3eb976c93f8a3a9ceab9b5a905ea862e7e9dd2b0da`，
仅收录源、原始日志、命令、身份与复现脚本；map、对象及可执行文件留在数据盘，
不进入 Git。独立审计结论以 `serializer-probe-bundle-verification.json` 为准。

## 保留的失败、超时及运行环境调整

以下均为真实已结束的候选或诊断记录，不能被后续通过覆盖，也不计为最终门禁通过。
时间为 runner 记录的子进程耗时；负数表示信号退出，外层 shell 状态另列。

| 记录 | 实际状态 | 原因、处理与限制 |
| --- | --- | --- |
| `feature-release-0022` | 70/81，exit 8，1390.277 秒 | 跨 Store Actor/无条件 epoch 的旧测试假设，以及 sandbox/owned_capacity 原 90 秒超时；计费 E2E 通过。兼容补丁随后独立复核 |
| `feature-sandbox-no-compiler-retry` | 子进程 -9，外层 137，90.003 秒 | 同一 sandbox 二进制在 ext4 临时目录、无编译并发仍达原期限；进程检查无残留 |
| `feature-release-0022-final` | 79/81，exit 8，1335.301 秒 | 文件名虽含 final，仍是失败候选。未修改 HTTPS 300 ms 部分响应头夹具观察为空，未修改 sandbox 80 ms 逃逸夹具 PID 文本为空导致 `stoi`；R2.4 与兼容修改均通过 |
| `feature-clang-serial` | 79/81，exit 8，2240.899 秒 | sandbox 原 80 ms 逃逸夹具 `stoi`；worker_tools 原并发重放因可变操作版本派生内部命令而被 Store 指纹守卫拒绝。此运行属于修复并发问题前的 v1，后续确定性红绿与最小修复另行登记 |
| `feature-release-0002-serial` | 80/81，exit 8，1821.703 秒 | v1 第二次完整配置再次暴露同一 worker_tools 并发重放竞态，促成确定性反例、最小修复及 v2 全部重验 |
| `feature-asan-final` | 子进程 -2，外层 130，505.620 秒；未完成 | 原 GCC Debug ASan 全量构建及兼容重编译成功；完整 81 项尝试中 store/workflow_store/acceptance_store 达原 90 秒期限，在 event_runtime 时主动中止 |
| `feature-asan-r24-final` | 子进程 -2，外层 130，683.299 秒；未完成 | v2 的 31 项候选与两路原生测试并行；四个 Store 项和 cleanup 通过，event_runtime、workflow_runtime、acceptance_runtime、worker_tools 达原 90 秒期限，在 cleanup_runtime 时中止。输入和二进制前后相同；同一二进制后续独占重跑另计 |
| `feature-asan-v2-exclusive-diagnostic` | 0/4，exit 8，362.325 秒 | 同一 v2 二进制在独占、全部四个 CPU 的串行诊断中，event_runtime、workflow_runtime、acceptance_runtime、worker_tools 全部达到原 90 秒期限；输入和二进制前后相同，不能仅因 CPU steal 观察而排除这些失败 |
| `feature-asan-r24-deadline-diagnostic` | 子进程 -9，外层 137，92.087 秒；未完成 | 原 GCC Debug ASan 核心只完成前三个子例，`timeout --signal=TERM --kill-after=2s 90s script ...` 到期终止 |
| `feature-build-ubsan` | 子进程 -2，外层 130，1693.406 秒；未完成 | 原 GCC Debug UBSan 构建在切换本地优化配置时主动中止，未执行该配置测试 |
| `settlement-red-compile`、`settlement-green-compile` | 均 exit 1，分别 28.096、28.402 秒 | 新结算子例的 Boost.JSON/字符串比较类型错误；初始候选和日志保留，修正后重新编译再做正式红绿，不计为行为反例 |

同一 sandbox 二进制在私有 `/tmp` tmpfs 下 **19/19、exit 0、3.202 秒**，
`owned_capacity_tests` 在相同条件 **11/11、exit 0、32.501 秒**，见
`feature-sandbox-private-tmpfs` 与 `feature-owned-capacity-private-tmpfs`。
第二次完整候选的两个短期限失败，使用相同二进制、断言与期限分配独立 CPU 后串行
重跑为 **2/2、exit 0、16.203 秒**，见 `feature-release-deadline-recheck`。
原生单项重跑不替代完整门禁，后续 v1 完整 `0022` 81/81 另有独立完成记录；
两者均不能替代 v3 门禁。

v2 ASan 并行候选期间，`validation-host-steal.json/.log` 记录 `vmstat 1 5` 的
四个间隔样本中 CPU steal 为 58%、56%、60%、55%。这是实际观察到的虚拟机调度
限制，不单凭该指标将超时记为通过。执行者向该候选的 CTest 发送 SIGINT，随后发现
它留下记录中的 cleanup_runtime 测试子进程；核对路径、UID 和无子进程后仅向该进程
发送 SIGTERM。最终检查无遗留执行进程，原 runner 为 -2/外层 130，部分 CTest 详情
另存 `.partial.log`。证据为 `asan-v2-loaded-stop`、`asan-v2-orphan-stop` 和
`asan-v2-final-process-check`。随后同一 v2 二进制在独占、全部 CPU、原期限条件下
执行四项诊断仍全部超时，真实 exit 8；详情和进程检查见
`feature-asan-v2-exclusive-diagnostic-detail.log`、`asan-v2-exclusive-diagnostic-process-check.json`。
调度观察不能证明这些超时与实现无关，也不能将其视为已解决。v3 的两种 31 项
sanitizer 仍按原期限及插桩选项独占执行，须以新的完整结果判断门禁。

最终门禁的 `sudo unshare` 仅创建私有 mount namespace 并挂载 `/tmp` tmpfs
（2 GiB、1777、nosuid/nodev），随后立即用
`setpriv --reuid=1000 --regid=1000 --init-groups --reset-env` 执行普通用户 runner。
宿主 `/tmp` 不变；源码、构建和证据始终在数据盘。该条件不模拟断电持久性，不能据此
声称生产强沙箱或委派 cgroup 正向验收通过；缺少 bwrap/委派 cgroup 的实际环境限制保留。

本任务前序编译树调度至 CPU 0/1；v1 CTest 每个配置串行，Release `0022` 可用 CPU 2/3，
首轮 Clang 用 CPU 3，Release `0002` 用 CPU 2。不同配置可并行，但使用独立构建目录及私有
临时文件系统。记录核对普通 UID/GID、nice 0，CapInh/CapPrm/CapEff/CapAmb 全零。
v2 的三个完整原生测试均为串行 CTest。v3 继续使用普通用户和私有 tmpfs，原生 CTest
并行度为 2、sanitizer 为 1；先运行 Release `0022`（CPU 0/1）及 Clang（CPU 2/3），
再运行 Release `0002`（全部 CPU），之后两种 sanitizer 分别独占全部 CPU。
这项调度计划以 `final-validation-plan.json` 为准，运行是否完成仍以实际 JSON 为准。
没有扩大原 **80 ms、300 ms** 夹具期限或 **90 秒** 套件期限，也没有关闭 sanitizer。
调用 shell 的 umask 为 0022/0002；公共测试夹具内部仍会设置 0077，不能把外层 mask
说成每个夹具的实际 mask，生产 Workspace 权限检查保持原约束。

超时/中止后的进程检查另存 `sandbox-timeout-process-check`、
`release-loaded-failure-process-check`、`asan-debug-final-process-check`、
`asan-store-timeout-process-check` 和 `ubsan-debug-build-process-check`；
相应检查未发现遗留的受影响测试或构建进程。仍在运行的正式门禁由执行者继续跟踪，
不能以部分日志 Passed 行或后台存活代替完成记录。

## 同 v3 原二进制的完整重验

用户要求继续解决阻塞后，先以完全未改的 v3 event_runtime 原二进制执行原 90 秒
CTest，`feature-asan-v3-current-event-probe` 为 **1/1、exit 0、71.993601 秒**。
随后执行原 31 项完整相关选择，`feature-asan-v3-resumption-full` 于
`2026-10-29T17:55:47.417050+00:00` 开始，
`2026-10-29T18:16:38.677013+00:00` 完成：**31/31、exit 0、1243.067519 秒**。
这不是以单项探针代替完整门禁。实际 argv、起止时间、退出码和原始输出分别保存在
同名 `.json/.log`，最终 CTest 详情另存 `feature-asan-v3-resumption-full-detail.log`。

两次重验都使用普通 UID/GID 1000、capability 全零、umask 0002、CPU 0–3、私有
`/tmp` tmpfs、`--parallel 1` 和 `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`。
312 项运行输入及 57 项二进制/构建身份与早期 v3 完全一致，运行前后也保持一致。
所有注册名和原 timeout 均与早期冻结计划相同，没有重编译、候选替换、扩大期限或
并行编译干扰。工作流为 143.44 秒，完整计费 E2E 为 387.44 秒，R3 评测为 98.47 秒；
三个 R2.4 回归注册项全部通过。

本轮有效门禁集合为此前 v3 的 Release 两种 umask、严格 Clang、UBSan，加本次
完整 ASan/LSan，共五份实际通过记录。`summarize_resumption.py`、
`archive_resumption.py`、`archive_resumption.py --verify` 均实际执行 exit 0。
`asan-resumption-final-process-check.json` 在完整运行结束后只读检查进程可执行路径，
范围内无残留进程、无权限读取失败；这不构成任何业务清理的进程退出证明。

| 后续补充产物 | SHA-256 |
| --- | --- |
| `r2-4-resumption-results.json` | `9f809d100fb7928b86223b123b0b9d26dd8c06d4f61838bdebddc6e32c1b62ae` |
| `r2-4-resumption-evidence.tar.gz` | `8bee0bedbe1f730c494338dd302265e0e753630328a7661fa64c8681bc52e43f` |

新包包含 49 个成员，压缩 637156 字节，文本展开 2964971 字节；逐项摘要、日志与
命令终态、旧摘要原哈希均已复算，没有执行归档中的代码。原三包、历史摘要、冻结
计划保持原 SHA-256，历史 `local_validation_complete=false` 不改写；新的摘要独立
记录本轮有效门禁为 true。`resumption-plan.json` 是开跑后的选择说明，明确不称为
预注册计划；实际选择仍逐项核对更早冻结的原计划。

`evidence_audit` 随后独立只读核验上述五门禁、49 个归档成员、12 份完整命令记录及
当前 312/57 项身份，结论为 `audit_passed=true`、无阻断发现。审计记录
`resumption-independent-audit.json` SHA-256 为
`e339a3cba709e54c96159b465690509300492a22f972ce5ecb5afb26a9d7b7c9`；详见复核文档。

本轮准备的 SAX 验证/serializer 组合和单次 restore 字节复用候选均留在 feature
外部，没有合入。前者只有 4 次编译、6 次链接通过，所有 oracle/运行测试均
`NOT_RUN`；后者未编译、未运行。补充包只收录小补丁和静态说明，不把它们计为
新增或通过测试。环境采样和编译时间改善只是当时观察，后续同二进制成功不能
证明历史失败的唯一根因或任意负载下的期限保证；原失败和诊断边界仍完整保留。

## 独立复核、历史完整性与发布状态

独立只读 Agent `independent_recovery_review` 已复核生产边界、新增回归和实际兼容补丁，
在本卡限定范围内未发现剩余缺陷；并发问题先被确认为基线已有竞态，随后最小修复、
确定性回归和红绿候选输入又经独立只读复核，未发现阻断合入的问题。
结算故障会话键和最终 20 子例测试也已追加静态复核：三处 tuple 一致，原 19 子例、
helper、期限及持久 UNKNOWN/全局容量守卫保留。夹具投影和类型修正后，正式红绿由
主执行者运行；审查者没有将候选执行或完整门禁冒认为自己的测试。
检查重点是 Store 事务/写者锁、公开 Actor 路径、精确
重放、WorkerTools 前置副作用、旧 claim、退出和所有权条件；补充了 PENDING-only
及控制限流回归。复核文档记录输入哈希，之后又确认 `writer_session` 与既有持久化
`writer_instance` 的措辞准确。审查者未运行构建/测试、未核验推送/CI；同一模型体系的
工程复核不能替代主协调 Agent 的独立审核。

最终 `protected-files-publication-final.json` 已逐项核对 323 个原基线文件的字节、类型和 Git 模式，
独立审计再次验证当前文件相符；较早核验记录同样保留：

| 原有范围 | 文件数 | 字节数 | 实际结果 |
| --- | --- | --- | --- |
| `db/`，包括已发布 SQL | 12 | 131063 | 无差异 |
| `third_party/` | 31 | 399041 | 无差异 |
| `docs/handoff/` | 83 | 773396 | 无差异 |
| 基线中的 `docs/reports/` | 197 | 6076206 | 无差异 |

`git diff --check`、`git status --short --branch`、`git diff --stat` 和 `git diff`
均已真实执行并保存 argv、原始输出及 exit 0。交付扫描中的 35 个当前文本改动文件
未发现常见凭据、环境文件、临时数据库或构建产物；新增归档另经逐成员检查。

同 v3 的五项有效门禁均已完成并通过，ASan 本地验证阻塞已解除，没有豁免门禁。
此前失败的机器摘要及原始证据保持。提交前必须再次 fetch main，若其前进则停止
发布并交协调 Agent 决定整合。提交、远端分支、该 SHA 的全部 Actions 和源码归档
与 Git 树核验在发布后另外登记，不能用当前本地通过替代远端证据。

本文件冻结提交前的本地证据，避免在提交内容中伪造其自身尚不存在的最终提交 SHA。
发布后的交付回执应提供实际完整 SHA、GitHub 分支链接、该 SHA 的 Actions 链接和结论，
以及最终源码快照核验结果。本文件登记点为提交前；本地验证已完成，尚不声明发布
或 CI 完成。原 GCC Debug sanitizer 的本地未完成记录、v2 ASan 独占四项超时及
生产强沙箱/断电持久性不在本批证明范围的限制均保留。本地 sanitizer 选择为 31 项，
原远端 GCC Debug 全部测试和 Make-ASan 须由本批最终 SHA 的实际 CI 结果补证。

| 状态 | 本次登记的真实状态 |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | 已完成按需恢复、身份隔离、并发重放及结算故障会话隔离，新增回归与限定范围独立工程复核完成 |
| `LOCAL_VALIDATION_COMPLETE` | 已完成：v3 四种默认目标增量构建、Release 两种 umask/严格 Clang 各 81 项、UBSan 31 项及同原二进制 ASan/LSan 31 项均通过；历史失败原样保留 |
| `PUSH_COMPLETE` | 本报告登记于提交前；实际正常提交、推送及远端分支核验由发布后交付回执记录 |
| `REMOTE_CI_COMPLETE` | 未完成；尚无本批实际提交的远端结果 |
