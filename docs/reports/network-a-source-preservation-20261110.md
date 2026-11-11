# Network A：已审源码保存与验证状态（2026-11-11）

**整体状态：OPEN / FAIL。** 本次在现有 [Draft PR #50](https://github.com/wmqfl861/wls861/pull/50) 保存已独立审阅的三个受控时间 fixture 测试文件。新的实际独立审核总体为 **FAIL**；有限有效结果为 Clang 18 与 Release 各一次原 resolver 母测试 **100/100 PASS**，以及一次指定单表达式敏感性对照 **97 PASS / 3 FAIL**。这些结果不构成整个 Network A 或完整矩阵通过。Store 版本保持 **410**。

候选产品源码清单仍为 **685** 个文件：相对旧快照仅三个测试文件改变，另外 **682** 项字节、大小和模式不变；本页单独作为公开摘要。相对 PR/main 基线 `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631`，生产/构建改动累计仍为 **+790 / −10 = 800**，预算余量为 0。此次待审差异的当前 HEAD 为 `1e760db268b3986b46b482a879c671b24c45e37a`；普通提交须使用提交时实查 HEAD 为父。

| 验证项 | 实际结果与范围 |
| --- | --- |
| 两缺陷 RED/GREEN | 原独立审核 **scoped PASS**；修复后原 resolver 母测试 **100/100 PASS**，原 81 项及新增 19 项顺序完整。只执行 1 项 CTest，注册集合为原 79 项。 |
| INSTALL / 普通 Make | 原独立审核 **scoped PASS**；真实 Make 构建、resolver **100/100 PASS**、资源安装 **12/12 PASS，无 skip**，包含安装与迁移后生产 helper 检查；不具有完整 CTest/JUnit 运行身份。 |
| 原 Release / umask 0022 | 原 79 项尝试为 **40 PASS、第 41 项中断、38 项未开始**，exit **1**、资源观察失败、await 晚约 **48 秒**、79 项 JUnit **缺失**。同一旧 ELF 的 C39 续跑为原第 41..79 项 **39/39 PASS**，真实 39 项 JUnit；资源内部 **11 PASS / 1 项 Make 专用 skip**。独立限定覆盖复核 PASS，仅为旧输入的两次覆盖，合计 **80 次已开始调用**，没有一次连续 79 PASS 或合成 79 项 JUnit。 |
| 原 Release / umask 0002 | 独立限定实际审核 **PASS**：旧输入的一次 **79/79 CTest PASS**，真实 79 项 JUnit、0 failure/skip；resolver **100/100 PASS**，资源内部 **11 PASS / 1 项 Make 专用 skip**。不重标为新三测试输入的 full79。 |
| 原 Clang 18 | **51 PASS / 1 真实 FAIL / 第 53 项中断 / 26 项未开始**；resolver 原母测试 **99 PASS / 1 FAIL**，`lifecycle_submillisecond_budget_is_exact` 的 `facts>=1` 未满足；历史原始 facts、RESULT、绝对 deadline 和 Error 仍为 **UNKNOWN**。 |
| O1 观察 | **仅诊断**，原独立审核 `PASS — LIMITED_DIAGNOSTIC_ONLY`；新观察 `facts=0, latched=0, pmax_ns=0` 支持准备阶段可能耗尽 700µs 的诊断，不补写旧 facts，不关闭原 Clang 失败；D1 **NOT_RUN**。 |
| 新受控时间 fixture 正例 | 实际限定 **2×100 PASS**：Clang 18 和 Release 原母测试各 100 项，同一完整顺序；各自只运行 resolver 一项 CTest。原 700µs、真实 30ms、resolver 90s 及全部原断言保持。 |
| 一次敏感性对照 | 指定单表达式取整对照实际 **97 PASS / 3 FAIL**，CTest exit **8**、无 native timeout；目标原 `pmax_ns<=700000` 断言真实失败，原始 `pmax_ns=1000000, facts=1, latched=1`。证明测试敏感性；私有变异不入 Git，不宣称历史生产缺陷。 |
| F1 正式运行门禁 | **FAIL / OPEN，未豁免**：母测试结束至执行者封存 **133.265366558s > 独立 60s 上限**，超出 **73.265366558s**；总 331.304s 小于 360s 不抵消失败。没有自动重跑授权。 |
| 新候选完整矩阵 | Release 两种 umask、strict Clang、UBSan、GCC ASan/LSan、原 Make-ASan 仍须新输入的实际完整运行及独立审核；旧覆盖或本次母 100 不替代一次 full79。 |

N1：root 额外补证比原 D 晚 **27.116179110s**，原样保留，为非门禁补证；没有外层 root exec，不能伪造 outer await。N2：保留两次依赖 alias 读取失败及限定修正；它们不属于产品失败或编译/产品重试。新独立实际审核未要求源码修正，采用上述有限事实同时保留 F1 正式失败。

原 C39 的 **442** 份 du 记录含 **1 次获准的 TMP 删除竞态重试**，不能概括为全 clean；原 umask 0002 的 **573** 份原始记录为 **572 clean + 1 次同期限内获准重试**。原记录、观察缺口及历史失败均保留。

| 发布与验收状态 | 本页保存时的事实 |
| --- | --- |
| SOURCE_SAVE / PUSH_COMPLETE | 准确三测试与本页待全新独立发布检查；本页不预报提交或推送成功。之后仅可向同一 PR #50 普通提交/推送并核验精确 commit/tree/远端 head。 |
| LOCAL_VALIDATION_COMPLETE / PR_REVIEW_COMPLETE | **false / false**；F1、历史 Clang 失败及适用新输入矩阵仍开放。 |
| FINAL_INDEPENDENT_A_REVIEW | **PENDING**；有限 SOURCE、实际正例和敏感性结果均不构成整 A 通过。 |
| B | **REQUIRED / DEPENDENCY_BLOCKED / NOT_ACTIVATED**。 |
| FORMAL_OWNER_ACCEPTANCE / MERGED | **NOT_CLAIMED / false**；PR 保持 Draft、OPEN。 |
| Windows 11 / 私有证据托管 | **NOT_RUN / BLOCKED**。 |

resolver 原 TIMEOUT **90s**、资源测试原 TIMEOUT **120s** 和全部原断言保持。原始日志、二进制、临时数据库、私有规则与审核原件仅留本地；此页仅保存可公开的状态摘要。

## 2026-11-11 追加：当前候选实际矩阵与未完成项

**Network A 仍为 OPEN / FAIL、NOT_QUALIFIED；B 仍为 REQUIRED / DEPENDENCY_BLOCKED / NOT_ACTIVATED。** 上文保留各历史输入与失败；本节追加随后已封存并独立审核的当前候选结果，没有新增产品运行或重试。候选提交为 `b0ded31910b740aa936d9e420085a1eb15ad2468`，产品源码清单仍为 **685** 项，`candidate-source685.json` 的 SHA-256 为 `84106691f21ae6ea2643f6a0638b6fe16fb238eb89f344cbfd80469eb511e009`。此次报告更新的 **source/test delta 为 0**；生产/构建累计差异仍为 **800**，余量 **0**。

| 当前候选验证项 | 实际执行、独立结论及限制 |
| --- | --- |
| strict Clang 18 / umask 0002 / full79 | **PASS_TEST_ONLY**：原顺序一次执行 **79 PASS / 0 FAIL / 0 TIMEOUT / 0 CTest skip / 0 NOT_RUN**，真实 79 项 JUnit。resolver 原 100 子例全通过；资源内部 **11 PASS / 1 expected Make-only skip**，该内部 skip 不属于 CTest skip。使用同一已审核构建，TEST 阶段 **0 configure/compile/build**。 |
| 当前 Release build | **功能构建身份与完整注册有限 PASS**：**6 条命令 exit 全为 0**，**54 ELF / 2 archive / 79 注册项**，本阶段执行测试 **0**。正式阶段为 **FAIL_LATE_AWAIT，未豁免**；构建功能结果不抵消正式失败。 |
| 当前 Release / umask 0022 与 0002 | 两项均为 **HELD_NOT_RUN**。真实晚收的 build await 不能满足既有入口要求的原期限内 await；未运行这两项测试，未修改入口/guard，未重新编译以抹去失败。 |
| 当前 UBSan / umask 0002 / full79 | **FAIL / OPEN**：注册并执行 **79** 项，**74 PASS / 4 CTest TIMEOUT / 1 CTest FAIL / 0 skip / 0 NOT_RUN**；CTest exit **8**、native exit **1**，真实 JUnit failure 总数 **5**，CTest 报告耗时 **2839.08s**。resolver **100 PASS**；资源内部 **11 PASS / 1 expected Make-only skip**。 |
| GCC ASan/LSan build 与 test；原 Make-ASan integrated | 全部 **HELD_NOT_RUN**，产品调用 **0**。既有拟定外层上限分别为 **3300s / 10230s / 13530s**，不是实际运行时间；没有新阶段时钟或槽位。 |
| F1 及历史失败 | F1 继续 **FORMAL FAIL / OPEN**，原 **133.265366558s > 60s** 不变；上文历史 Clang、Release、观察缺口与失败原样保留，未改绿。 |

UBSan 的 **UB1 OPEN** 对应 `s16_cli_smoke`、`r1_window_cli_smoke`、`r2_rework_cli_smoke`、`r2_amend_cli_smoke` 四个原 CTest 超时；**UB2 OPEN** 对应 `r1_structured_cli_smoke` 内 `pump-fake` 的原 **50s** 子进程超时，CTest 分类是 **FAIL**。两者原因仍为 **UNKNOWN**。已捕获 stdout/stderr/LastTest 中未见匹配的 sanitizer diagnostic，不能据此宣称 sanitizer lane PASS。

Release 原生 session **29383** 的真实 await 晚收已获 root 确认。后置捕获值 **465667.357927393** 大于原 D **462957.292570735**；捕获值不是精确 native await 时刻，其差值不能当作精确迟到秒数或迟到下界。原精确 await 时刻仍为 **UNKNOWN**。

剩余三项受阻于真实最新 Context 产品组的前驱证据：terminal 缺少 `observer_stopped`，原始 supervisor `starttime_ticks` 为 **UNKNOWN**。普通完成检查允许整数 exit code，包括 **1**；阻塞来自上述证据缺失，未把 Context exit 1 改记为 Network 测试失败。随后 Correction 仅 **HELD_PREACTIVATION、产品调用 0**，没有替换 latest；未补造字段、采用旧组替代或增加 adapter。

关键独立审核摘要 SHA-256：Clang `9747961f509027e7ebd23528123391adf790c95e209b5b401d3f703e250ecedf`；Release `cee9222ef96395870df76ece16d47295125197734eb54ea29b7335c6dfcd2b86`；UBSan `802c92bde39fb0de0a296a0f5f5766b7a49043d95f7bb70be7b254dc72e404dc`；剩余 HELD `f883fe095497ae5e366206701f06d0dbb3eee7ee4bb755cd4f026892521131a3`。原作者已采纳正式失败与 HOLD，未授予豁免或整 A 验收。

成文前只读远端核验：[PR #50](https://github.com/wmqfl861/wls861/pull/50) 为 **Draft / OPEN**，head 为上述 `b0ded319…`；该 head 的 [runtime CI](https://github.com/wmqfl861/wls861/actions/runs/42934732405) 中 strict、strict-umask-0002、Clang-18 均 **SUCCESS**，[source snapshot CI](https://github.com/wmqfl861/wls861/actions/runs/42934732342) 亦 **SUCCESS**。这些结果仅属于该旧 head。本追加的 **SOURCE_SAVE / PUSH_COMPLETE 为 PENDING**，须先经全新独立公共保存审核，再普通提交/推送至同一 PR；本追加提交的 CI 尚未运行，不能以旧 head 结果替代新提交验收。

**LOCAL_VALIDATION_COMPLETE=false、PR_REVIEW_COMPLETE=false、MERGED=false**；正式所有者验收未授予，Windows 11 **NOT_RUN**、私有证据托管 **BLOCKED**、真实 WLS/付费调用 **NOT_RUN**。本节仅公开必要事实；原始证据继续仅留本地。
