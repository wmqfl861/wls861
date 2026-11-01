# Store 事件命令规范字节复用验证（2026-11-01）

Store 事件写入现在只生成一次经过原有校验的命令规范字节，并将该字符串同时用于 command_sha256 和 payload。同一冻结探针在原生产库上观察到首次序列化 2 次、在候选库上观察到 1 次；两边精确重放均为 1 次。候选的全部探针断言、五项定向 CTest，以及提交08在umask0022和0002下的完整Release各 **92/92** 已通过。其余本机矩阵仍未完成，本文不把计数减少写成已测性能加速，也不把它认定为历史 Timeout 或 fairness 失败的根因。

本版执行证据截点为 `2026-11-01T04:20:59.760968Z`：保留此前定向执行、提交、限定审核、推送及完整Release0022，再单列04:20:56结束的完整Release0002和04:20:59结束的runner自检。共享GCC门禁刚启动，尚无结束结果；后续计划和启动状态不是通过证据。首个事件文本包仍冻结在04:07:58范围，未将后续结果混入；其非作者包/0022复核于04:27:30结束，作为单独审核事实补充。

**基线、提交与证据位置。** 原基线为 `579e3c7675bc2abb336dde6a95bfddf2139f47cd`，本地提交为 `08dad7e815b9c3c84d3bd2393818b2030a6e0cde`，分支为 `fix/zcode-local-validation-20261101`。该提交只改 `src/store.cpp`（增加五行、删除一行）和新增 303 行 `tests/repros/store_event_serialization_same_source.cpp`。未修改 CTest/Make/CI 目录、92 项产品注册及其期限、SQL/迁移、third_party 或历史报告。实现与 Store 版本不变。

任务根目录为 `/mnt/d/codex-workspace/wls861-zcode-followup-20261101-wvm7aclp`；以下 `E/` 指根下 `evidence/`。原版隔离源码在 `event-baseline/`，候选在 `zcode/`。本地大日志索引不是公开附件；本批独立文本证据包在正式发布前保持候选状态，其限定审核结果单列。此前六文件工具交付的结果仍以 [native 续报](zcode-native-diagnostics-followup-20261101.md) 原时间范围为准，不回写其结论。

新文本包候选为 `E/public-event-receipts-01/store-event-command-bytes-receipts-20261101.zip`，231,407字节，SHA-256为 `2d4d9cffbc5720775fa5b427433cd14c9993fdbcafeb3640d42ffc440efb1af7`，92个文本成员；成员数与产品测试数分别计数。它包含截止04:07:58的九轮已结束recorder证据、四轮编码工具身份投影、精确探针/源码差异、各版本依赖/身份、限定源码审核、定向及完整0022的JUnit/各自LastTest、推送回执、README/来源映射/成员清单/SHA256SUMS/扫描收据。保留一份最终794项源码清单，其余重复清单以原SHA、变化项及关键输入身份表示。CRC、全部成员字节、91项校验和、相对路径/普通UTF-8类型及列明秘密模式检查通过；打包后PROVENANCE直接引用的91个原件重新核对身份均相同。未纳入二进制、数据库、gmon、设置、Coding Plan对话/提示词或原始进程/环境dump。字段投影与安全的四项验证环境值有明确说明；有界扫描不代表穷尽秘密检测。该包已完成限定非作者只读审核，在正式发布前仍为 `CURATED_TEXT_REVIEW_CANDIDATE_NOT_PUBLISHED`。之后完成的Release0002/runner结果尚未收入此首包，后续另补证据归档。

**实际作者调用与最小修改。** 本机已安装的 ZCode CLI `3.16.3-39 / runtime 0.17.0` 使用既有 GLM-5.3 Coding Plan，round07–09 编写和加强 test-only count probe，round10 编写生产补丁。root 与非作者 Agent 阅读实际差异、要求修正并运行下述验证。各轮原 start/completion、prompt SHA、stdout/stderr 身份在对应目录；编码工具 exit0 仅表示调用结束。

| ZCode 调用 | 开始—结束 UTC | exit / 秒 | 范围 |
| --- | --- | --- | --- |
| `E/zcode-round-07/` | 03:27:39—03:34:50 | 0 / 431.628830 | 首版手动计数探针。 |
| `E/zcode-round-08/` | 03:38:20—03:41:28 | 0 / 187.735116 | 独立命令字节预期、合法非平凡 JSON、持久证据与重放/拒绝检查。 |
| `E/zcode-round-09/` | 03:42:23—03:43:42 | 0 / 79.532887 | 完成全局事件计数等最终探针修订。 |
| `E/zcode-round-10/` | 03:45:36—03:46:42 | 0 / 65.741695 | 仅修改 Store 事件写入的一次编码复用。 |

最终生产差异位于 `Store::Impl::event`。在原请求 scope、command_id、type 与容量守卫之后，保留 event_id 的生成位置，再取得原 `json_text` 返回值：

```cpp
guard_capacity_event(request_id, command);
const auto event_id = new_id("evt");
const auto encoded_command = json_text(command, payload_limit);
// 原 events INSERT 中的绑定：
{event_id, request_id, type, actor, command_id,
 sha256_hex(encoded_command), encoded_command,
 json_text(result, 16 * 1024 * 1024), stamp()}
```

原路径分别调用 `sha256_hex(canonical_json(command))` 与 `json_text(command, payload_limit)`，后者又对同一不可变 command 做规范编码。现在复用一次函数内 const 字符串，没有跨调用、事务、Store 或 Actor 缓存。`json_text` 本身未改：仍先调用 canonical_json，再检查原字节上限，再经 parse_json_strict 校验后返回。UTF-8、结构/深度、有限数值、重复键等原严格解析规则没有被删除；默认 1 MiB、live 结算 4 MiB 及 result 16 MiB 等原调用限额保持。

event_id 仍在容量守卫后、command 编码前生成。原 INSERT 列与其余绑定、capacity_event_inserted、route_event 使用的 sqlite3_last_insert_rowid、observe_capacity 及顺序保持。原事务、锁、Actor/session/epoch、CAS、UNKNOWN、claim/settle、Workspace、进程退出和终态/清理边界均未增加旁路；精确重放自己的 fingerprint 路径未改。成功路径的 hash/payload 继续来自同一确定编码契约。

异常顺序需准确限定：新路径在计算 SHA 前完成 command 字节校验，旧路径先为 hash 做一次规范编码/SHA，再走 payload 校验。编码失败仍在 INSERT 前并受原事务回滚约束，但本文不宣称所有分配、字符串复制、crypto 等低层异常的细微求值顺序完全相同。

**同源探针与真实旧版反例。** 正式红绿使用 `E/event-serialization-red-green-02/` 中冻结的 16,150 字节探针，SHA-256 为 `03f286cd1f357f4e2f4b3cb531d5fdf49cd74866da19ca8230b80402688a1856`。它是一个手动探针、一个方法，未注册为新 CTest。早期 `event-serialization-red-green-01` 的计划/首版源保留；以下结果只绑定最终 03f286 源，不把较早候选设计当作已执行反例。

探针使用真实临时 Store、合法 admit、当前 Store 签发的 Dispatcher 和第一次 plan 发布。GNU ld `--wrap=_ZN3wls2s114canonical_jsonB5cxx11ERKN5boost4json5valueE` 仅对带唯一夹具 marker 的顶层 plan 命令计数，随后始终调用真实生产 canonical_json；它不替代解析结果、权限/容量守卫或期限。验证阶段关闭计数，重放另行计数。

原版与候选均以 GCC13、C++20、`-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread -MD` 编译同一冻结源，分别使用自己的 tests/include、`WLS_SOURCE_DIR` 与生产 archive。原版链接 `E/event-serialization-red-green-01/baseline-libraries/{libwls_s1.a,libwls_kernel.a}`；候选链接 `zcode/build-zcode-release/` 下对应库。完整准确 argv、cwd、普通用户、umask、TMPDIR、退出与原输出在各轮 command/completion 中；生成的 old-probe.d/candidate-probe.d 证明各自 fixture/头依赖来自各自源码，没有相对 include 混版。

| 原始记录 | 已执行结果 | exit / recorder 秒数 |
| --- | --- | --- |
| `E/event-probe-old-compile-01/` | 原生产库探针编译成功，stderr 空。 | 0 / 26.855186 |
| `E/event-probe-old-run-01/` | 首次 count2、replay1；全部持久/拒绝断言已到达，仅最后第 292 行 `published_count == 1` 失败。 | 1 / 0.776684 |
| `E/candidate-event-release-build-01/` | 使用 `cmake --build build-zcode-release --parallel 2` 完成原 Release 默认目标的增量重建。 | 0 / 81.354688 |
| `E/event-probe-candidate-compile-01/` | 候选自己的库与头编译同一探针成功，stderr 空。 | 0 / 24.247816 |
| `E/event-probe-candidate-run-01/` | 首次 count1、replay1，PASS/RESULT 1 passed, 0 failed；stderr 空。 | 0 / 0.795043 |

以上均为普通 UID/GID1000、umask0022、数据盘独立 TMPDIR、outer_timeout=false；各轮 793 项 tracked source-before/after 相同。执行程序分别为 `E/event-serialization-red-green-02/old-probe` 与 `candidate-probe`。旧 red 有编译成功、wrapper 实际命中 count2、合法业务及完整前置断言证据，因此不是编译错误、非法夹具、早期守卫拒绝或未命中包装器造成的失败。两个单次带 wrapper 的时长不构成性能基准或加速结论。

探针的合法 body 包含中文/转义、嵌套 object/array、null、int64、uint64 最大值、double 和 negative zero。它在计数前独立构造 captured Actor、CAS、requirements 和原 body 的 expected_command，逐字核对 SQLite payload、command_sha256、result 及解析后的字段。首次发布只增加一个 state_version、一个 plan_revision、一个请求事件和一个全局事件，保留原 epoch、requirements、config identity、snapshot、budget 及已有历史前缀。

精确 same-key 重放、foreign writer_session 在原 key/新 key 上的 Stale 拒绝、同 key 改意图的 Conflict 拒绝后，request/snapshot/budget/plan、事件计数及原 payload/result/hash 保持冻结值。最后才断言首次 count==1，旧 red 因而不会跳过这些检查。证据范围仍有限：原历史串只比较 seq/type/command_id/command_sha256/payload/result 六列，不含 event_id/actor/created_at，也不是整个数据库文件；foreign session 负例不是一次真实 recovery 后旧 Actor 的完整场景；本例 budget 不涉及非零 reserve/settle。不同进程的随机身份不能当作跨版本全部原始数据库字节的直接差分。

**输入、库、二进制与提交映射。** `old-after.json` 和 `candidate-after.json` 保留各自 43 项输入、实际 `.d`、二进制 before/after 与 unchanged 事实。归一源码根和库路径后，保存输入差异仅为 src/store.cpp 与 libwls_s1.a，其余 probe、fixture、头、kernel 及保存的 runtime_types 源身份相同。非作者审核重哈希其中可检查的 42 项；runtime_types.cpp 只检查原保存元数据，不应写成复核者重新读取并哈希了全部 43 项。

| 身份 | 原版 | 候选 |
| --- | --- | --- |
| `src/store.cpp` SHA-256 | `96d9194ac01bdc9d0f7edb964665dd896bb3ec858bfdbcdb68313be66df48931` | `7b234b88a369a246e0f1fa5c42001d466608d4876822a51595914517989fc3dc` |
| `libwls_s1.a` SHA-256 | `27d4259e1c3d6db42dcb440a6ca06555fe0e79fc7ec8c8ef60fc90661bd06891` | `46b249ffca506c557170e0279e2c7419dc2c0eca471378be01d7e3f3debed504` |
| 探针 binary SHA-256 | `1be7f6e084e59b8929b7169a55ab7b781cc9a8268597c3897ac04947d0a92357` | `a6dc63c16517aec1b1b06780cf693ae6c07facc01ee2616abdd6ece5e17c67d6` |
| 编译依赖 `.d` SHA-256 | `b29d7165ceb17eb24ef4a9044b67fbf571668d14cea9e1fd79a8ef40c844b8c0` | `c6265f69c4a2f74824cd3c403d6ab9a8a1a9e6354e1e64db66a912e7c9e8ee82` |

两边 kernel archive 的 SHA-256 同为 `a780803a40cb241a40454fad86b7ec19da7e28babb66110aa49ce6f0f2e1767f`。build/probe/focused 的真实运行收据均记录 **HEAD579 加精确候选工作区字节**，因为它们先于提交；不得改写为当时已经运行提交08。`E/event-command-bytes-review-01/commit-mapping.json` 将实际运行的 Store/probe 字节对应到本地提交08，确认 parent579、只有上述两文件差异。该提交映射收据 SHA-256 为 `ea5063f5ce36bd221a94de3599a428450a83fd88c2553dc5a1d8298d09f83017`。

**定向回归、两种umask的完整Release与剩余矩阵。** root 执行的精确选择是 `ctest --test-dir <任务根>/zcode/build-zcode-release --output-on-failure --parallel 1 --no-tests=error --output-junit <E>/event-focused-release-01-identity/ctest.xml -R '^(runtime_types|store|event_store|audit24_capacity|r24_recovery)$'`。这里只缩写绝对根目录，完整 argv 保存在 `E/event-focused-release-01/command.json`。

该轮 UID/GID1000、umask0002，五项 CTest **5/5 PASS**，0 failure、0 timeout、0 skipped、0 selected-not-run，exit0；CTest 内部 21.03 秒，recorder 21.754976 秒。五项原 TIMEOUT 均为 90 秒；92 是整个注册目录数量，其余 87 项不属于本次定向结果。61 项 binary/control 文件 before/after 相同，实际二进制与冻结 test-plan 相符。JUnit 的部分 system-out 受到原 1024 字节阈值截断，另存完整 LastTest.log 包含五项结束 RESULT/Test Passed；不把截断 JUnit 当作完整方法原文。收据在 `E/event-focused-release-01-identity/receipt.json`，SHA-256 为 `6622c4b4bd319d2766fbd896e45848c982e3035e67a146184bf77641b4e6adc3`。

完整 Release0022 在提交 `08dad7e815b9c3c84d3bd2393818b2030a6e0cde` 上执行全部原92项，UID/GID1000、umask0022、parallel1，原期限保持。它于 `2026-11-01T04:07:37.786883Z` 正常结束，**92/92 PASS**，0 failure、0 timeout、0 skipped、0 not-run，exit0、outer_timeout=false；recorder 893.977124秒，CTest内部893.47秒。794项 tracked源码前后相同，61项binary/control文件前后相同。它与此前 HEAD579+dirty 的定向阶段明确分开；新增探针已在提交08跟踪，因此此轮源码计数为794。

本轮命令沿用上述 ctest 参数，去掉 `-R` 以执行全目录，JUnit写入 `E/event-full-release-0022-01-identity/ctest.xml`。原输出和完成收据在 `E/event-full-release-0022-01/`，独立复制完整LastTest与test-plan在其 `-identity/` 目录；receipt SHA-256为 `69a5053f6cfc9640ab1722fa29966fa423e510d67543d91136659db6b5a23aa3`。这份JUnit实际有92个testcase，无failure/skipped。源码清单SHA为 `ebccdd21546d9b18a834d391da6fdcb63a5eb93a2e9cf738d98d7eb4253e50e3`，binary/control清单SHA为 `21c661aa17dc45af1a239d7c259455802879c517decbf0520d05157459626f00`。

04:20追加：完整Release0002的recorder于04:07:59.587111启动，在 `2026-11-01T04:20:56.417195Z` 结束，精确HEAD08，**92/92 PASS**、0 failure/timeout/skipped/notrun，exit0、outer_timeout=false。recorder为776.829678秒，CTest为776.49秒；794项源码及同一61项binary/control清单前后相同。原命令、输出和完成记录在 `E/event-full-release-0002-01/`；JUnit、各自完整LastTest及身份在对应 `-identity/` 目录，receipt SHA为 `d05c6108babf9e60249be6084ea0909af69c43b9d0a262aad57177a951d025ae`。这是同一92注册目录在第二种umask下的完整执行，不计成184个独立套件。

随后同一提交08的 `tests/test_run_gcc_asan.py` 在04:20:59.435900结束，24/24工具方法PASS、exit0、1.236783秒，794项tracked输入未变；原记录在 `E/event-gcc-runner-selfchecks-01/`。该工具自检与产品CTest分别计数。04:20:59.760968启动 `scripts/run_gcc_asan.py --source <任务根>/zcode --data-root /mnt/d --compiler /usr/bin/g++-13 --python <既定venv>/bin/python --expected-tests 92`，fresh目录为 `build-gcc-asan-20261101T042059Z-b7698edb`；正式runner未修改，历史失败关闭标志仍为false。本版没有该共享GCC门禁的完成记录。

| 后续所需验证 | 04:20:59截点的真实状态 |
| --- | --- |
| 完整 Release，umask0022 | **PASS，92/92**，精确提交08，已真实结束。 |
| 完整 Release，umask0002 | **PASS，92/92**，精确提交08，已真实结束。 |
| GCC gate runner selfchecks | **24/24工具方法PASS**，单列，不是产品sanitizer通过。 |
| 共享GCC13.3 Debug ASan/LSan | `E/event-shared-gcc-asan-gate-01/` 已启动，**RUNNING / 未判定**。 |
| 完整UBSan、Clang18严格构建/测试 | 串行驱动后续项，**NOT_RUN**。 |
| 本地原Make-ASan | 本批未安排本机执行；精确SHA远端该路线另行审核，不从其他配置推导。 |
| 其他集成与历史兼容 | 属于已执行92项的注册按本轮原JUnit记录通过；尚需其他配置的适用覆盖，历史失败不因此关闭。 |
| 提交08推送、精确 SHA 远端 CI 与源码快照 | **推送/远端ref已核验**；CI及远端源码快照未由本版判定完成。 |

剩余矩阵计划在 `E/event-final-local-matrix-plan.json`，其冻结时的状态字段不回填为结果；`E/event-remaining-matrix-driver-01/events.json` 记录04:07:58.199725启动Release0002。正式运行期间root保持候选工作区冻结，后续每项仍须独立检查完成记录、输入/二进制身份与失败事实。

**实际推送状态。** `E/publish-08dad7e-01/before.json` 记录推送前 origin/main仍为 `a6b8559018a599d7a806b05942857a56d69a00ce`、远端功能分支仍579；after.json记录正常push exit0、ls-remote功能分支指向08，且 [Draft PR #12](https://github.com/wmqfl861/wls861/pull/12) 为OPEN、base仍 `fix/async-review-99c9063-20261031@7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a`、head为08。提交tree为 `149bca333bce2460ba940ace3c76cc1111dfdd83`。after收据SHA为 `516d91cb282c3d8b62a3c1338d4b796a0b13238c3791844a11d6f67882135de2`；原push输出保留在Codex工具会话23992，没有独立原始日志文件，不能将该收据写成完整push原日志。未合并或retarget既有PR。

**限定独立审核与待补收据。** 非作者 `/root/independent_recovery_review` 只读检查精确补丁、旧 red、候选 green、`.d` 与五项原始执行证据，没有编译或运行这些测试，也没有编写 Store 补丁。其 `E/event-command-bytes-review-01/review.md` 已追加 `STATIC_PATCH_REVIEW_COMPLETE`、`TARGET_RED_VERIFIED`、`CANDIDATE_GREEN_AUDITED`、`FIVE_FOCUSED_CTESTS_AUDITED` 为 TRUE；`verified-green-focused.json` 的限定结论为 `VERIFIED_TARGET_GREEN_AND_FIVE_FOCUSED_CTESTS`。先前审核正文仅 red/static 的时间范围仍保留，后续追加没有把它冒称为当时已核 green。

该复核期间一次元数据路径分类检查先错误地将候选 build 目录中的 archive 归作 source，产生检查器 assert exit1；修正分类顺序后核验通过。原错误与修正记录在 verified-green-focused.json；它不是产品 red 或候选产品测试失败。图覆盖存在旧/partial 路径，本地精确源码与 frozen diff 提供实际依据，不以图覆盖推断全仓完整性。此为同模型非作者工程复核，不等于主协调者正式接受或第二模型验收。

该限定源码/红绿/五项复核于03:57:25.760302结束，receipt SHA为 `edf6410e1f0371b1ee8f60b559d87e7a23501cdceaaf182c0dbdef048c3da3f6`。它早于完整Release0022结束，不冒称已经审核后者。

另一次非作者包/0022复核在 `E/event-public-package-review-01/`，于 `2026-11-01T04:27:30.638912Z` 完成，结论为 **NO_PACKAGE_BLOCKER_IN_BOUNDED_REVIEW**。它验证92个文本成员、91项校验和、90项member inventory及88条provenance；55份逐字原件与28份投影复建一致，91个直接来源加两个去重alias的after原件共93份身份匹配。独立核对完整0022的92项目录/JUnit名称顺序、92个LastTest通过标记、794源码路径与08提交树，以及61项binary/control前后身份。review.md SHA为 `f421e0a21d8729ee587c9cbea125b03104d43f1b09e692b1fa0ad1972a02ee47`，completion-receipt.json SHA为 `7a91485eb7666dcd8ed1324b76a1806d572bd5f77fc0ce3a631af2f2c0e1f5bf`。该审核没有运行归档代码/产品或编译，没有改包、旧证据或工作树；它的来源计数/中文路径引用检查修正也保留在收据，未当作产品失败。

这份包审核仅覆盖首包截止04:07:58的范围，**未审核04:20新增0002/runner或后续门禁**，没有执行远端ref/CI查询，也不是主协调者批准。剩余完整矩阵、新结果补包及其独立复核仍待真实结束记录后追加。无需修改原蓝图、历史SQL或接口契约来实施本次函数内字节复用。

**真实状态与仍开放事项。** 所有历史 Release0002 owned_capacity、UBSan/ASan Async Timeout、原 ASan resource 探针失败、更早 GCC/Clang/live_workflow 根因均继续 OPEN，`historical_clang_asan_failures_closed=false`。Windows 11 普通用户验收仍 NOT_RUN，完整 Windows runtime 未因本修改完成。产品验证使用 FakeModel/合法临时数据；本轮编码工具调用不授予真实 WLS 供应商、付费 API 或生产操作权限。

| 状态 | 本版截点的准确结论 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | **TRUE_IN_BOUNDED_SCOPE**：一次规范字节复用与一个真实手动回归探针已实现并本地提交08。 |
| LOCAL_VALIDATION_COMPLETE | **FALSE**；机制红绿、定向5/5、两种umask的完整Release各92/92及runner24完成，ASan门禁运行中，UBSan/Clang尚未执行。 |
| PUSH_COMPLETE | **TRUE_FOR_08dad7e**；功能分支和Draft PR12 head已读回。本报告及新文本包自身尚未发布。 |
| REMOTE_CI_COMPLETE | **FALSE / NOT_VERIFIED_FOR_08dad7e**。 |
| PR_REVIEW_COMPLETE | **PENDING**；主协调者正式采纳未由同模型限定审核代签。 |
| MERGED | **FALSE**。 |

下一次追加应记录剩余矩阵实际结束状态、源码/二进制冻结核验、新文本包的独立审核及最终发布回执。不得用一次count2→count1的机制证据、单配置完整绿色或此前579的远端绿色关闭尚未验证的产品/历史问题。

**V3追加：本机共享GCC门禁仍运行，已观察到真实Timeout（2026-11-01T05:02:28Z）。** 前述V2正文和04:20:59执行截点保持原样；以下是新的观察范围。`zcode/build-gcc-asan-20261101T042059Z-b7698edb/evidence/ctest.log` 的05:02:28.837295只读快照已出现下列七项CTest超时：

| 原注册项 | 此次原CTest输出时长 | 快照中的结果 |
| --- | ---: | --- |
| `r3_offline_acceptance_cli` | 600.12秒 | `***Timeout` |
| `store` | 90.39秒 | `***Timeout` |
| `workflow_store` | 90.31秒 | `***Timeout` |
| `acceptance_store` | 90.40秒 | `***Timeout` |
| `event_runtime` | 90.39秒 | `***Timeout` |
| `workflow_runtime` | 90.39秒 | `***Timeout` |
| `acceptance_runtime` | 90.30秒 | `***Timeout` |

这是运行期间已经结束的失败子项，**七不是最终失败总数**。官方summary仍为 `NOT_COMPLETE`，外层recorder尚无completion；不能给出最终CTest/runner退出码、完整通过数、最终source/binary-after或整个进程树结束证明。本阶段准确状态为 **RUNNING_WITH_OBSERVED_TIMEOUTS**，`LOCAL_VALIDATION_COMPLETE=false`。原600/90秒期限及正式runner保持，ASan/LSan配置仍含 `halt_on_error=1:detect_leaks=1`，`historical_clang_asan_failures_closed=false`；此处没有放宽期限、关闭sanitizer或改变生产输入。

观察依据保留在 `E/public-event-late-results-plan-01/observations/`。04:57:59.142706的首份观察为10,004字节、可见前四项Timeout，SHA-256为 `ed63e15ec55e6df19e330c19687d686a05a4dd05c10dcbc9590304a2cdbc845d`；05:02:28.837295的观察为12,272字节、可见上述七项，SHA-256为 `e6c674b3ec39f904dbd2b64ee2c976bd2f39faa0bede963539f77a49cca03acb`。两份快照通过随后读取的同长度日志前缀重建并匹配首次读取SHA；原观察时间、复制时间和方式写在 `gcc-running-observations.json`，不改写正在增长的原日志，也不把快照称作最终完整日志。

CLI只读分析现已在 `E/asan-offline-cli-timeout-analysis-01/` 冻结，于 `2026-11-01T05:06:38.339596Z` 完成，限定结论为 **READ_ONLY_STAGE_LOCALIZATION_COMPLETE / PERFORMANCE_ROOT_CAUSE_OPEN**。001–024共24份结束收据，其中21份exit0、3份内部60秒Timeout，各自截断到毫秒的 `elapsed_ms` 合计572931，即572.931秒；该总和不是Python总wall time。005是tool_roundtrip/single/initial（60371ms）、016是partial_amend/single/keep（60300ms）、022是rejected_rework/single/correction（60289ms），三者均为 `exit_code:null`，stdout/stderr为空。024正常exit0，025为exhausted_rework/multi/correction，仅有 `.command.json`。命令文件在spawn前写入，因此不能证明025成功启动、已经结束或C++停在哪一行。

原runs/outcomes和精确Python控制流将此次超时限定在 `setUpClass` 的 `run_fake` 循环；十个测试方法未开始，最终evaluator、report.json和evidence-sha256.json尚未生成。六项outcome中的三个COMPLETED只是runner执行标志，不能写成三项验收通过；另三个FAILED保留内部超时事实。子进程输出被捕获、setup没有阶段输出、cell超时先记录后继续，解释了CTest缺少即时方法/阶段信息，但没有定位C++耗时根因。正常class cleanup会删除临时目录及日志，属于取证保留缺口；本轮目录实际留存是观察事实，不能推导整个进程树退出或任意cleanup已完成。

该独立分析未执行产品、测试、编译或模型，也未打开/哈希数据库。17项输入、其中12项Git08仓库身份、10份源码/stdlib快照及99份冻结原日志有各自清单；本报告只读校验其 `SHA256SUMS` 的126项均匹配。analysis.md SHA为 `af3979bc13233c5092e0c5cdbc62076c430c3b67cf59276bee536537ed5be116`，completion-receipt.json SHA为 `53b39e037f8f1e7cd37d53ae2d42dac9880771f7fc3e1debe1f1881196930c64`。原99份日志保持私有，没有自动纳入公开补包。诊断报告末尾的 `REMOTE_CI_COMPLETE=false` 仅表示该诊断没有审核远端，不覆盖下述audit04对code08的独立结论。它不构成新的已确认生产功能缺陷或C++根因证明，不能将主机负载、库版本、ASan开销、死锁或此次字节复用作为已确立原因；原600.12秒CTest超时与所有历史OPEN继续保留。

**V3追加：远端成功与本机失败分别登记。** 精确08的已有远端独立审核03确认 [PR GCC ASan job 124744917757](https://github.com/wmqfl861/wls861/actions/runs/41271462706/job/124744917757) 的原92项通过，UID/GID1001、test umask0002、GCC13.3 Debug及detect_leaks=1，ctest exit0。该PR实际checkout为 `47534906df0402b20651b1971ac642c98aa1af9a` 合并验证提交，其base7dc/head08与相同tree关系按原API/源码产物核验保留；不得写成直接checkout08。此前Push/PR的strict、strict-umask0002、Clang18、UBSan八个作业的92/92也分别有原日志审核，见 `E/ci-08dad7e-audit-02/` 与 `-03/`。

后续 `/root/evidence_audit` 的 `E/ci-08dad7e-audit-04/` 最终原日志/artifact审核已于 `2026-11-01T05:06:44.028676Z` 完成，确认精确08的 **7个workflow、17个job全部success，其中12个Linux job原日志已在连续冻结审核中核验**。本V3据此登记 **REMOTE_CI_COMPLETE=true，仅适用于code08**。查询时间范围仍为workflow04:56:37、Linux jobs04:56:38及全部jobs04:57:54；旧audit02/03和V2的较早截点不回填为当时已完成。

[PR Linux](https://github.com/wmqfl861/wls861/actions/runs/41271462706) 与 [Push Linux](https://github.com/wmqfl861/wls861/actions/runs/41271460248) 各有6个成功作业。两份共享GCC ASan产物分别为13069509584和13071100488，均为原92项PASS、0 failure/error/skipped/notrun、ctest exit0，原测试名/期限保持；原ctest耗时分别1991.662654724秒和2317.010153091秒，794项source及61项binary/control清单前后一致并核对Git08。两端均GCC13.3 Debug、UID/GID1001、build umask0022、test umask0002和 `halt_on_error=1:detect_leaks=1`。PR实际checkout仍为上述merge475349，Push为head08；二者tree关系保留。两份Make-ASan也已在原日志和API中成功结束，不另计为两次92项CTest；其缺少独立数值exit/before-after inventory的范围限制保留。

另外五项成功workflow是 [PR Windows](https://github.com/wmqfl861/wls861/actions/runs/41271462756)、[Push Windows](https://github.com/wmqfl861/wls861/actions/runs/41271460257)、[PR fairness](https://github.com/wmqfl861/wls861/actions/runs/41271462726)、[Push fairness](https://github.com/wmqfl861/wls861/actions/runs/41271460270) 与 [source snapshot](https://github.com/wmqfl861/wls861/actions/runs/41271460295)。Windows/fairness此次只独立核验API成功元数据，没有追加方法级产物审核，不代替Windows11普通用户实机验收。源码产物794项的原字节/blob/mode/路径在audit03验证，本轮重哈希原ZIP并重核Git08映射。远端产物不含ELF载荷或完整LastTest，未宣称远端ELF重哈希或所有方法原始输出完整。audit04的review.md SHA为 `5b68f157ee3c3ab13427981dab4249bd1c16a562580a886553dc96901352aefb`，completion-receipt.json SHA为 `b469ef0f47de87da73f0159942453faf8caec895cef51525cfd06e8d271e13ab`。

主任务另已正常更新PR12进度正文并读回，原记录为 `E/pr12-progress-body-08dad7e-01/{before.json,body.md,edit-result.json,after.json}`；edit-result记录05:05:57.633284、exit0，after仍为head08、base7dc、Draft=true、OPEN。after.json SHA为 `3b848211adc13e5b2dfa9589412ed1d105f87731bf645532a81a82c84050f50f`。这次正文更新是audit04较早API字段比较之后的独立事实，不改写audit04的原比较时间，也不是提交本报告或正式批准PR。

这些远端成功与本机05:02:28快照中的七项Timeout属于各自真实运行，彼此不覆盖；七仍非本机最终失败总数。两种umask的Release通过、新本机失败及更早历史OPEN同时保留，原因未确立，整体本机矩阵尚未结束。UBSan/Clang等后续本机阶段须等串行驱动的真实收据，不从计划或远端绿色推断。尚未发布的本报告/补包及未来最终SHA需要独立CI核验，不能继承code08的远端完成状态。

**V3追加：晚到结果补包仅形成草案。** 新目录 `E/public-event-late-results-plan-01/` 保留原README、v1白名单/索引、两份局部观察和校验和，另增 `whitelist-draft-v2.json`、`index-draft-v2.json`、`UPDATE-v2.md` 与 `SHA256SUMS-v2`。v2草案分11组、索引91个明确候选路径，增加audit04最终远端收据和PR12正文读回；这些数字不是测试数或最终归档成员数。v2白名单SHA为 `f0ae8c84cfc8f2b7320d0ddd32b28e48b535561de9095c451666b62bcc2c95f0`，v2索引SHA为 `eb7c41bc4a24414c4ff53d8d0c2444d434815beba26b6621e789a80cb1a1d668`。v1原字节及其80路径截点保持；v2仍把官方GCC标作运行中、后续本机阶段标作待真实结果，刚冻结CLI分析尚需最终逐项选择和发布复核，不自动将其私有99份日志公开。

本轮**未创建最终补包**。两个已审ZIP及V2 patch/hash保持原字节；等待root提供最终收据后，新的独立补包须保留本机失败、每项实际退出/超时/未执行事实、原source/binary身份、各自完整日志，以及远端成功与最终源码审核的独立范围。缺少收据保持缺失状态，不补造完成；局部快照默认仅作本地草案来源。归档前仍需文本/路径/类型/秘密模式检查及非作者复核，原 `PR_REVIEW_COMPLETE=PENDING`、`MERGED=false` 和全部历史OPEN不变。

**V3状态截点。** 本次只追加已冻结分析和限定远端核验；本机失败观察沿用05:02:28的局部截点，没有重新复制正在增长的大日志。

| 状态 | V3的准确结论 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | **TRUE_IN_BOUNDED_SCOPE**，仍只对应code08的字节复用及手动探针。 |
| LOCAL_VALIDATION_COMPLETE | **FALSE / RUNNING_WITH_OBSERVED_TIMEOUTS**；最终本机矩阵、退出与after身份尚待真实收据。 |
| PUSH_COMPLETE | **TRUE_FOR_CODE08_ONLY**；本报告及新补包尚未发布。 |
| REMOTE_CI_COMPLETE | **TRUE_FOR_CODE08_ONLY**；依据audit04完成收据，未发布报告/未来最终SHA尚未验证。 |
| PR_REVIEW_COMPLETE | **PENDING**；限定工程审核不等于主协调者接受。 |
| MERGED | **FALSE**。 |
