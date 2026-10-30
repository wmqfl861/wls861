# R2.4 交付证据独立复核（2026-10-30）

**结论：R2.4 已发布交付的证据复核通过，没有发现阻断正式 PR 审核的证据缺口。**
本轮重新读取原日志、核对命令和终态、重算源码与二进制身份，直接查询 GitHub 并下载原日志和源码 artifact；没有仅复述上次交付回复。
审计对象是 `55dc989e02f7e3e2cd1806e53d47298dd8742376`，原基线是
`a6b8559018a599d7a806b05942857a56d69a00ce`，提交树是
`31f45957a6aeb57bdab0704b89f3d7fdadbae6b3`。

主执行者已正常创建 [正式 PR #1](https://github.com/wmqfl861/wls861/pull/1)，
head 为 `feat/r2-4-demand-driven-recovery`，base 为 `main`。
本审计实时读取确认该 PR 为 open、非 draft、未合并，当次没有 APPROVED review。
**`PR_REVIEW_COMPLETE=pending`，`MERGED=false`。** 同模型体系的独立工程审计不代替主协调 Agent 的正式批准。
本轮没有编译或重新运行 R2.4 实现测试，没有执行任何归档代码、修改既有证据或移动 Git 引用。

## 复核方法和实际阅读范围

任务目录为 `/mnt/d/codex-workspace/wls861-blueprint-20261030`，旧原始执行现场为
`/mnt/d/codex-workspace/wls861-r2-4-20261029`，均在数据盘。
本轮审计使用普通 ubuntu UID/GID 1000、Git 2.43.0、GitHub CLI 2.101.0。
机器证据为新任务目录 `evidence/r24-evidence-audit.json`，其 SHA-256：

`0dfded39e19e3d8bb71073babfaffe99b5d69c285c3d606a5e1883a392c83717`

阅读分为明确的不同方法：

- **全文语义阅读：**完整阅读交付提交中的全部 40 份 `docs/reports/*.md`，合计 447962 字节。`evidence_audit` 阅读 24 份，`independent_recovery_review` 在本轮阅读其余 16 份并返回逐文件结论和哈希；本审计再次复算这 16 份身份。使用完整 `cat` 或覆盖全行的有界 `sed`，输出截断处补读。文末列出全部路径和阅读者，完整逐文件结论与 SHA-256 在机器记录中。
- **相关审核：**完整阅读旧 R2.4 工程复核及本轮[十一项源码独立复核](../reviews/r2-4-independent-audit-20261030.md)；相关历史 review 的 7 Markdown、2 C++ 反例和 1 patch 由独立源码审查者在本轮全文阅读，逐项身份见 `evidence/r24-code-review.json`。没有将这些静态阅读当作新运行测试。
- **机器解析：**递归解析 42 份仓库报告 JSON、221 份旧现场 JSON/JSONL、407 份归档 JSON；枚举 52 份 SHA 清单的完整条目、路径、重复和格式。三种位置包含同一记录的重复副本，这些数目不是测试套件数。
- **原日志：**对 666 份日志或文本完整读取字节、计算哈希并扫描原终态/FAIL/Timeout 行；另单独审阅 R2.4 红绿原日志和五门禁、远端 CI 的实际终态。这里不宣称逐行语义审核每份日志或每段归档源码。
- **归档与提交：**用 `tarfile`/`zipfile` 只读成员和原字节，核验内部 INDEX 或外部成员目录。没有解压运行、导入或调用其中的脚本。只读 `git archive` 的内存 tar 与下载源码 tar 逐字节比较，Git blob/模式用于保护历史文件。

全部 JSON/JSONL 解析和清单格式检查通过，已记录的同名原日志哈希均相符。
首个全目录检查脚本因未正确处理 `git ls-tree` 的 UTF-8 引号路径 exit 1；改用 NUL 分隔读取后完整复算 exit 0。
这是审计采集脚本的问题，未执行产品测试，也没有改写任何产品失败。
`remote-make-progress-log-probe.json` 的历史 exit 1 是当时 Make job 仍在运行时读取日志的失败探针；它使用 `at` 时间字段，不是未完成的测试门禁。

## 原版反例、绿例与测试计数

原基线构建使用独立 pristine worktree。核心/清理回归编译 argv 指向基线头文件和 tests，链接基线原 `libwls_s1.a`、`libwls_kernel.a`；CLI 使用基线 `build-release/wls`。
本轮重新核验五项基线二进制/库哈希及回归源身份，均与捕获记录相符；源、库和最终实现 ABI 没有混用。

| 逻辑范围 | 红侧实际来源与结果 | 绿侧记录 |
| --- | --- | --- |
| R2.4 核心 17 子例 | pristine `a6b8559…`；6 通过、11 失败，exit 1，29.615253 秒 | 早期同源专项 17/17、exit 0；最终 v3 门禁再次覆盖 |
| R2.4 清理 6 子例 | pristine；2 通过、4 失败，exit 1，16.815238 秒 | 早期同源专项 6/6、exit 0；最终 v3 再覆盖 |
| R2.4 CLI 3 子例 | pristine CLI；1 通过、2 失败，exit 1，6.382201 秒 | 早期同源专项 3/3、exit 0；最终 v3 再覆盖 |
| WorkerTools late-prepare 新子例 | pristine Store/头文件/资源，加原 WorkerTools 的私有 before-prepare 调度观察点；18/19、exit 1 | 修复候选同一测试源 19/19、exit 0 |
| WorkerTools 结算会话新子例 | **feature v2** Store/头文件及 v2 WorkerTools；19/20、exit 1，绝非 pristine 基线红例 | 会话键候选同一测试源 20/20、exit 0 |

前三行的最终原日志为 `baseline-r24-{red,cleanup-red,cli-red}-private-tmpfs.json/.log`，
直接记录空闲 epoch 1→2、完整数据库行变化、旧 Actor 未拒绝和 CLI 的 2≠1、3≠2。
必要恢复、显式 init、见证篡改、回滚等基线原本通过的用例属于保留性回归，不被算作缺陷反例。
早期缺 binding 的编译错误、FTS/WITHOUT ROWID 查询夹具错误、15 子例旧轮次分别保留，未用来证明产品缺陷。

三份新增 R2.4 测试是 **3 个逻辑套件、26 子例（17+6+3）**；原 WorkerTools 套件另增 **2 个子例（18→20）**。
同一结算子例的两个核对顺序、多个参数或多种工具链不增加独立套件数，最终项目为 **81 个 CTest 注册项**。
late-prepare 红侧带测试观察点，不能称完全未插桩的原二进制；结算红侧是 v2，不改称 pristine。
相应来源和相同测试源哈希分别重新核对 `concurrency-counterexamples.json` 与 `settlement-counterexamples.json`。

## 同一 v3 的本地有效五门禁

312 项冻结运行输入的清单 SHA-256 为：

`59c8310d64a961d2ebac1159b93e8aa925499be61795b06831a96d79e2fa2050`

本轮重算旧执行 feature、新文档 worktree 的这 312 项，全部匹配原清单与提交树。
文档和 AGENTS 不属于此运行输入集合，不能以该清单代替完整 Git tree。
Release、严格 Clang、ASan、UBSan 四个现存构建目录各自的 **57 项**可执行文件/静态库/构建文件，均重新枚举并逐项重算，与原运行前后记录精确一致，没有多项或缺项。
ASan 的 57 项还同时等于原失败、单项探针和完整重验记录；不是通过换二进制取得后续绿灯。

| 有效门禁 | 实际已执行结果 | exit | runner 秒数 | 原记录 |
| --- | --- | ---: | ---: | --- |
| Release / umask 0022 | 完整 81/81 | 0 | 1180.883560 | `feature-release-0022-session-final` |
| Release / umask 0002 | 完整 81/81 | 0 | 957.807708 | `feature-release-0002-session-final` |
| Clang 18 严格 Release | 完整 81/81 | 0 | 1174.086152 | `feature-clang-session-final` |
| UBSan | 相关选择 31/31 | 0 | 1156.904475 | `feature-ubsan-session-final` |
| ASan/LSan 同源完整相关重验 | 相关选择 31/31 | 0 | 1243.067519 | `feature-asan-v3-resumption-full` |

逐份核对原 argv、cwd、UID/GID 1000、umask、零 capability、实际结束时间、exit、全部注册名和 `LastTest` 终态。
最终计划的原 timeout 映射与当前原生成配置一致，五组分别恰有 81/81/81/31/31 个 Passed，无 Failed 终态。
四次默认目标增量构建均 exit 0，各有 51 个实际 Ninja 动作；这不是四次清洁构建。
本地 sanitizer 为 Clang 18 Debug、显式 `-O2 -g`、无 NDEBUG，保留 Werror、完整插桩和 frame pointer；
ASan 保持 `halt_on_error=1:detect_leaks=1`，UBSan 保持 `halt_on_error=1:print_stacktrace=1`。

31 项包含请求恢复、Store、工作流、进程结算、清理、计费、单 Agent、完整 `billing_baseline_e2e` 和历史解释等相关注册项；**本地没有宣称 81 项 ASan/UBSan 全量执行**。
更广的远端全量结果在下一节单独登记。原夹具内部的私有 umask 不由外层 0022/0002 推导。

## 历史失败与四份 R2.4 证据包

历史 `r2-4-results.json` 原样保持 `local_validation_complete=false`，SHA-256
`78c718189cf951ac6a61352ee8390aa46d17502b91196a6377910abb1f7fb2fc`。
其 ASan 原记录真实为 **19/31、12 Timeout、exit 8**，另有三个内部 unittest FAIL，不能只说外层超时而掩盖内层失败。
后续 `r2-4-resumption-results.json` 是独立追加的有效五门禁快照，真实为 `local_validation_complete=true`，SHA-256
`9f809d100fb7928b86223b123b0b9d26dd8c06d4f61838bdebddc6e32c1b62ae`。
两份状态对应不同执行时点，不能回写旧状态。后续同二进制成功不证明历史超时只有环境原因。

| 归档 | 成员数 | 压缩字节 | SHA-256 |
| --- | ---: | ---: | --- |
| `r2-4-evidence.tar.gz` | 339 | 4567874 | `45ac14ee3558dfb8f18054f4d543d0598297c7032a012e457943ed806bea4f6c` |
| `r2-4-asan-profile-evidence.tar.gz` | 17 | 612802 | `35c838fe112aa648b05f44f6f192fc0b3097fd39b7d356191dd1600898f710b8` |
| `r2-4-serializer-probe-evidence.tar.gz` | 49 | 455104 | `92bedd5a7547f3792f7b1d3eb976c93f8a3a9ceab9b5a905ea862e7e9dd2b0da` |
| `r2-4-resumption-evidence.tar.gz` | 49 | 637156 | `8bee0bedbe1f730c494338dd302265e0e753630328a7661fa64c8681bc52e43f` |

本轮核验四包全部 INDEX、成员集合、大小、SHA 和映射后的原现场字节，全部一致。
复现脚本只作文本读取。性能样本保持其 Timeout/exit 8，serializer 双侧原期限测试都失败；
外部性能候选保持 NOT_ADOPTED，未运行 oracle/运行测试保持 NOT_RUN，编译成功不计为验收。
后续 resumption 选择说明写在开跑后，不改称预注册计划。

整个历史目录共 **11 个归档、998 个成员**，均与内部 INDEX 或外部成员清单相符；无重复成员、危险路径或链接成员。
源码保护核对覆盖 **323 个原文件**：db 12、third_party 31、handoff 83、基线 reports 197；本轮对原 Git blob、字节和可执行模式核对，零差异。
历史文本中的未完成状态保持原版本含义；例如 R2a.2 对 cleanup_runtime 原限时的 90 秒文字错误，后继离线报告已明确纠正为 180 秒，未改历史日志或实际限时。

## 最终 SHA 的 GitHub Actions 与源码快照

本轮直接 `gh api` 读取 [CI run 40930613935](https://github.com/wmqfl861/wls861/actions/runs/40930613935)，
其 head 为完整 `55dc989e…`、status=completed、conclusion=success；六个任务均绑定该 SHA。
另下载原始日志 ZIP（SHA-256 `1365be88b06748a14510663deed352ea6bf30577bb23f04bf88fa6282259da9a`），核验以下真实终态：

| 任务 | job ID | 本轮核对的原结果 |
| --- | ---: | --- |
| strict / umask 0022 | 123647727722 | success；日志 100%，0 failed out of 81 |
| strict-umask-0002 | 123647727121 | success；日志 100%，0 failed out of 81 |
| Clang-18 | 123647727603 | success；日志 100%，0 failed out of 81 |
| UBSan | 123647727615 | success；日志 100%，0 failed out of 81 |
| ASan | 123647727821 | success；日志 100%，0 failed out of 81 |
| Make-ASan | 123647727650 | success；原 GCC `make -j2 asan`、用户 CPPFLAGS 和最后三个 Python 套件终态完整 |

Make-ASan 是完整 Make 目标，不称为 81/81 CTest；其原编译日志保留
`-DWLS_CI_USER_CPPFLAGS=1`、ASan、严格警告，原日志最后有计费 16 方法及后续 10/14 方法的 OK。
本轮也重新核验旧交付现场的完整合并日志 hash
`e75765f5ae2e1cc707a06bcf6e097fa43f80c95eda0fd42fe83839fa16e4989a`。

[源码快照 run 40930613463](https://github.com/wmqfl861/wls861/actions/runs/40930613463)
同 SHA、completed/success。重新下载 artifact **12920854435**，ZIP SHA-256 为
`1b4fe5ae088db8bfe39e324dcb4282690b0a83f7c63bd30124906cb877011c98`，与 GitHub digest 相符。
`COMMIT`、`TREE`、`SHA256SUMS` 和 `source.tar.gz` 全部核验：

- `source.tar.gz` SHA-256：`98a347e0201bf4092d70111665a73d87ac1000bb3072ac53ff9993142d5fce14`。
- 解压 tar SHA-256：`3c511ac374652f92149b08873774e61444bd3ef5acc18d3dfaf075483526a085`。
- tar 与本地只读 `git archive 55dc989e…` **逐字节相同**，312 项冻结输入全部对应提交树。

原发布后的 `evidence/r2-4-final-delivery.json/.md` 及其八个证据引用均已重新核对；
JSON SHA-256 为 `4e51e29813b4163cc49885360a28fc13fa1766f46bb76cfc665c1cdaf0c2c275`。
本轮读到远端 main 仍为 `a6b8559…`，R2 分支仍为 `55dc989e…`。
提交内旧报告的提交前登记点、旧失败摘要和发布后回执继续分开，不在旧文件内伪造未来 SHA。

正式 PR 创建另触发 [run 40991571597](https://github.com/wmqfl861/wls861/actions/runs/40991571597)。
主执行者当前观察为 strict、0002、Clang、UBSan 已成功，ASan/Make-ASan 仍在运行；
其本轮观察保存在 `evidence/r24-pr-observation.json/.log`，后续进度由主执行者继续核对。
上述原 push 六路完成结论不冒充此新 PR 事件也已全部完成。

## 本轮新验证与尚未证明的边界

本审计没有重新编译或运行实现测试。主执行者在本轮文档工作中实际执行了
`tests/contracts_tests.py`，**38/38、exit 0**，以及 `make verify-handoff`，
两份原件完整性脚本均 **exit 0**；原 log hash 与 JSON 重新核对一致。
这两个命令使用 UID/GID 1000、umask 0002，记录是 `specification-contracts` 和
`published-source-integrity`。它们是本轮新执行；2026-10-29 五门禁是本轮复核的历史执行。
完整文档治理验证另见[本轮总报告](blueprint-v0.5.0-governance-validation-20261030.md)。

[源码独立复核](../reviews/r2-4-independent-audit-20261030.md)未确认阻断性生产缺陷，但明确以下覆盖限制：

- 4096 条结算故障表、触发第 4097 条及容器分配异常没有找到直接运行回归；本项只有源码保持和守卫分析，不能由五门禁全绿推断触顶分支已执行。
- CANCEL_REQUESTED、孤立 CLAIMED inbox、FAILED job，没有各自独立新增 R2.4 红绿例；也没有给每个公开 Actor 方法、每种非 PREPARED 状态或所有并发调度排列单设反例。公共守卫与综合回归证据保留其限定范围。
- Store 是受信宿主 API，session 围栏不构成对任意同 UID/同进程代码的密码认证。UNKNOWN、CAS、预算、终态、原退出及资源所有权各是独立条件，恢复成功和新会话都不证明进程退出。
- 合成退出证据、FakeModel、本地 TLS、SIGKILL 和源码哈希不证明真实供应商质量/费用、生产强沙箱正向、整机掉电、并发写者租约服务或私有行 purge。没有真实模型付费授权或生产操作结论。

主协调 Agent 决定上述覆盖补充和下一张任务卡；本轮不扩大实现或把覆盖不足编造为已复现缺陷。

| 状态 | 精确范围与本轮结论 |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | 原 R2.4 `55dc989e…` 已完成；本轮源码复核无确认阻断缺陷 |
| `LOCAL_VALIDATION_COMPLETE` | 原同 v3 五门禁完整通过，已重核原记录与输入身份；历史失败原样保留 |
| `PUSH_COMPLETE` | 原 R2 分支远端指向 `55dc989e…`，已实时读取确认 |
| `REMOTE_CI_COMPLETE` | 原提交 push run 40930613935 六路及源码快照 40930613463 完整通过；新 PR 事件运行另计 |
| `PR_REVIEW_COMPLETE` | pending；等待主协调 Agent 正式审核 |
| `MERGED` | false；未合并 main |

## 全部 40 份历史报告阅读清单

下表范围固定为交付提交 `55dc989e…` 的报告集合，不包含本轮新增报告自身。
阅读者 A 为 `evidence_audit`（24 份），B 为 `independent_recovery_review`（16 份）。
全部为本轮全文语义阅读；逐文件完整结论和 SHA-256 已记录到机器审计，不能用此清单的哈希代替阅读。

| 实际路径（相对 docs/reports） | 阅读者 | 历史范围或关键限制 |
| --- | --- | --- |
| [M1-validation.md](M1-validation.md) | B | 0.1.0-m1，Make严格/UBSan同49项各49/49，不相加98 |
| [audit24-recovery-validation.md](audit24-recovery-validation.md) | B | 0.7.2三P1恢复与容量窗/Root CAS |
| [billing-baseline-ci-followup.md](billing-baseline-ci-followup.md) | A | 首次远端两ASan取消，CMake70通过/1超时/7未完成 |
| [billing-baseline-independent-validation.md](billing-baseline-independent-validation.md) | A | 联合Release14/14方法，五任务两条件10/10格 |
| [billing-baseline-validation.md](billing-baseline-validation.md) | A | 0.11.0首次发布1df86cef，305输入 |
| [claude-review-fixes-validation.md](claude-review-fixes-validation.md) | B | 0.7.1/fcdbdd0，原105 C++失败及s15失败、Clang17真UAF |
| [claude-review-resume-validation.md](claude-review-resume-validation.md) | B | 0.7.1接续1d7467d，原GCC13 Make ASan -O1 regex Werror失败 |
| [code-audit-20261027-validation.md](code-audit-20261027-validation.md) | A | 0.11.1审核以d3da700c为基线 |
| [five-node-validation-20261027.md](five-node-validation-20261027.md) | A | 0.9.0/Store410，281输入，Release70/70双mask、ASan7/7、Clang九TU语法 |
| [independent-audit-validation.md](independent-audit-validation.md) | A | ADR079/0.8.6，258输入，Release63/63双mask、ASan/UBSan各12组 |
| [live-workflow-evidence-validation.md](live-workflow-evidence-validation.md) | A | v2来源专项31/31、工作流TLS18/18及ASan18/18 |
| [r1-history-evidence-validation.md](r1-history-evidence-validation.md) | B | 0.7.3，分页1..64/高水位/16KiB原反馈回读 |
| [r1-management-window-validation.md](r1-management-window-validation.md) | B | 0.9.0，管理窗14方法含130轮与五维live投影 |
| [r1-structured-context-validation.md](r1-structured-context-validation.md) | B | 0.7.4，显式structured-v2/旧缺省framed-v1、原工具配对/逻辑指纹 |
| [r1-worker-history-window-validation.md](r1-worker-history-window-validation.md) | B | 0.7.5，显式window-v1/完整近期轮次与窗口外hash引用 |
| [r2-4-demand-driven-recovery-validation.md](r2-4-demand-driven-recovery-validation.md) | A | R2.4 v3: 三个新增套件17+6+3，WorkerTools另增两个子例 |
| [r2-integration-validation.md](r2-integration-validation.md) | A | c975be3精确R2.2基线重新构建52/52，补原STATUS缺失报告 |
| [r2-model-amendment-checkpoint.md](r2-model-amendment-checkpoint.md) | A | 历史执行服务中断检查点，未来步骤和未确认补丁均非本轮指令 |
| [r2-model-amendment-validation.md](r2-model-amendment-validation.md) | A | R2.3 231输入，Release双mask/UBSan54/54及Make通过，本地ASan53/54和隔离90秒均失败 |
| [r2-rejected-task-validation.md](r2-rejected-task-validation.md) | A | R2.1基线红例与22C++/10CLI/1契约 |
| [r2a-allocation-validation.md](r2a-allocation-validation.md) | A | 236原清单发布只db/README一项文档差异，其余235相同 |
| [r2a-directory-validation.md](r2a-directory-validation.md) | A | 233输入，Release56/56双mask、ASan/UBSan各5相关组 |
| [r2a3-r3-offline-validation.md](r2a3-r3-offline-validation.md) | A | 255输入，Release63/63双mask、ASan/UBSan各8组，新增88C++/25CLI |
| [r3-evaluation-validation.md](r3-evaluation-validation.md) | A | 初版评测23/23，五任务十格固定条件，真实SQLite原事实/失败未知未执行保留 |
| [r3-https-transport-validation.md](r3-https-transport-validation.md) | A | 0.8.7组件262输入，Release64/64双mask、ASan42方法1套、Clang两TU |
| [r3-live-core-compatibility-20261026.md](r3-live-core-compatibility-20261026.md) | A | 核心四套兼容4/4/16.52秒、UID65534 |
| [r3-original-store-validation.md](r3-original-store-validation.md) | A | Store410 15native+10TLS方法，TLS内五真实SIGKILL |
| [r3-owned-capacity-validation.md](r3-owned-capacity-validation.md) | A | 原版4097合法条目无法进入4096库存，零删除 |
| [runtime-resources-validation-20261026.md](runtime-resources-validation-20261026.md) | A | 35资源/160549字节，14资源+31Store+12Schema与安装10方法、真实重定位/删除来源/FIFO拒绝 |
| [s1-acceptance-delivery-validation.md](s1-acceptance-delivery-validation.md) | B | 0.6.0/Store406，strict/UBSan263 S1 C++和55CLI，CMake29/29 |
| [s1-event-runtime-validation.md](s1-event-runtime-validation.md) | B | 0.4.0/Store404，strict/UBSan167 S1 C++和26CLI，CMake22/22 |
| [s1-foundation-validation.md](s1-foundation-validation.md) | B | 0.2.0-s1.1/Store402，strict/UBSan72 S1 C++和8CLI，CMake15/15 |
| [s1-owned-cleanup-followup-validation.md](s1-owned-cleanup-followup-validation.md) | B | 0.7.0接续精确恢复1df3833 |
| [s1-owned-cleanup-validation.md](s1-owned-cleanup-validation.md) | B | 0.7.0首轮303 S1 C++/68CLI、CMake32/32，后续普通用户CI暴露三处故障夹具未达目标边界，不能作为有效发布依据 |
| [s1-worker-tools-validation.md](s1-worker-tools-validation.md) | B | 0.3.0/Store403，strict/UBSan138 S1 C++和16CLI，CMake19/19 |
| [s1-workflow-runtime-validation.md](s1-workflow-runtime-validation.md) | B | 0.5.0/Store405，strict/UBSan202 S1 C++和38CLI，CMake25/25 |
| [signed-billing-validation-20261027.md](signed-billing-validation-20261027.md) | A | 签名账单专项299输入、UID65534 |
| [single-agent-baseline-validation-20261027.md](single-agent-baseline-validation-20261027.md) | A | 原生11/11、评测40/40，身份需完整跨角色原历史，v3恢复区间与原instruction相连 |
| [two-module-validation.md](two-module-validation.md) | A | 0.10.0/296输入，Release74/74双mask、ASan8/8、Clang十一TU |
| [v0.4.1-validation.md](v0.4.1-validation.md) | B | 0.1.1-m1，strict/UBSan55 C++/CLI、Python35、本地CMake8/8 |

全部原报告与归档的身份和原结论保留。新任务外部证据文件 `r24-audit-core-observations.json`、
`r24-audit-provenance-observations.json`、`r24-audit-build-identity.json`、
`r24-audit-machine-inventory.json`、两份 `r24-audit-semantic-reads-*.json` 及
`r24-audit-remote-*` 保存本轮独立观察；所有引用的大小和 SHA-256 由
`evidence/r24-evidence-audit.json` 固定。没有把字节完整性检查、静态审查、历史执行和本轮新测试合并成一个泛化的通过声明。
