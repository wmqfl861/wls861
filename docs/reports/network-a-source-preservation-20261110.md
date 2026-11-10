# Network A：已审源码保存与验证状态（2026-11-10）

**整体状态：OPEN / FAIL。** 本次保存 FIFO 重开及 closed-stdio 两项修复和既有测试，供现有 [Draft PR #50](https://github.com/wmqfl861/wls861/pull/50) 审阅。Network A 的完整适用矩阵与最终独立资格审核尚未完成，历史失败继续保留。Store 版本为 **410**。

本次提交前，完整 685 文件清单与冻结快照、已独立审阅的源码逐字节一致。相对 PR/main 基线 `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631`，全部生产/构建改动累计 **+790 / −10 = 800**，包含三个新增生产文件；预算余量为 0。普通提交的实际父提交为 `3f7209d60c2e7621cbbdc438543759c9e1117cee`。

| 验证项 | 实际结果与范围 |
| --- | --- |
| 两缺陷 RED/GREEN | 独立审核 **scoped PASS**。修复后原 resolver 母测试 **100/100 PASS**，原 81 项及新增 19 项顺序完整；只执行了 1 项 CTest，注册集合仍为原 79 项。 |
| INSTALL / 普通 Make | 独立审核 **scoped PASS**。真实 Make 构建、resolver **100/100 PASS**、资源安装测试 **12/12 PASS，无 skip**；包含真实 Make 安装与迁移后生产 helper 检查。此直接 Make 运行不具有完整 CTest/JUnit 运行身份。 |
| 原 Release / umask 0022 | 原 79 项尝试中 **40 PASS、第 41 项中断、38 项未开始**；实际进程 exit **1**、严格资源观察失败、实际 await 晚约 **48 秒**、原 79 项 JUnit **缺失**。这些原始失败和缺项继续保留。 |
| 原 Release 的 C39 续跑 | 使用同一冻结物理源码/ELF，原第 41..79 项 **39/39 CTest PASS**；真实 39 项 JUnit 无失败或跳过。resolver **100/100 PASS**；资源测试为 **11 PASS / 1 项 Make 专用内部 skip**，该 skip 不计 PASS。实际工具 session **34967** await 完成、exit **0**，运行与收尾均在原 deadline 内完成。 |
| Release 两次运行覆盖复核 | 最新独立限定审核 **PASS**；已登记 `0022: COMPLETE_COVERAGE_79_ACROSS_TWO_ATTEMPTS`，仅支持同一 ELF 复用。旧 40 项与新 39 项各有明确身份，合计 **80 次已开始调用**（包含旧第 41 项中断）；没有一次连续 79 PASS，也没有合成 79 项 JUnit。 |
| 原 Clang 18 | **51 PASS / 1 真实 FAIL / 第 53 项中断 / 26 项未开始**。resolver 原母测试为 **99 PASS / 1 FAIL**，失败项 `lifecycle_submillisecond_budget_is_exact` 的 `facts>=1` 未满足；历史原始 facts、RESULT、绝对 deadline 和 Error 仍为 **UNKNOWN**。 |
| 新 O1 观察 | **仅诊断**，独立审核 `PASS — LIMITED_DIAGNOSTIC_ONLY`。原 ELF 的一次新观察为 `outer_us=1293 facts=0 latched=0 pmax_ns=0 pzero=0 threw=0 started=1 ok=0 pending=1 elapsed_us=951`。它支持准备阶段可能耗尽 700µs 的测试前提诊断，不补写旧 facts，不关闭原 Clang 失败；D1 **NOT_RUN**。 |
| 新受控时间 fixture | **NOT_RUN / 尚未实现于本快照**。本次保存保留原三个测试文件字节，不包含后续测试修正。 |
| Release umask 0002 | **NOT_RUN**，仍需单独准入审核与实际运行授权。 |
| UBSan、GCC ASan/LSan、Make-ASan | **NOT_RUN**；尚未启动的新矩阵不能由旧记录、限定 PASS 或远端 CI 代替。 |

C39 原始资源记录中保留 **442** 份 du 记录，其中有 **1 次获准的 TMP 消失/删除竞态重试**；不能把所有原始记录概括为全部 clean。实际终态记录表明收尾完成、无剩余归属进程或 ownership gap；未观测区间没有补造成持续监测。

| 发布与验收状态 | 本页提交时的事实 |
| --- | --- |
| SOURCE_SAVE | 已核验既有已审源码，准备向同一 PR #50 正常提交/推送；不创建额外 PR。 |
| PUSH_COMPLETE | **提交时待远端核验**。推送完成后由精确 commit / tree / 远端 PR head 核验收据记录；本页不预报未来推送成功。 |
| LOCAL_VALIDATION_COMPLETE | **false**；原 Clang 失败与完整适用矩阵仍开放。 |
| FINAL_INDEPENDENT_A_REVIEW | **PENDING**；源码、RG、INSTALL 和 Release 两次覆盖的限定 PASS 均不构成整个 A 通过。 |
| B | **REQUIRED / NOT_ACTIVATED**。 |
| FORMAL_OWNER_ACCEPTANCE / MERGED | **NOT_CLAIMED / false**；PR 保持 Draft、OPEN。 |
| Windows 11 / 私有证据托管 | **NOT_RUN / BLOCKED**。 |

resolver 原 TIMEOUT **90s**、资源测试原 TIMEOUT **120s** 和全部原断言保持。原始日志、二进制、临时数据库、私有规则与审核原件仅留本地；此页仅保存可公开的状态摘要。
