# N5-1 持久事件异步 runtime：源码检查点

本报告时间范围为 2026-11-02 的本地源码检查点；**尚未编译、尚未执行新增 C++ 测试、尚未推送**。
本文件不表示 N5-1 验收完成，也不替代 Store 411、完整回归和最终 SHA 的远端 CI 证据。

## 已确认输入与授权

- 原始源码基线：`451b232a0dde08220a5e096d91209087b027b42e`。
- 实际工作分支：`feat/n5-1-async-events-20261102-csj777e9`。
- 已保存的运行时源码提交：`ead72f103b2f0c134dd8f9ce8b7c79a19eac9079`。
- 该源码树：`8d97aeedf1010b9a57ae50f63658f0742d47524b`。
- 工作目录：`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/async-events`。
- 主协调 Agent 已转交用户正式采纳的方案 B / 013 / 411 完整契约；本次仅实现其第一离线纵向切片。
- Store 作者拥有 `store.hpp`、Store 实现及 013；本检查点尚未合入其提交。正常合并及构建注册由协调流程接续，禁止据此源码检查点声称可独立构建。

代码位于数据盘。没有调用真实模型、付费 API 或生产服务；新增 fixture 使用既有临时目录、FakeModel 和临时 SQLite。
正式验证前须继续使用数据盘 `TMPDIR` 和普通用户，并核验实际输入和二进制身份。

## 代码清单与实际行为

| 文件 | 本检查点变化 |
| --- | --- |
| `include/wls/persistent_event_runtime.hpp`、`src/persistent_event_runtime.cpp` | 新的显式有限批次 host；拥有既有 `AsyncCoordinator`，每请求每轮最多准入一个新调用，其他独立请求的回调等待不构成整轮屏障；保留冻结管理上限、每任务 Worker 上限和有界结果容量 |
| `include/wls/persistent_event_selection.hpp`、`src/persistent_event_selection.cpp` | 从实际 T0 捕获集合 C 选择 S；核原 requirements revision、目标 task 和已支持事件类型，note 还核原完整 binding；共享原同步管理目标匹配逻辑，空 claim 仍执行原前置形状校验 |
| `include/wls/async_coordinator.hpp`、`src/async_coordinator.cpp` | 新增显式 `submit_event`，结果槽及可用离线 adapter 先于 T0；T1 同原捕获调用绑定，owner 线程执行 T2、T3、T4，模型线程不访问 Store；T1 已返回后异常或无法证明无关联的失败保留原 Job/claim/容量并 fence；UNKNOWN 保留结果并停止新业务 |
| `include/wls/offline_runtime.hpp`、`src/offline_runtime.cpp` | 抽取只构建原预约值的 `make_offline_reservation`；原同步预约继续调用同一校验及原 Store 入口 |
| `src/event_runtime.cpp` | 同步 action-in-claim 匹配提取为共享纯函数；保持有效目标语义及 malformed/empty-claim 前置校验，不启用异步默认行为 |
| `tests/persistent_event_runtime_tests.cpp` | 13 个新方法，计划注册为一个独立 `persistent_event_runtime` suite |
| `tests/persistent_event_legacy_probe.cpp` | 可对原 410 与新 411 显式模式构建的独立能力差异 probe，不计为第二个产品 suite |

第一切片仅支持 plan、捕获原任务的模型 AMEND、instruction、原 owner ack、note。
其他工作流目标保持拒绝或可见的待处理状态，没有 live、文件或进程执行器，也没有新增调度池。
`future` 就绪只表示宿主完成通道；报告读取 Store 中的原 link、逐 ID dispositions 和 completed IDs。
空 S 经 typed release 释放无关联的原 claim，原 snapshot 和未完成 ID 保留。

旧 link 不能用新 Actor 或新恢复 ID 自动续用。Store 作者约定 T0/T1 对同 Actor 范围的未 CLOSE link 保持 HOLD。
epoch-only 恢复下已经 current 的 instruction 不重发；其 TeamLead 恢复 notice 可以保留 PENDING，不能借另一 Worker 的 ack/note 消耗。
关闭和显式证据重试仅保存原 T3 观察，绝不重试 T4 或模型回调。未取得 T2 的调用不能凭宿主生成的结果补造派发事实。

## 新方法与当前执行状态

实际测试源包含以下 **13 个方法 / 1 个新产品 suite**，全部 `NOT_RUN`：

1. 原目标形状在空 subject 和空 claim 前仍受校验。
2. selector 使用实际 C，保留前页 HOLD、无关目标、旧 revision/binding 与迟到 ID 边界。
3. 缺少脚本以及旧入口拒绝不新增 claim 或 reservation。
4. 两请求各两个独立节点，在一个 TeamLead 回调等待时仍推进另外三个节点至 note。
5. 捕获后生成的 feedback ID 只由后继调用自己的证明处理。
6. 部分模型 AMEND 保留省略节点的具体 ID，后继 keep 仅处理该原 ID，已有 sibling 不再变更。
7. UNKNOWN 保留 nullable 原观察、无业务/ID 完成，停止批次新准入，flush 仅重放原证据。
8. cancel 后迟到响应保持原 request/response，拒绝过期业务与 ID 完成。
9. RESERVED 和 DISPATCHED 的重启均保留原 Actor/link HOLD，拒绝旧 Actor，重复 poll 不消费新 claim 配额。
10. T1 后注入 CLOSE 持久化失败，回滚业务并保留容量；移除故障后 flush 仍不应用 T4。
11. close 停止并 join 回调后只保存原观察，不应用业务，不确认 ID。
12. 声称 real adapter 的合成拒绝 fixture 和超出协议上限的配置均在 T0 前失败。
13. 恢复后空 S 通过真实 `submit_event` 释放无关联 claim，保留原待处理 ID 和预算。

拟议 CTest 名为 `persistent_event_runtime`、可执行文件 `persistent_event_runtime_tests`，新 suite 原始期限为 **180 秒**。
其异步观察 barrier 固定为 **5 秒**。未扩大任何旧 suite 或原 barrier 期限。
实际 CMake、Make、catalog、runner default 和 CI 注册须在 Store 分支整合后统一核对；本检查点没有独立修改计数。

独立 legacy probe 只有一个方法：同一业务条件下，admit/claim 后未预约调用、未结算、无反馈，尝试原 `finish_events`。
原 410 合法旧契约允许该操作，新 411 显式模式必须拒绝绕过关联证明的旧入口。
probe 比较实际 `attempts=0`、`feedback=0`、DONE 数量和原请求/预算/事件；预期旧侧 oracle 为 red，新侧为 green。
**两侧均未运行。该差异不是已复现的历史生产漏洞，不能用新 API 缺失导致的编译失败冒充旧版业务反例。**
实际旧 ELF 拒绝打开 411 数据库属于另一项兼容证据，由主协调 Agent 排程。

## 已执行的检查与证据身份

| 命令或检查 | 实际结果 |
| --- | --- |
| `git apply --check .../coordinator-offline-event-v2.patch` | exit 0；共享补丁由主协调 Agent 审阅并应用 |
| `git apply --check .../empty-claim-validation.patch` | exit 0；共享同步校验补丁由主协调 Agent 应用 |
| `git diff --cached --check` | exit 0 |
| `git diff --cached --stat` | 11 个文件，1,368 行新增、44 行删除 |
| 正常 `git commit` | exit 0，生成上述完整源码 SHA；提交正文明确尚未整合编译、测试未执行 |
| 源码提交后 `git status --short` | 干净（本报告创建之前的时间点） |
| 六个自有新文件冻结字节与提交 blob、工作区比较 | 全部一致；这是源码身份检查，不是编译或产品测试 |

私有原始输入清单位于任务证据目录的 `n5-1-runtime-source-freeze-01/receipt.json`，SHA-256：
`2027fe79c733d2316f8628b508b7faec68c188395c51033ed03fd67f3f080e9e`。
提交树逐文件核验在同目录 `post-commit.json`，SHA-256：
`f1ee34f25110900419de942530ba9610f6905e08a00f406d2d99b2101357df05`。

- `tests/persistent_event_runtime_tests.cpp`：`27f32f1c42b182eb47822252e9a1f94018802b5fb364812489af2ca680b81fce`。
- `tests/persistent_event_legacy_probe.cpp`：`24540cefd84d5bf722a220213a1cf63f432fdef5dae57f8245000805c2300aa5`。

Store 作者将对本检查点 runtime 进行非作者复核，当前结果尚未收到。
执行 Agent 对另一个作者的 Store 11 个事务抽取进行了限定范围的非作者只读审核，未发现该范围的守卫丢失或新嵌套 BEGIN；
该审核不替代本 runtime 的独立复核，也不涵盖新 013、`.inc` T0–T4 或容量算法。
审核源码 `src/store.cpp` SHA-256 为 `e96246ed9e5c062a5197df08ccf22120a60920289f46e1fa7c5c3dadb411371e`，
证据 `n5-1-store-extraction-review-01/review.json` SHA-256 为
`29c54b9dd3366c644d084105328de09a4e5da4a0d8817a730aada292d5b0f438`。它只是源码审核，不是正式 approve。

## 待完成事项和真实状态

Store 411 正常整合、实际构建、两个 probe 实跑、新 13 方法、旧同步/异步完整回归、两种 umask、sanitizers、严格 Clang、最终二进制身份、独立复核和远端 CI 均尚未完成。
没有将未完成的正式 N5-0 构建或后台测试计入本模块成功。当前依协调约定暂不启动新的重编译，以免与正式验证混跑。

| 状态 | 本报告时间点 |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | **NO** — 源码检查点已保存，Store 集成与可执行性待验证 |
| `LOCAL_VALIDATION_COMPLETE` | **NO** |
| `PUSH_COMPLETE` | **NO** |
| `REMOTE_CI_COMPLETE` | **NO** |
| `PR_REVIEW_COMPLETE` | **NO** |
| `MERGED` | **NO** |
