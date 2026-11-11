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
