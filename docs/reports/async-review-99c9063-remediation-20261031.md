# 99c9063 异步与公平性独立审核整改：实际验证

本批针对 PR #10 精确输入 `99c9063f3e29cd6d69c56fb15815715d499a374a` 的两个已确认 P1 行为缺陷：
旧控制命令精确重放误停当前新调用，以及跨池资源 FIFO 在占据执行槽后等待形成循环。
两项最小生产修复、真实回归、非作者交叉审核和有界诊断已形成独立提交；正式采纳仍交主协调 Agent。
这份报告不撤销原审核的 CHANGES_REQUIRED，不把现有工程通过写成批准或合并。

执行日期：2026-10-31 UTC。产品与限定诊断截至19:13:20 UTC；本报告收尾的Git/文档核对时间见最终验证回执。
实际验证的产品输入为 `976466720e5fb17f6dcc67b0660b74d246996426`，Git tree 为
`30d6e38091c35a4439cb3b2d13075687b9d0877f`。后续交付限定为报告/证据/STATUS/ROADMAP，产品输入保持9764667；
最终文档提交的推送及CI在本报告冻结时均为 **PENDING**，计划外部回执目录为 `E/final-publication-01/`。
提交后由PR11正文和外部回执登记准确SHA、远端读回、CI及源码快照，版本化报告不嵌入自身提交SHA。

## T0：实际接手、环境与依赖

数据盘实测挂载为 `/mnt/d`、`/dev/vdb2`、ext4，接手时约 47 GiB 可用。
本批独立 worktree、构建、TMPDIR、临时 SQLite 与外部证据位于
`/mnt/d/codex-workspace/wls861-review-fixes-20261031-153649/`；测试 UID/GID 为 1000/1000，普通用户 `ubuntu`。
环境为 Ubuntu x86_64、Linux 6.8.0-147-generic、Git 2.43.0、GCC 13.3.0、Clang 18.1.3、Python 3.12.3。
测试 Python 为 `/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python`。
原目录与其他 Agent 未提交工作均保留；没有 reset/clean、强推或覆盖原分支。

原 PR #10 的 base 为 `feat/fair-scheduling-20261031`，head 为 `99c9063f`；本次修复直接依赖该集成输入，
创建独立分支 `fix/async-review-99c9063-20261031` 和 [Draft PR #11](https://github.com/wmqfl861/wls861/pull/11)，
base 为 `work/next-cycle-integration-20261031`。原 PR #1–#10 未由本批合并、重定向或改写堆叠关系。
接手与首次发布核对 main 均为 `a6b8559018a599d7a806b05942857a56d69a00ce`。
2026-10-31 19:06:47 UTC、文档应用前的fetch/readback再次确认main为该SHA、base为99c9063、远端分支为9764667，
见 `E/prepublication-inputs-01/receipt.json`；最终文档发布核对另记，不把此时点事实提前写成最终发布成功。

阅读范围包括 AGENTS、ADR-092/093、A3/A4 实施契约、当前 Scheduler/Async/Store 与 OfflineRuntime 相关实现及测试。
原审核四附件取自 `docs/review-attachments-99c9063f` 精确提交
`ce518883d890d4bef62457aeed2e1b4ff1ec6ec4`；本地最初依据审核描述重建的探针与后来原附件执行分别保留。
原附件 source SHA-256 为 `aec4981953945e2e714564623cb15006bec97d4703ca6d02a4baa3c6595f8f84`。
附件作者的 Debian/GCC14 历史结果没有改标成本机 Ubuntu/GCC13 结果。

本报告证据路径前缀 `E/` 表示上述工作区的 `evidence/`。已冻结的原整改证据为
[文本包](async-review-99c9063-evidence-01.tar.gz)及[成员索引](async-review-99c9063-evidence-01.members.json)，
SHA-256分别为 `c1431a300cc77537ef86e782d8ea91e945aa853e724f2175b11f09470daa6614`、
`9a20a92cb5a23de00589b2803ac6ff80e332609db4fc863974b3535b5bdda88f`。
已冻结的本机矩阵为[文本包](async-review-99c9063-local-matrix-01.tar.gz)及[成员索引](async-review-99c9063-local-matrix-01.members.json)，
SHA-256分别为 `bd0363668d724649233ce14d9a5abd33a74dfca7026c1ee4d2881a9201198240`、
`95c8f21b6122e8b95bf954366f28878c94163bcf2054a8e3bd25b6c38a84ca5d`。
后者 `--check-only`、`--assemble` 均exit0，301个文本成员完整保留三个正式失败轮次；归档完成不改变产品结果。
后续有限探针另存附录，不重造这份已经冻结的本机矩阵包。
另有[补充文本包](async-review-99c9063-supplement-01.tar.gz)及[成员索引](async-review-99c9063-supplement-01.members.json)，
归档exit0，1,041,353字节、220成员（218份原文本加README/MEMBERS），SHA-256分别为
`ca1efe129964f02c50320d7a440a35a30253ab1edbe5ab8dd43194180cee2d2b`、
`ff2a2b41ee643b039d04444bd76ec188bfd2a71bdedfdfe5d8079e952c0bfa82`。
六个原证据目录覆盖有限探针原日志/身份及新FAIL、正式矩阵非作者审核、前置发布核对；前两个包保持原字节。
只读核验证据，不执行归档中的代码。环境、PR 依赖、命令和 worktree 收据保留在 `intake/`。

## T1/T2：最小修复与实际文件

| 范围 | 作者独立提交 | 集成提交 | 主要文件 |
| --- | --- | --- | --- |
| R-A3-01 | `065503e862f3147f3db1ee9e34a129c8091607e4` | `876c4b90f8a8b146b432a7f808d6d149c3b80ca0` | `src/async_coordinator.cpp`、`tests/async_coordinator_tests.cpp`、`docs/implementation/A3-async-coordinator.md` |
| R-A4-01 | `a018d727445def59d7e0f81799793eccc6f23dd4` | `053ab2b2b1010651c3ac0b5d4e7adde02035743c` | 同一生产源及测试、`include/wls/async_coordinator.hpp` 注释、两个资源测试头、窄复现入口、A4 契约及 ADR-093 |
| T4 观察工具 | `951990071c24fb812569d03d8903e643d52f53b9` | `9f7059642dbc9c1c32dd247d35a49c4f804391a5` | `scripts/diagnose_transport_failures.py`、两份原 TLS 测试的显式观察 hook、新观察 helper 及工具自检 |
| T5 原生证据工具 | `6558ed5570080a6e020227f56a4243ee60e428bf` | `88d08499df3c21051720696623a6b8d1c3e7746e` | Windows workflow、`ctest_evidence.py`、`run_windows_ci.py` 和六个工具自检 |
| CI 工具核对 | root 集成提交 | `976466720e5fb17f6dcc67b0660b74d246996426` | `.github/workflows/ci.yml` 增加观察工具自检步骤 |

相对99输入，实际生产差异只有 Async 源 `63` 行新增 / `15` 行删除，以及 Async 头 `2` 行新增注释。
Store 源/头、M1 Scheduler 源/头、SQL、CMakeLists、Makefile、GCC sanitizer runner 与92项名字/期限目录均未变。
`db/`、`third_party/`、`docs/handoff/` 和历史报告保留原字节；19:06 UTC文档应用前的保护路径diff为空，
完整diff及diff-check收据保留在 `E/prepublication-inputs-01/`。最终文档整合后的差异和Git审查另记。
身份及五个集成提交的文件清单见 `E/integrated-implementation-9764667.json`。

R-A3 每次仍调用原 Store 授权、指纹和控制事务。在单构造线程、原独占 Store 写者顺序中，
先读控制前 request state version，再识别控制返回版本是否实际前进；历史重放结果不会高于当前已提交状态。
只有此次真实转换才请求 stop：AMEND 选中原捕获需求修订已过期的调用，cancel 选中原 Dispatcher epoch 已过期的调用。
精确重放没有新的 stop 副作用；不同参数/CAS、旧会话、错误 owner 和终态业务仍由原 Store 拒绝。
当前合法终态经验报告的当前控制 binding 与不可变 accepted source 分开验证，旧源版本本身不等于当前控制已过期。
此判断依赖原单 Store 宿主线程契约，不建立并发写者协议，也不更改公开 Store 结果格式、历史指纹或事件字节。

R-A4 在向任一 M1 pool 入队前确定同资源最早未完成调用，后继保留有界 host 队列、原预约及结果容量，
但不占 M1 排队或执行槽。实际 request/team/class 选择仍由原 `submit_fair` 算法负责。
资源头到原 response Ready、结算、业务应用和反馈持久化后才 Done 并释放资源资格。
没有入池的取消/AMEND/close/停批等待者记原 `host_not_dispatched` Interrupted 观察及未知 usage，不调用模型。
已入池调用仍须观察真实 future；结算/反馈故障继续保留原结果和容量并围栏新业务，证据重试不能再次应用业务。
首次执行仍由原 Actor/session、完整 CAS/epoch 和 `claim_offline_call` 事务授权，资源就绪不授予权限。
没有增加线程、借审核槽、删除 FIFO、刷新身份、sleep 重试或扩大期限。

## 同源目标红绿与历史失败保留

所有本机反例分别链接其输入版本自己的生产头、库及资源，命中目标断言才记红例；编译、夹具错误和超时不代替产品反例。
未修改99的原 Scheduler/Async 已实测2/2 CTest、exit0、10.812852秒，内部为9/9与18/18。
这些原套件通过与新增目标反例失败同时成立；99原绿色 CI 也保持其历史事实。

| 原附件实际轮次 | 生产输入 | exit / 秒 | 原观察 |
| --- | --- | --- | --- |
| original-baseline-amend-01 | 未修改99 | 1 / 0.706225 | 原结果、request、budget 相同，但新调用 stop_requested1；命中56行目标断言。 |
| original-baseline-cycle-01 | 未修改99 | 1 / 1.224247 | cycle1、awaiting2、queued2、executing0、resource_blocked2、两普通槽各占1；命中120行 `!cycle`。 |
| original-amend-amend-01 | 已测R-A3输入，hash对应065503e | 0 / 1.371360 | 同源原反例通过，stop_requested0；执行时HEAD尚为99，不能误报成已提交head。 |
| original-resource-cycle-01 | R-A4提交a018d727 | 0 / 1.837129 | 同源原反例通过，cycle0，四个模型各调用一次并清空。 |

原附件用 synthetic Error 响应观察 stop，不能把它称为四次成功业务提交。
本地另外冻结的合法业务回归则在相同冻结输入下验证真实业务和原证据：

| 本地冻结轮次 | 输入 | exit / 秒 | 结果 |
| --- | --- | --- | --- |
| amend-red-run-01 | 未修改99 | 1 / 0.711347 | queued/inflight当前Dispatcher受旧重放误停，0通过2失败。 |
| frozen-amend-green-run-01 | 065503e | 0 / 0.720005 | 相同两个冻结回归2通过0失败，合法新计划及原attempt/response/feedback一致性断言通过。 |
| resource-red-run-01 | 未修改99 | 1 / 1.218893 | 结构循环、四个新模型调用0，0通过1失败。 |
| frozen-resource-green-run-01 | a018d727 | 0 / 1.244749 | cycle0、new_model_calls4、accepted4、资源FIFO保留，1通过0失败；原claim/envelope/settlement/feedback及效果计数检查通过。 |

四次原附件运行均无外层超时，源/头/库/二进制前后身份不变；本地冻结绿例也核验了原探针输入和二进制不变。
原附件完整 argv、stdout/stderr 和身份见 `E/original-probes-{baseline,amend,resource}/` 及各轮同名目录。
本地冻结原红收据为 `E/frozen-local-repros/baseline-red-receipt.json`，绿收据为
`E/frozen-green-amend/receipt.json`、`E/frozen-green-resource/receipt.json`；逐轮索引为 `E/red-green-index-v1.json`。
早期原红收据中附件尚未获得的 NOT_RUN 保持原件，后来原附件运行另有收据，不回写历史状态。

组件实际运行另计：

| 轮次 | CTest / 内部方法 | exit / 秒 | 说明 |
| --- | --- | --- | --- |
| amend-candidate-suites-01 | Scheduler通过；Async19通过3失败 | 8 / 13.807180 | 三个新typed ID前缀含连字符，夹具先决条件错误；原始失败保留。 |
| amend-candidate-suites-02 | 2/2；Scheduler9/9、Async22/22 | 0 / 13.264118 | 仅改三个非法前缀，生产源/断言/屏障/期限均不变。 |
| resource-candidate-suites-01 | 2/2；Scheduler9/9、Async25/25 | 0 / 22.356944 | 包含非作者要求补入的Handoff与非头AMEND覆盖。 |

三个前缀纠正为 `stale-cas`→`stalecas`、`terminal-amend`→`termamend`、`terminal-cancel`→`termcancel`。
首轮静态复核遗漏此夹具问题，不能把其失败称为新产品缺陷或将首轮结果隐藏为通过。
编译和运行的完整命令、cwd、UID、umask、TMPDIR、结束状态与source-before/after均在各轮目录。

## 新回归、交叉审核及其限制

R-A3 新增四个方法：queued当前Dispatcher重放、inflight当前Dispatcher重放、真实取消与迟到原证据、
当前终态experience的AMEND/cancel精确重放及原source/job CAS。
它们还验证立即/重复/迟到重放、同键异意图、旧CAS、错误owner/session与预算/事件前缀不变；原AMEND方法增强stop与原响应/反馈观察。
终态experience通过真实私有文件、Worker提交、TeamLead检查和Dispatcher验收创建，没有注入假accepted/job行。

R-A4 新增七个方法：2/2真实循环四调用、非头cancel、close的原UNKNOWN观察、foreign destruction的恢复事实、
结算/反馈保留、跨Normal/Review/Handoff同key FIFO与独立兄弟推进、非头AMEND。
同一方法中的结算故障和反馈故障是两个故障点；默认4/8双预留和Ready前驱持有在原方法上增强。
原18加4加7共29个唯一Async方法，仍然一个CTest suite；Scheduler仍9方法，Linux完整注册数仍92，Async期限仍180秒。

R-A3的非作者复核为 `/root/concurrency_fix`，见 `E/cross-review-a3-by-resource/review.md` 和candidate03补记。
R-A4的非作者复核为 `/root/independent_recovery_review`，见 `E/cross-review-a4-by-amend/review-01.md`、`review-02.md`；
先指出host层Handoff及资源非头AMEND两个覆盖缺口，作者补入后限定静态复核未见新增阻断。
`E/integrated-cross-review/` 逐字节证明生产源等于两个修复的组合、29例精确并集、原夹具和期限保留。
该整合核对者同时是R-A3作者，因此这部分只称作者自查与整合核对；R-A3的独立性来自另一位实际非作者复核。

这些是同模型Agent之间的非作者工程复核，不是第二模型评测、全仓无缺陷证明或主协调者批准。
复核者没有把root的实际执行改称自己的测试；组件绿例不能自动替代combined或最终SHA结果。
图索引缺少新Async路径，结构判断以实际精确源/差异fallback完成，没有使用空图证明不存在问题。

另有非作者 `/root/evidence_audit` 的独立覆盖审核 `E/final-coverage-audit-01/`：18条要求—方法—执行映射有重叠，
不计为18个独立测试或套件；实际仍为Async29方法和Scheduler9方法。原18个Async方法中15个函数体逐字相同、3个增强，
其中一处非空反馈CHECK被读取并比较原response的更强断言替代，不能声称所有原CHECK文本不变。
逐项证据见该目录 `requirement-method-execution-map.json`、`original-case-preservation.json`、
`three-original-case-differences.patch` 及完成收据。审核明确未单独执行“adapter本身抛异常，同时有同资源跨池阻塞后继”组合，
亦非任意线程交错的证明；该限制与同模型工程复核、主协调者正式接受分开保留。

## T3：产品输入、实际本机矩阵与远端矩阵

本机正式产品输入为9764667，共785项完整源码/文档输入；构建配置相互独立，使用原警告、期限和断言。
各轮记录 source-before/after、二进制身份、command/completion、原始stdout/stderr、JUnit及完整LastTest。
设置ASAN_OPTIONS本身不表示Release/Clang二进制启用ASan。
全部正式矩阵均已结束，只读汇总为 `E/local-final-matrix.json`，SHA-256为
`78d96be5bb7eb4484557ff8b8e400692b79acd43dbdfb41c5293dcbb51bce2b8`。
该工具未运行产品；`evidence_consistency_check` 为COMPLETE，`observed_selected_result` 为FAIL_OR_INCOMPLETE，
证据核对完成与产品门禁通过分别保留。
非作者最终矩阵审核为 `E/local-matrix-final-audit-01/review.md`，SHA-256为
`5b83771d296f087f90f4da1e933fc9ff5efef996a1589ad83655ed77c780469d`。

| 本机配置 | 实际范围与结果 | exit / 秒 | 证据 |
| --- | --- | --- | --- |
| GCC13 Release configure/build | 完整默认构建成功 | 0 / 2.744953、1321.045431 | `E/final-release-01-{configure,build}/` |
| GCC13 Release、umask0022 | 92/92 PASS；0失败、0超时、0skip/not-run | 0 / 2479.036900 | `E/final-release-01-ctest-0022/`；`E/final-release-01/junit-0022.xml`及原完整LastTest。 |
| GCC13 Release、umask0002 | 91 PASS、1 owned_capacity Timeout（90.74秒）；0其他失败、0skip/not-run | 8 / 2498.111225 | `E/final-release-01-ctest-0002/`；`E/final-release-01/junit-0002.xml`及原完整LastTest。 |
| Clang18 Release、umask0002 | 受影响2/2 CTest；Async29/29、Scheduler9/9；本机其余90项NOT_RUN | 0 / 16.304404 | `E/final-clang-01/result.json`、`ctest-0002-original-logs/LastTest.log`；源码/二进制前后相同。 |
| 本机UBSan、umask0002 | Scheduler9/9方法PASS（0.07秒）；Async原180秒期限Timeout（180.18秒）、方法输出0字节；范围受影响2/92，其余90本机NOT_RUN | 8 / 181.464094 | `E/final-ubsan-01/result.json`、`junit-0002.xml`、`ctest-0002-original-logs/LastTest.log`及对应recorder；7构建身份、785源码前后相同。 |
| 本机GCC ASan/LSan、umask0002 | Scheduler9/9方法PASS（0.17秒）；Async原180秒期限Timeout（180.59秒）、方法输出0字节；受影响2/92，其余90本机NOT_RUN | 8 / 182.485592 | `E/final-asan-affected-01/result.json`、`junit-0002.xml`、`ctest-0002-original-logs/LastTest.log`及对应recorder；7构建身份、785源码前后相同。 |
| 本机原Make-ASan | 本轮NOT_RUN，完整执行证据来自远端；不以两个受影响CTest代替Make全链 | — | 远端Make原始日志另核。 |

两轮Release均已真实结束，61项构建身份及785项源码输入两轮前后和跨轮一致，完整92项均无skip/not-run。
原command/completion、两份JUnit及完整LastTest保留；两轮外层采集均未超时，0002是套件内owned_capacity超时。
两份JUnit按原注册名保留store、r24_recovery、workflow_runtime、r24_concurrency_settlement、cleanup_runtime、
billing_baseline_e2e、single_agent_runtime等相关结果；除0002的owned_capacity外其余注册通过，不另计一套完整门禁。
本轮本机Clang和sanitizer的范围限定为受影响2/92，CI完整92项另列；其余90本机NOT_RUN。
正式矩阵的三个未通过项是Release0002 owned_capacity、UBSan Async及ASan/LSan Async，均为原期限Timeout；
整体 `LOCAL_VALIDATION_COMPLETE=FALSE`，历史问题保持开放。

Release超时的只读归类见 `E/local-release-0002-timeout-triage-01/` 的报告、failure-receipt及完成收据。
其捕获时全套尚未结束的原记录保持；最终完整结果由上表及原JUnit补充，不回写历史。
同一Release二进制的owned_capacity在0022为65.9196秒通过、0002在原90秒期限超时；该差异的根因仍 **UNRESOLVED**。
当前UBSan Async和ASan/LSan Async分别在原180秒期限超时且均无方法输出，根因仍 **UNRESOLVED**，不能定位首个未完成方法。
ASan/LSan原 `detect_leaks=1` 保持，超时不能证明完整ASan/LSan检查通过。
现有记录不支持性能、外层umask或与历史失败同因的归因，后续诊断不能把这些原失败改绿。

限定sanitizer冻结探针仅执行预定4次二进制，各一次、不重试，实际为3 PASS、1 FAIL；内部方法共5 PASS、1 FAIL。
它们与上述三个正式Timeout分别记录，仅记 `DIAGNOSTIC_NOT_FULL_GATE`，不替代完整门禁或关闭根因。

| 冻结探针 | compile exit / 秒 | run exit / 秒 | 实际结果 |
| --- | --- | --- | --- |
| UBSan AMEND | 0 / 65.470818 | 0 / 5.790741 | 2方法PASS。 |
| UBSan resource | 0 / 65.782118 | 0 / 9.705964 | 1方法PASS，4次真实调用、accepted4、FIFO保留。 |
| ASan AMEND | 0 / 53.718590 | 0 / 15.798956 | 2方法PASS。 |
| ASan resource | 0 / 58.916776 | 1 / 27.799066 | 0 PASS、1 FAIL；outer_timeout=false，未触发180秒外层watchdog。 |

ASan resource在冻结 `resource-regression-suite.cpp:136` 的共享until助手原5秒deadline断言
`std::chrono::steady_clock::now() < deadline` 失败；没有CROSS_POOL_FIFO成功或结构cycle输出，原输出未出现ASan/LSan报告。
共享助手多次被调用，现有输出未标识具体失败调用点。该次结果不能证明两项P1在此ASan配置通过，根因仍 **UNRESOLVED**；
编译或取证工具exit0不代表探针通过，实际run和该轮recorder均退出1。
原命令、冻结输入/库/二进制身份及结束收据见 `E/sanitizer-targeted-diagnostic-execution-01/` 的FINAL和execution-receipt。
`FINAL.json` SHA-256为 `b6694649675f452fe84a42fea5da5db1d186630148eb96aa1a6adc43c82ececa`，
`execution-receipt.json` SHA-256为 `7c3051b900bce94b5bb4ba67b9f5faf28d63e93d9841d11dc67fe788a7b6cfa6`。
该诊断由A4作者执行，非A4独立审核；原785源码、5个冻结输入、库及各binary前后身份相同，进程组均退出。

工具自检是独立合成工具结果：

| 工具 | 方法数 / 结果 | exit / 收集秒数 | 证据 |
| --- | --- | --- | --- |
| GCC gate runner | 24/24 | 0 / 1.201186 | `E/final-tool-runner-01/` |
| 即时unittest失败取证 | 4/4 | 0 / 0.787700 | `E/final-tool-immediate-01/` |
| opt-in传输观察 | 11/11 | 0 / 0.724012 | `E/final-tool-transport-observer-01/` |
| Windows CTest证据采集 | 6/6 | 0 / 3.251558 | `E/final-tool-windows-evidence-01/` |

截至现有 `E/remote-9764667-01/metadata/20261031T172429Z/summary.json`，9764667共17/17作业成功。
主任务冻结远端收据为 `E/remote-9764667-01/FINAL.md` / `FINAL.json`，后者SHA-256为
`be84af2d9f4168708ec72b99136eb16612e775e5f937a617e76b2c5d4f0ab14c`。7个工作流、17个job均首轮成功，未重跑CI。

| 9764667远端工作流 | Push / PR | 实际范围 |
| --- | --- | --- |
| Linux六路 | [41220729821](https://github.com/wmqfl861/wls861/actions/runs/41220729821) / [41220751664](https://github.com/wmqfl861/wls861/actions/runs/41220751664) | strict、strict-umask0002、Clang18、UBSan、GCC ASan/LSan完整92/92，原Make-ASan整链；Push/PR为相同六配置的两次运行。 |
| 同源公平性红绿 | [41220729844](https://github.com/wmqfl861/wls861/actions/runs/41220729844) / [41220751625](https://github.com/wmqfl861/wls861/actions/runs/41220751625) | 原公平算法诊断，旧A3红/当前A4绿；与R-A4-01跨池死锁反例分开。 |
| Windows原生组件 | [41220729868](https://github.com/wmqfl861/wls861/actions/runs/41220729868) / [41220751577](https://github.com/wmqfl861/wls861/actions/runs/41220751577) | 每轮4注册、27方法；完整LastTest已取证，仍为Server2022提升令牌组件范围。 |
| 可核验源码快照 | [41220729814](https://github.com/wmqfl861/wls861/actions/runs/41220729814) / — | 785项，重建tree等于9764667提交树。 |

PR API head为9764667，PR工作流实际checkout为合成merge `cbc4107e0c6984e0acc3d8759ddeff87247b75b9`；
Push checkout为9764667本身，不混写两种身份。ASan Push/PR artifact均为92个JUnit注册通过，
785源输入与61项构建身份前后相同，UID1001、umask0002、`detect_leaks=1`，无LSAN suppression。
成功JUnit可能截断内部方法输出，所以只报告已核实的注册数，不臆测完整内部方法总数。
Make每事件按原Makefile的92条调用逐条核对顺序：57个native调用通过801方法，32个unittest调用通过397方法，
另有demo_smoke的3场景和2项完整性脚本。1201个显式通过项不计为1201套。13个unittest方法只有7/6个通过点号及完整Ran/OK汇总，
未臆造逐名输出。实际分类和限制见上述FINAL；首次解析器未识别quiet点号而exit1及随后分类纠正均保留，产品CI未重跑。

9764667源码artifact13049931580，ZIP SHA-256 `28e86a20c195fa7f2ae783b5bcb5d85f015b567402eacff280fdb401e2d19ed2`；
内部tar.gz SHA-256 `5e9fa1c328b00b299b772390bcd7ef2d5a82be10cdb405a269d88bafc5a3c1e9`，785文件的Git树重建与本地一致。
最终文档交付的推送、CI及全快照仍为 **PENDING**，计划写入 `E/final-publication-01/`；
该位置是预留路径，准确SHA与结果由提交后PR11正文和外部回执登记，本段不继承前一SHA结论。
完整原始远端资产57,519,077字节（约54.9MiB）保留在数据盘 `E/remote-9764667-01/`；入库仅纳入审核后的FINAL/冻结清单与必要摘要，
不复制整组产物。原GitHub artifact及7个ZIP digest核对位置由FINAL列出，12份独立job日志与workflow ZIP对应日志逐字节相同。

## T4：有界原GCC诊断与仍开放的历史问题

历史输入772feaee本机GCC13.3 Debug `-g` ASan/LSan仍为48通过、2失败、39超时；
历史Clang18 Debug `-O2 -g`为29/39、10超时；旧live_workflow根因仍UNRESOLVED。
`historical_clang_asan_failures_closed=false` 保持；两个后继Async缺陷不能解释此前失败。

观察工具默认关闭，仅显式opt-in；0600独占创建有界JSONL，仅记录阶段、单调时间、数值/布尔及固定枚举，
不输出原始秘密、请求/响应正文、标识符、任意异常文本或路径。普通测试不创建日志、不额外parse。
AST对照移除显式观察hook后，两份原测试完整行为与历史源码一致；原断言、3500ms、15/18秒、12/16秒及注册180秒期限均保留。

非作者 `/root/concurrency_fix` 发现首个工具候选的I/O异常会逃逸、妨碍Peer.close或替换原网络异常。
新增同源工具反例实际为8通过/2ERROR、exit1；修后11/11工具自检通过，还独立验证真实合成进程组timeout收尾。
这些是新工具缺陷红绿，不冒称历史产品失败复现；prepared-v1保持NOT_RUN，原错误和准备日志全部保留。
最终非作者审核见 `E/cross-review-t4-by-resource/review-candidate-02.md`。

三项原GCC方法各执行一次，来自两个原suite，均是 `DIAGNOSTIC_NOT_GATE`：

| 选择方法 | collector exit / 秒 | 本轮实际观察 |
| --- | --- | --- |
| partial request upload | 0 / 5.894790 | prefix4096，supplied/reported upload65536/901181，request_write deadline；accepted socket1、完整request0。 |
| effective live capacity | 0 / 13.405899 | accepted0、INTERRUPTED、deadline_exceeded；原3MiB有界raw前缀、settlement、ledger一致及unknown_cost100；没有新重发权限。 |
| large escaped valid output | 0 / 6.514095 | accepted1、COMPLETED，1048508原body字节、174662字符；原settlement及ledger检查通过，未知成本仍100、没有重发权限。 |

使用保留的772feaee原GCC二进制，没有重编产品；714项原source身份匹配，715项私有overlay只有两份测试观察hook和新helper差异。
每项前后source/overlay/binary/Python/runner身份一致；UID1000、umask0002、原ASAN/UBSAN选项、无抑制。
运行窗口无其他重编译/产品gate，三leader自然reap、peer live_threads0、无残留相关worker/driver。
收据为 `E/t4-transport-observation/completion-receipt.json`，其SHA-256为
`2fe7c749e6205422da89965674545419407ccd65948dde6f072d343b3b98f4cb`；冻结清单SHA-256为
`03c624f61b1367cb36fa47f0206de6fc6c91dbbe9aa366c87cc27dc13b5adcb6`。

单方法成功未重现原完整suite前驱、负载与失败条件，不证明历史根因关闭。
effective本次传输elapsed8312076us且父进程在原18秒内返回，不能据此推断历史18秒异常发生于哪个未观察内部阶段。
旧prefix0不证明零上传、不授予重试；旧没有返回的状态/证据也不能用本次成功记录补造。
旧live_workflow没有因这三项诊断被运行或关闭，保留待主协调者另派有界问题卡。

## T5：原生完整日志与平台边界

Windows证据工具在成功、失败和timeout时保留完整LastTest或临时日志、failed索引及逐方法事实，
拒绝从四个green注册推断缺失、重复或skipped方法已通过。baseline六个合成工具自检为3失败/3ERROR、exit1；
修后6/6、exit0。原CTest命令、30/30/60/30秒产品期限和300秒采集外界限均未改。
完整工具红绿和非作者审核见 `E/native-log-review/completion-receipt.json`。

9764667本轮远端Push/PR已分别取回完整LastTest、4注册下3+4+14+6=27个方法，
源785项前后相同。Push原LastTest SHA-256为 `14560bafe7812c4be61b44e7e7ec7e77d569a7537f1d109c584151e5683f7180`；
PR合成merge为 `f4fb6b143ac4ed95803f6c2295aa5180def060e21f5b09306cb87606841e10f8`。
artifact只保留单份binary hash inventory，不能改称可执行字节或binary前后双清单；源码前后身份与方法事实另有实际记录。

实际环境为Windows Server2022、NTFS、elevated token组件执行；
`WINDOWS_11_ORDINARY_USER_ACCEPTANCE=NOT_RUN`、普通用户安全验收NOT_RUN、完整Windows runtime NOT_IMPLEMENTED。
本卡没有新增Windows Store/VFS、任意run、强隔离、进程退出证明或工作区清理支持。

## 最终身份、状态与交还事项

正式产品运行输入清单为 `E/prepublication-inputs-01/validated-9764667-inputs.json`，SHA-256为
`bbc595cd91d5f7c8439cb50d6e184caee26a308e265c8a1e10948bb061d17bf2`。
2026-10-31 19:06:47 UTC、文档应用前已核对785项文件字节/权限与Release61、Clang/UBSan/ASan各7项构建身份不变，
对应 `E/prepublication-inputs-01/receipt.json` 的SHA-256为 `b7a5de69e2ec7f5445a091202b8da47c8ba683f9143bb03140d51638a7d18c55`。
最终文档交付输入与相对9764667的精确差异见 `E/publication-final-check-01/validation.json`：相对9764667只增加本报告和三份文本证据包/索引，并补充STATUS、ROADMAP；产品源码、测试和构建输入未再修改。
探针后的源码/库/各binary身份及进程结束见上述FINAL与execution-receipt：4次编译、4次运行均结束，各进程组退出，
source-after于19:13:20 UTC完成，最终观察无相关活动进程。该有限执行事实不把任何失败改写为通过。
所有未成功或中断轮次保留原状态；证据包检查不接纳密钥、真实环境文件、数据库或构建产物。
最终 `git diff --check`、status、完整diff、保护路径及远端分支读回记录见 `E/publication-final-check-01/validation.json` 及同目录原始diff/status/stat/fetch记录；提交后的远端读回另见 `E/final-publication-01/`。

| 状态 | 本批准确范围 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | 两项最小生产修复、回归与T4/T5工具已实现、分别提交并集成；正式采纳不由实现者代签。 |
| LOCAL_VALIDATION_COMPLETE | **FALSE**；Release0002 owned_capacity、UBSan Async、ASan/LSan Async三个原期限Timeout及单列ASan资源探针断言失败保留；完整本机Clang/sanitizer其余90项及历史根因仍开放。 |
| PUSH_COMPLETE | 9764667已正常推送且Draft PR11存在；最终文档交付在本报告冻结时 **PENDING**，计划外部收据目录 `E/final-publication-01/`。 |
| REMOTE_CI_COMPLETE | 9764667已有17/17成功及精确artifact核验；最终文档交付在本报告冻结时 **PENDING**，由提交后PR11正文和外部回执单列。 |
| PR_REVIEW_COMPLETE | **PENDING**；原A3/A4 CHANGES_REQUIRED未由执行Agent或同模型交叉审核覆盖。 |
| MERGED | **FALSE**；没有合并main或PR1–11，没有启用自动合并。 |

本批只做离线FakeModel、自建临时SQLite/文件、loopback TLS与合成工具夹具，真实供应商调用、付费请求和生产操作均为0。
没有启动B0–B6生产实现，没有新增并发写者租约、自动inbox pump或真实live执行入口。
交回主协调Agent复核两项整改及证据；三个正式Timeout、ASan资源探针共享until新失败、历史运行时、
Windows11普通用户、正式采纳和后继任务仍按独立问题卡决定。
