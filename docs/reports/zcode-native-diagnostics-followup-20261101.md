# ZCode 原生测试观测与本机诊断续报（2026-11-01）

本轮完成了六文件测试观测及工具兼容修复，并取得有效的工具红绿结果。产品完整本机验证仍未完成：新 ASan Async 诊断出现原五秒等待失败，并达到原 180 秒套件期限；历史三个正式 Timeout、ASan 资源探针断言失败及更早 GCC/Clang/live_workflow 问题全部保持 **OPEN**。本报告不以单方法诊断、自检或性能采样替代正式矩阵。

本报告的原工具及诊断执行截点为 `2026-11-01T03:24:58Z`，限定工具提交为 `579e3c7675bc2abb336dde6a95bfddf2139f47cd`。后文单列截至 03:39:59 的 Release 构建及 03:41:38 的 PR/分支回执，不改写此前结果。该 SHA 的最终远端 CI、完整源码快照和可能的生产性能修改须由各自新收据登记，不从前一提交继承结果。

**基线、范围与证据位置。** 本轮基线是 `7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a`，分支为 `fix/zcode-local-validation-20261101`。它继承原 `99c9063f3e29cd6d69c56fb15815715d499a374a` 审核后两项 P1 修复；本轮工具提交没有再次修改生产源码。前批事实与未决事项见 [原整改报告](async-review-99c9063-remediation-20261031.md)，其历史内容保持原样。

任务数据盘根目录为 `/mnt/d/codex-workspace/wls861-zcode-followup-20261101-wvm7aclp`，以下 `E/` 表示该目录的 `evidence/`。它是本机原始大日志索引，不是可公开访问的链接。每轮 `command.json`、`completion.json`、原 stdout/stderr、source-before/after 与专用评估收据按原路径保留；需要公开复核的文本收据包及成员 SHA 清单由发布者另行登记，不能把本机路径当作已发布附件。

本轮已准备供 root 检查的文本包 `E/public-tool-receipts-01/zcode-native-diagnostics-receipts-20261101.zip`，223,575 字节，SHA-256 为 `2da6c77fe735808aa3071cb478ddceeaee43ef77c73dc49ae2c12351c2d7a822`。174 个成员均为 UTF-8 普通文本，含 README、来源映射、成员清单、SHA256SUMS 和扫描收据；20 轮已结束 recorder 记录及六轮编码工具身份分别保留，不计作 26 套测试。原 stdout/stderr、completion、限定复核、冻结探针/helper 源、JUnit 与两份各自保存的 LastTest 保持原字节；命令/编码工具字段投影及 helper 的 AST 字面量提取均显式标注来源 SHA。全量 792/793 项清单以原 SHA/计数、变化项和关键输入身份压缩呈现。CRC、173 项校验和、成员字节/路径/类型及列明的秘密模式检查通过；没有打包二进制、gmon、数据库、settings/provider 配置或原始进程/环境 dump。扫描是有界模式与明确选择范围，不能代替 root 发布前检查；该包此时仍是本机候选，未声明已公开发布。

发布依赖为 [Draft PR #12](https://github.com/wmqfl861/wls861/pull/12) → [PR #11](https://github.com/wmqfl861/wls861/pull/11) → [PR #10](https://github.com/wmqfl861/wls861/pull/10)。03:41:38 完成的原 API 回执确认 PR12 的 base 为 `fix/async-review-99c9063-20261031@7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a`，head 为 `fix/zcode-local-validation-20261101@579e3c7675bc2abb336dde6a95bfddf2139f47cd`，Draft/open、未合并；远端分支 ref 也指向此工具提交。原件见 `E/ci-579e3c7-audit-01/pr12-refresh-01.json` 及 `branch-ref-refresh-01.json`。原 PR 的 base 未由本报告改写，主协调者的原 CHANGES_REQUIRED 不因工具审核而自动关闭；本报告与文本包自身尚未发布。

**实际 ZCode 作者调用。** root 按用户要求运行本机已安装的 `zcode-app-cli 3.16.3-39 / runtime 0.17.0`，使用既有 `bigmodel-individual-coding-plan / GLM-5.3`。round01–05 由 ZCode 编写和修正测试观测工具，root 与非作者 Agent 阅读实际差异、提出修正并执行验证。round06 只提出性能候选，尚未作为生产修复实施。

| 调用 | 开始—结束 UTC | 实际 exit / 秒 | 证据 |
| --- | --- | --- | --- |
| round01 | 02:44:29—02:53:31 | 0 / 542.397338 | `E/zcode-round-01/` |
| round02 | 02:57:18—03:02:39 | 0 / 320.960195 | `E/zcode-round-02/` |
| round03 | 03:03:59—03:08:03 | 0 / 244.826328 | `E/zcode-round-03/` |
| round04 | 03:10:51—03:11:20 | 0 / 29.563825 | `E/zcode-round-04/` |
| round05 | 03:12:18—03:15:10 | 0 / 171.486300 | `E/zcode-round-05/` |
| round06，只读建议 | 03:18:21—03:24:58 | 0 / 396.321336 | `E/zcode-round-06/` |

这些 exit 0 表示编码工具调用结束；代码正确性由后列审核及真实执行判断。各轮 prompt SHA、实际 argv、JSONL 输出身份和 stderr 身份记录在 start/completion 中。调用使用数据盘上的 `ZCODE_CLI_SETTINGS_FILE`、`ZCODE_SESSION_DB_PATH` 与 TMPDIR；既有用户 settings/provider 配置只读，其当前字节数和 SHA 与 `E/zcode-user-config-before.json` 一致。会话数据库与设置文件不进入提交。

用户对编码工具的授权不扩展到 WLS 产品供应商调用、付费 API 测试或生产操作。本批产品诊断继续使用 FakeModel、私有临时 SQLite/文件和合成工具夹具；没有把编码工具输出当作供应商质量、费用或产品验收证据。

**六文件实际变化。** `git diff 7dc80ed…579e3c7` 仅包含下表；`src/`、`include/`、数据库/SQL、CMakeLists、Makefile、原 92 项 Linux CTest 名称及期限、Windows 组件均无差异。实现版本和 Store 版本未升级。

| 文件 | 实际变化 | 最终 SHA-256 |
| --- | --- | --- |
| `tests/test_support.hpp` | 每方法 START 及原 PASS/FAIL/RESULT 立即刷新；保留原顺序、异常捕获、计数、0/1 退出、0077 隔离与 umask 恢复。 | `5b44c995a3c1a8e93774aa5d4f3a672a23d49571e34e6f45fd8ea197aad20922` |
| `tests/async_coordinator_tests.cpp` | 原 until 增加调用位置诊断；同一次 clock 采样供日志与 CHECK，保留五秒期限及 predicate/poll/CHECK/yield 次序。 | `c6d97d89ee087a4f74a896aac2c86c4687bd8aa34550e369f9dc4cd324ae4c48` |
| `tests/test_native_test_observation.py` | 新增原始 pipe 握手、SIGKILL、失败释放和全成功的合成自检，核对精确行序、失败文本、计数、退出、umask 和子进程回收。 | `5fd83d75567861d61d8ab4438d833ab03125e9f01a988d1aaba2d70dfc5ca694` |
| `scripts/run_a4_fairness_red_green.py` | 精确接受原输出或一条同名 START 前缀；保留原退出、空 stderr 或目标业务失败定位校验。 | `5347e26f53724a39dda0cdb269da1523f254891e8f610a05d95b538862b64727` |
| `tests/test_a4_fairness_gate.py` | 验证两种合法格式及错误标签、重复/乱序、缺 RESULT、附加输出、错误 stderr 等拒绝条件。 | `d7daa243d10ee199b589f090befc2f8ca540062150e20060ccb45c244af8c3ad` |
| `.github/workflows/ci.yml` | strict/ASan 路线增加独立 native 观测自检；没有新增产品 CTest 注册。 | `b86d320bf38e7a314685d980db23e968c020e74b57f3955b53ae6960e4992f27` |

**确定性工具反例及回归。** 下表均有真实结束记录，UID/GID 1000、外层 umask0022、独立数据盘 TMPDIR、outer_timeout=false。秒数为外层 recorder 实际时长。

| 执行记录 | 实际内容及结果 | exit / 秒 |
| --- | --- | --- |
| `E/native-observation-old-red-02/` | 同一最终自检配 7dc80ed 原 support；编译成功，到达独立 REACHED-BLOCKER 后预期八行而实际三行，立即目标失败；helper 实际 -9，reaped=true。 | 1 / 3.258715 |
| `E/native-observation-zcode-green-02/` | GCC13 严格编译成功；kill、release-failure、all-pass 三个合成场景通过，helper 实际 -9/1/0，均 reaped=true。 | 0 / 3.253416 |
| `E/native-observation-clang-green-01/` | Clang18 严格编译成功，同一三个合成场景通过，helper 实际 -9/1/0，均 reaped=true。 | 0 / 4.286189 |
| `E/fairness-classifier-old-red-01/` | 相同新版九方法配原 classifier；8 PASS、1 FAIL，唯一目标错误为新 START 格式的 NOT_GREEN != EXPECTED_GREEN。 | 1 / 0.805665 |
| `E/fairness-classifier-zcode-green-01/` | 九方法全部 PASS。 | 0 / 0.788449 |
| `E/existing-gcc-runner-selfchecks-01/` | GCC gate runner 24/24 工具方法 PASS。 | 0 / 1.258492 |
| `E/existing-immediate-runner-selfchecks-01/` | 即时 unittest 证据工具 4/4 方法 PASS。 | 0 / 0.863927 |
| `E/existing-transport-observer-selfchecks-01/` | transport observer 11/11 工具方法 PASS。 | 0 / 0.742258 |

native 是**一个 runner 的三个合成场景**，在两种编译器上分别运行；不是三套产品测试，ASan CI 路线里的这项 helper 也不自动成为产品 ASan 套件。fairness9、runner24、immediate4、transport11 各按自身工具方法计数，不与 92 项产品目录相加。

实际工具命令包括 `env CXX=/usr/bin/g++-13 python3 -B tests/test_native_test_observation.py`、Clang18 同形命令、`python3 -B tests/test_a4_fairness_gate.py`，以及 `test_run_gcc_asan.py`、`test_immediate_unittest.py`、`test_transport_test_observation.py`。旧版命令仅将对应被测旧输入替换到冻结目录，完整绝对 argv 保留在各 command.json。native 两边使用相同 HELPER_SOURCE，SHA-256 为 `daa7c7493a5f7b53f4586a2916cac81e8cc90003cc6cbd3644f6877f14d635aa`；旧 support SHA 为 `78b6079d252ded46bb22b086fc40309a2486411fac35dd924e638e67b74c3881`，逐字等于基线 Git 对象。

旧反例实际已经输出原 PASS、原 FAIL 正文和独立 marker；它证明缺少要求的即时完整观测，不能写成“原 FAIL 正文丢失”。`native-observation-old-red-01` 的首次 guard 失败（exit1，34.597314 秒）依赖等待旧版没有的新增 START，不计为最终确定性反例，旧日志仍保留。后续审核发现并修正了双 clock、握手可能隐式 flush、参数 tuple 顺序和失败清理回执问题。round03 初读的 tuple 错误与稍后冻结副本并非同一版本，最终 review 明确更正这一归属，没有覆盖旧审核记录。

八轮工具执行的 792 项 tracked 输入 before=after；当时新增 native 自检尚未跟踪，另以最终审核冻结 SHA、旧输入清单及 HELPER_SOURCE 身份核实，不能谎称它已包含在该 792 项清单内。三个临时 helper 二进制的运行时 SHA 保存在日志；临时目录已按设计删除，不能改称现在仍保留可执行字节或 binary before/after 双清单。

**产品诊断的实际结果与边界。** 以下均为 `DIAGNOSTIC_NOT_FULL_GATE` 或 `DIAGNOSTIC_PROFILE_NOT_GATE`，UID/GID 1000、umask0002、数据盘 TMPDIR。保留 `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1` 和 UBSan halt_on_error/print_stacktrace；仅设置环境变量不把 Release 二进制变成 sanitizer 构建。各诊断使用冻结测试副本并链接保留的原生产库，没有通过放宽期限或关闭 sanitizer 取得结果。

| 记录 | 实际结果 | CTest 子项秒数 / recorder 秒数 / exit |
| --- | --- | --- |
| `E/diag-asan-async-run-01/` | 29 方法中 22 START、20 PASS、1 FAIL、0 RESULT、7 未开始；原 180 秒 Timeout。 | 180.175 / 181.034784 / 8 |
| `E/diag-asan-resource-run-01/` | 一个原资源方法 PASS；真实四调用、accepted4、cycle0、FIFO preserved。 | 9.29525 / 9.763713 / 0 |
| `E/diag-release-owned-run-01/` | 原 owned_capacity 的 11 方法 PASS，原 90 秒期限保留。 | 23.2478 / 23.847137 / 0 |
| `E/diag-asan-fairness-profile-run-01/` | 原 fairness 一个方法 PASS；仅 wrapper 增加 -pg，原生产 archive 未重编。 | 39.9306 / 40.393850 / 0 |

前三轮各自执行 `ctest --test-dir <E>/diagnostic-preparation-01`，分别使用 `-R '^asan-async$'`、`-R '^asan-resource$'`、`-R '^release-owned$'`，均带 `--verbose --output-junit` 并写各自独立 XML。这里只缩写绝对根目录；真实完整 argv 以各 command.json 为准。profile 命令指向 `E/fairness-cpu-profile-01/`，仅登记一个诊断子项。每轮只有一个 CTest 子项，方法数量不当作独立套件数量。

ASan Async 第 18 方法 `queued_s1_requests_and_teams_rotate_without_starvation` 在冻结测试第 895 行调用 until 后记录 `UNTIL-DEADLINE`，对应释放首个 blocker 后等待 `sequence->read().size() >= 1`。它之前的八 blocker 入场等待已返回，真实请求和任务准备也已完成；首次公平顺序断言尚未执行。原 helper 在 poll 后、下一次 predicate 前检查 deadline，因此日志证明该时刻已越过五秒，不能证明 sequence 仍为空、单次 poll 的耗时、死锁或调度顺序错误。之后三方法 PASS，证明该次异常已经返回 runner。

第 22 方法 `logical_question_wait_releases_slot_without_releasing_unknown_budget` 仅有 START；没有开始时间戳或阶段日志，不能把整套 180 秒归作该方法独占时长，也无法区分 setup、drain、断言或析构。第 23 资源方法及后六方法未开始，不从这一轮获得通过或失败结论。诊断评估、逐方法映射、原 stdout/JUnit 对应与进程组现状见 `E/diagnostic-async-assessment-01/`。原 Async 独立 stdout/XML 保留；共享 LastTest 已由后续轮次覆盖，不能冒充其原日志。

两轮 recorder 明确记录 `source_unchanged=false`：Async 期间 tracked 的 fairness classifier 和 Python 自检两文件变化；resource 期间仅 Python 自检由 `62d3909b…` 变为最终 `d7daa243…`。它们不是被冻结 C++ 二进制的编译/运行输入，后者通过专门测试/库/二进制身份核对保留限定诊断价值；仍不得把全候选输入写成全程冻结。Async 时新增未跟踪文件另有变化，不属于 792 项 tracked 清单。Release-owned 和 profile 轮的 tracked before/after 相同。并行编辑不是失败原因的证据。

profile 保存了 gmon、flat profile 和身份清单。可执行文本 PC 直方图的高位样本包含 Boost.JSON 解析、字符串处理与序列化；生产库没有 -pg 插桩，因此没有完整库调用次数/调用图，也不全面覆盖共享库 CPU 或 off-CPU 等待。这个单方法成功及热点观察既未闭合原 Timeout 根因，也未证明任何生产优化已经实现。`E/fairness-cpu-profile-01/profile-identity.json` 记录的 binary SHA 为 `ebedf26e8936fcff55249ac7f062316842e7f086955ba592110b4e38c18729e1`，gmon SHA 为 `e0daca7c00381533f5818d25d4a29aec24ca5546ef4285167a284134c1e7cdaf`；round06 的候选建议需另行最小设计、红绿与适用完整验证。

**后续构建事实。** root 对工具提交 579e3c7 执行 GCC13 Release 完整默认构建：configure 于 03:28:17 完成，exit0、2.268402 秒；`cmake --build build-zcode-release --parallel 2` 于 `2026-11-01T03:39:59.236208Z` 完成，exit0、660.162950 秒。两轮各 793 项 tracked 输入 before=after，UID/GID1000、umask0022，收据分别为 `E/candidate-release-configure-01/` 和 `E/candidate-release-build-01/`。此追加事实只证明构建结束，尚未据此登记产品测试通过。随后 ZCode round08 正在加强 test-only count probe；事件生产性能优化未实施，后续事件补丁及验证归入独立报告。

**非作者复核。** `/root/evidence_audit` 对最终六文件作只读复核，结论为 `NO_BLOCKING_FINDINGS_IN_LIMITED_TEST_TOOL_SCOPE`；随后读取真实执行收据，结论为 `VERIFIED_TOOL_RED_GREEN_AND_SELFCHECKS`。它未编写这六文件补丁，也未运行这些测试；执行属于 root。该审核为同模型非作者工程复核，不是第二模型评测、全仓安全证明或主协调者正式 PR 采纳。

| 本机复核记录 | SHA-256 |
| --- | --- |
| `E/native-observation-review-final/review.md` | `9bbe430d9bf12f9649f05e891c201b800077542e42bed99911721d974ae220d6` |
| `E/native-observation-review-final/reviewed-inputs-and-fairness-evidence.json` | `3b77a250c41e1d4824e7403831ae600c84e09f4fe5217b3b06509dbbcb95ed48` |
| `E/native-observation-execution-audit-01/review.md` | `0f0522cc9bbba644b38257466b5e24bffa2e32ef02624c12ca18f0661ab215be` |
| `E/native-observation-execution-audit-01/verified-executions.json` | `21273676f303176f2be0d07ead1b43c21e147a60e392cc421d6c26e04c7c4897` |

执行复核逐项确认七个实际 native helper 均 reaped，且读取时八轮 runner/recorder/helper 共 23 个记录 PID 已不存在。该事实只证明所观察测试进程已结束，不授予业务清理权限或替代产品退出/结算证据。

**未决事项与交付状态。** 新诊断不改写前批三个正式失败：Release0002 owned_capacity、UBSan Async、ASan/LSan Async 的原期限 Timeout，以及单列 ASan resource 探针五秒断言失败。更早 GCC 48 PASS/2 FAIL/39 Timeout、Clang 29/39/10 Timeout、旧 live_workflow 根因继续开放，`historical_clang_asan_failures_closed=false` 保持。完整本机 Clang/sanitizer 尚缺的原范围、Windows 11 普通用户实机验收及完整 Windows runtime 也未由本轮完成。

| 状态 | 本报告证据截点的准确范围 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | **LIMITED_TOOL_SCOPE**：六文件测试观测及工具兼容已实现并形成 579e3c7；没有声称本机失败根因修复完成。 |
| LOCAL_VALIDATION_COMPLETE | **FALSE**；上述工具自检通过，新 Async 诊断失败，完整产品矩阵及历史问题保持开放。 |
| PUSH_COMPLETE | **TRUE_FOR_TOOL_579e3c7**；远端 ref 与 Draft PR12 head 已有回执。本报告/公开文本包自身的最终交付仍 **PENDING**。 |
| REMOTE_CI_COMPLETE | **PENDING**；尚未以本报告登记新精确 SHA 的 Actions、实际 checkout 及源码快照核验结果。 |
| PR_REVIEW_COMPLETE | **PENDING**；同模型限定工程复核不代签主协调者接受。 |
| MERGED | **FALSE**；没有合并或自动合并授权/结果。 |

后续交还字段为本报告及文本包的最终发布完整 SHA、该 SHA 的远端分支读回、Actions/源码树核验及可公开文本收据包身份。事件路径最小性能优化须单独记录真实代码、验证和限定结论；本报告当前仅记录候选建议及 test-only probe 进行状态。
