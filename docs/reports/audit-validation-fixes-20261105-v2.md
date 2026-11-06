# 审计修复验证模块报告 v2（B-001 / E001 / F-01 / C-01）

- 报告版本：`audit-validation-fixes-20261105-v2`（一次性排他创建；本文件不再回写，后续新事实由新版本化报告/收据承载）。
- 生成方式：REPORT_ONLY 阶段（`report-implementation-01`）由全新 ZCode implement 会话按 FINAL v2（`plan-final-v2.md`，SHA-256 `3536c676cb03f2bfc03b06f1d04da520b06e55178c4dc25ed9740203b90505e7`）及同作者收尾/证据裁定创建。本阶段对仓库的唯一写入是排他新建本文件与同名 `.json`；未改任何源码/测试/构建/注册/CI/SQL/历史报告，未做任何 Git 写入。
- 事实时间范围：截至 2026-11-06T04:09Z（报告创建前最后一次只读核验）；远端 CI 事实截至各自取证收据时间（§6）。本报告不自称其未来提交 SHA 的 CI 或审核结论——按裁定由后续独立外部收据绑定，不制造自引用的未来 SHA 或批准。
- 证据引用约定：`EV/` 前缀为本模块私有证据根（本地数据盘 `.wls861-local` 下，不入 Git）中的相对路径；公开报告仅含安全事实、相对证据引用与哈希，不含私有原始日志、异常 marker、凭据或 /proc、PID 明细。
- 保留的 v1 阶段报告（不可变历史证据）：`docs/reports/audit-validation-fixes-20261105-v1.md`（SHA-256 `c78cfd7da4e32c460241100ba381679b22dcb7ce84d101fb14ac4c1db324411d`）、`docs/reports/audit-validation-fixes-20261105-v1.json`（`3cd040285373980d776ab6398c09fb3c66c0ca5592049653ec5ef07a1d549d67`）。本 v2 按 FINAL v2 校正其中口径（§2），不修改原件。

## 0. 摘要

四项确认缺陷（B-001 Gate stop 丢失唤醒、E001 验证 runner 孙进程收尾、F-01 公开错误携带私有路径、C-01 测试硬编码 /tmp）均完成最小修复并取得真实原版 RED / 候选 GREEN 证据。冻结验证叶 `b9cd31cd084b2411589128e7268d39eacac83741` 已通过发布前独立代码/证据硬审（PASS，范围限定，§6.1）；与 product 叶组成联合输入 `aa24f1dd02d4a6856aa812b83c64fc7b1407c4a7` 完成 11 项定向（11/11 通过，§4）与本机完整矩阵各环境终态（§5）。矩阵未全部通过：UBSan 原正式运行中断（86 终态 = 70 过/15 超时/1 失败；9 项补充补测 9/9 通过但不成一次正式 95），GCC ASan/LSan NOT_COMPLETE（67 过/27 压线超时/1 失败），原 Make-ASan 在首个失败测试二进制处中止（async_coordinator_tests 33 过/1 失败）；GCC Release umask0022/0002 与 Clang18 Release 为 95/95。外层 run-cmd 阶段回执的退出码/信号/正常路径清理字段经同作者裁定统一降级为 UNKNOWN（§5.6），原字节保留。`LOCAL_VALIDATION_COMPLETE=false`；最终报告头 CI 与独立结果硬审仍待后续外部收据。历史 Clang/live_workflow/Make-IPC/sanitizer 事项保持 OPEN。

## 1. 冻结身份与输入绑定

| 项 | 值 |
| --- | --- |
| 审计冻结基线 | `a5675e1245bebba41c499e56bd2c8a16be2d5dae`（tree `fac6f4451f8d95998ce9a22a0d228f33c582dc50`） |
| 验证叶（本模块） | 分支 `fix/audit-validation-20261105-adp35vg4`，HEAD `b9cd31cd084b2411589128e7268d39eacac83741`，tree `56048157bcf095b80b9ec7c13aa64ec362f09cab`，父提交 a5675e1；本报告创建时 porcelain 干净 |
| 联合输入 | 分支 `test/audit-core-integration-20261105-adp35vg4`，HEAD `aa24f1dd02d4a6856aa812b83c64fc7b1407c4a7`，tree `3a6fffc1c5017a6124207be61a8504d1cccc2fc3`；= a5675e1 顺序 cherry-pick product 叶 `0542d38dac86d51642d1095c77f7901b3de26c9e` + validation 叶 b9cd31cd；本报告创建时 porcelain 干净 |
| 联合输入清单 | `EV/core-integration-01/core-integration-input-v1.json`（`e443fabf906e847caea263526dc45b81eb35069a6a1224a04ed0dda39823bebe`）：23 个映射文件（product 9 + validation 13 + 受保护按 baseline_identical 核对）、9 个受保护文件全部与基线字节一致 |
| 叶冻结清单 | `EV/validation/local-freeze-01/frozen-input-v1.json`（`a254897777332c34b1ba3a46b122fc8e5c2fa4d0060366f815227580e2705cba`） |
| registry 锁定 | `tests/gcc_asan_public_methods.json`（整文件 SHA `75fde02a7b479c44acf29c19fc68b756723627596d7b6345efaac814d473e31b`）锁定四源并逐项哈希匹配（doctor_diagnostics_tests 32 方法、single_agent_tests 14、r24_recovery_predicates_tests 6、r24_concurrency_tests 7）；`EV/core-integration-01/registry-hash-binding-v1.json`（`674709d82def5b1baf49dc51b06dac39fe4f63612c6b21b73b86866bc6e93b67`） |
| 95 项目录 | 联合 configure 后 95 名字/95 deadline 与 `tests/gcc_asan_expected_tests.json`（`620fe97b1d0b502b26f6ffd64977e9e7292423b78283467fd49ffe2816427efd`）逐项一致（CATALOG_CHECK PASS）；静态标签抽取由 joint-targeted-01 prebuild 静态门禁覆盖 |

本模块 13 个改动文件（11 代码/测试 + 2 份 v1 报告，2026-11-06T04:09Z 实测，与冻结记录逐一相同）：

| 路径 | SHA-256 |
| --- | --- |
| `tests/async_coordinator_tests.cpp` | `73160b1b9c5f69a976974cc936b7e85f8c982b8e151a292ddee51913ef925d51` |
| `scripts/run_gcc_asan.py` | `46866558cb0865bd06a1367569cb8c96c968ad167c9fdc7874a59fc2f745f32f` |
| `tests/test_run_gcc_asan.py` | `5de8be03b37dc2fdc6cb0d321c8b03cfbd88ad8dee4d2702f51339db863584c8` |
| `scripts/evaluate_r3.py` | `e3f35aa2ff93de9181fbb7ab448914f1006bb0accf7a9ba55e82bfd52ebcfc2a` |
| `scripts/r3_offline_acceptance.py` | `11e447bc1126aaf71522c00cc27eb0eb1a08d2534219b26881a1e111acd3d6544` |
| `tests/r3_evaluation_tests.py` | `f589d87ea02ef80bcb49a8821f5cc57b4ba3a598f739af0b57f8447f0d00855c` |
| `tests/r3_offline_acceptance_tests.py` | `b9bda6d4eb2026e0daeb18a28589933997f60a3cc4b9bb7ddd256fb088e3112f` |
| `tests/r3_offline_acceptance_cli_tests.py` | `e618358b9a46ce968ed36b528082a30b85e1b5f83b54c268d2aeb3ae220886dc` |
| `tests/test_billing_baseline_e2e.py` | `c6365be4ef3450713935f258ee220a8e951532c7d214a230c7a97439ec46e42f` |
| `tests/sandbox_tests.cpp` | `e0dfc5cd1a558da1a579e3951c2aadbc27ed64af412b80f7692ced47613f12a1` |
| `tests/owned_capacity_baseline_repro.cpp` | `b57d8a7bd1f3766e7c5cc50fd9eb6455725e3070a51811d1ee85025f1e079199` |
| `docs/reports/audit-validation-fixes-20261105-v1.md` | `c78cfd7da4e32c460241100ba381679b22dcb7ce84d101fb14ac4c1db324411d` |
| `docs/reports/audit-validation-fixes-20261105-v1.json` | `3cd040285373980d776ab6398c09fb3c66c0ca5592049653ec5ef07a1d549d67` |

受保护文件（本报告创建时实测与基线一致）：`CMakeLists.txt` `2d696863…`、`Makefile` `8a3b6292…`、`.github/workflows/ci.yml` `d15850cc…`、`tests/gcc_asan_expected_tests.json` `620fe97b…`、`tests/gcc_asan_public_methods.json` `75fde02a…`、`tests/doctor_diagnostics_tests.cpp` `5fac599a…`、`tests/single_agent_tests.cpp` `597cd5c4…`、`tests/r24_recovery_predicates_tests.cpp` `b66c4d7b…`、`tests/r24_concurrency_tests.cpp` `a6e8d296…`；另有候选依赖的 `tests/test_support.hpp` `5b44c995a3c1a8e93774aa5d4f3a672a23d49571e34e6f45fd8ea197aad20922`（基线字节一致，未修改）。

**组合依赖与口径**：§4/§5 的定向与矩阵成绩属于联合输入 aa24f1dd（product+validation 两叶冻结代码），组合依赖 product 叶的 9 个文件；`leaf_standalone_tested=false`，不得把联合 95 项写成验证叶独立 95 项。验证叶上下文的独立执行证据是各 RED/GREEN 私有夹具、旧库链接局部检查与 E001/F-01 自检（§3）。

## 2. 对 v1 报告与早前自述的口径校正（原件不改）

1. **F-01 RED 等级**：`f01-red-driver-v2` 原版 exit 0 只打印泄露观测，不能单独记为成功行为 oracle 的 RED；`red-offline-suite-original-scripts` 确有 marker 的预期断言失败，但另含 4 个新 API/测试导入 ERROR，须分开列明；`red-oldstyle-outcomes-test` 仅 usage 失败，不是缺陷 RED。干净的同成功断言双输入 oracle 见 §3.3（`EV/validation/f01/oracle-02-v1/`）。
2. **耗时口径**：E001 自检 unittest 内部 37.179s、命令收据墙钟 38.200156582s；F-01 15 项 unittest 内部 3.307s、命令收据墙钟 8.506169648s。内部耗时不得称为完整墙钟。
3. **v1 Markdown 实际哈希**：`c78cfd7da4e32c460241100ba381679b22dcb7ce84d101fb14ac4c1db324411d`（实施 stdout 表格曾有抄写差异，以文件实测为准）。
4. **188 项索引**：implementation-01 证据索引共 188 条，其中 8 条为既有 plan-review 文件，不是本阶段新建 188 件。
5. **权限口径**：观察时点 8 个 Python 文件 stat 0664、2 份 v1 报告 0644；作者收据记录报告创建时 0600。按实际 stat 分时点记录，不虚构权限变迁、不擅自规范化旧文件。
6. **会话标识映射**：`implement-receipt-v1.json` 内 `CODEX_SESSION_ID=01a18545-80f9-7ee3-993f-0e49f338e3ac` 是进程环境标识，不是 ZCode session 或独立性证明；impl-01 权威会话为 `sess_cb431329-7507-4fc1-ba1c-466eb89bec14`（入口回执 `receipt-implement-5423c71d.json`，`b690a984a66d55d8407334891d6ec437486743187ff8a14fbbbbeb19afa84b5e`）。
7. **会话链**（均全新会话，无 resume）：计划审核 `sess_d14cd3b3-33d8-47d3-8c95-5d2f58a6c1a8`；实施 sess_cb431329…（impl-01）、`sess_dac41ed7…`（impl-02）、`sess_d08951be…`（impl-03）、`sess_d1f0f755…`（impl-04）、`sess_1cd40211…`（joint-targeted-02）；full-matrix-01 会话 UNKNOWN（无终态回执；工具会话 56991 不是 ZCode session）；`sess_de64dcb9…`（full-matrix-02）；首次硬审 `sess_a4270998-8056-4dd3-851e-d9b7bc0f94e8`；本报告阶段为全新会话（实际 ID 由入口回执记录）。impl-02 入口回执 conclusion=unknown（stdout 未含规定标记，模型自述“实施：已完成”）。
8. **外层回执退出/清理字段（最重要）**：full-matrix-01/02 私有 run-cmd helper（`EV/full-matrix-01/logs/run-cmd-01.py` `f31ab9c1…`、`EV/full-matrix-02/helpers/run-cmd-02.py` `630344e7…`；两版非逐字节相同，仅 docstring/usage 差异，有效逻辑相同）先 `os.waitpid(pid,WNOHANG)` 收割直接子进程，随后 `Popen.wait()` 在 CPython 3.12（`/usr/lib/python3.12/subprocess.py`，`baa9f913…`）遇 ECHILD 记 0。同作者裁定（`planner-adjudication-wrapper-exit-and-cleanup-evidence-v1.json`，`a29224f91123851c1e42d9de498f197b2559db8660e753ba59a874752f06bc27`）将 11 份受影响外层回执的直接子进程退出码/终止信号/正常路径组清理完成性统一记 **UNKNOWN**；stage 侧车里 `true_exit_code=1/2/0` 的表述被撤回；`cleanup_complete=true` 与空成员数组是初始化默认值（正常路径无组普查、escaped_members 无填充逻辑），不构成无遗留证明。原 meta/log/侧车/回执字节一律不改。
9. **Make 结果计数**：原 stdout 共 16 条 RESULT——**前 15 条 failed=0，第 16 条 `33 passed, 1 failed`**；不得写“16 个成功二进制”或“前 16 条全过”。`Error 1/Error 2` 是 make 自身日志文本，非顶层进程原始 waitstatus。

## 3. 四缺陷分列

### 3.1 B-001 · Gate 的 stop 通知竞态（P2）

- **原证据等级**（审计 B_workflow，`a362c1ff73eb1f0dfd10769743cd8a1009a9a2280b3e04ed059a531056600cd14`）：原代码 STATIC_CONFIRMED（`tests/async_coordinator_tests.cpp:68`，a567 版 `74336e545626fe95395f8f03076e34ce09ac274a691fb948003ecb3e47c5a4b8`）；等价等待算法确定性 probe REPRODUCED（`stop_requested=1 returned_on_original_stop=0 rescued_by_explicit_release=1`）。不是原 CTest 失败或历史超时的根因证明。
- **修复**：`condition_variable` → `condition_variable_any`；respect_stop=true 走标准 stop-aware `wait(lock, stop, predicate)`（谓词只判 released）；respect_stop=false 走普通 `wait(lock, pred)`，stop 不放行；删除裸 notify 的单独 stop_callback；在原 Gate 内增加默认未设置的一次性 `before_wait` 测试 seam（谓词捕获 false 之后、返回 wait 之前，移动后清空）。保持 entered 计数、release 互斥协议、生产 callback/close 语义与原 180s 时限；仅测试文件，不触生产调度器。
- **RED**（`EV/validation/b001/red-b001/`）：`apply_b001.py red` 得源快照 `a38113dba04ba79e68346845c11073a3c96dbc7b8366907f6301bbee8abc0302`（旧算法+seam+3 个真实 stop 测试），链接已核验旧 Release 库（`libwls_s1.a` `6cf0984c28956d3c5a4d18df0ac1e5d1385cd77bce31d25b6dc9aa9d8207c9ff`、`libwls_kernel.a` `a780803a40cb241a40454fad86b7ec19da7e28babb66110aa49ce6f0f2e1767f`，f614→a567 等价性重核含源/头/flags；联合 Release 构建中 libwls_kernel.a 逐字节复现同哈希）得 ELF `58a49676…`。运行 exit 1、墙钟 44.5s（240s 看门狗未触及）：34 例中唯一失败 = `stop between false predicate and wait cannot lose its wakeup` 的 `CHECK(completed_from_stop)`——确定性丢失唤醒窗口；断言前已无条件 release+join+取回 future，非超时击杀。
- **GREEN**（`EV/validation/b001/green-01/`）：等待算法最小修改（`green-vs-red.diff` 仅 Gate 算法变化，seam/测试字节相同），当前叶源 `73160b1b…`，同输入 34/34 全过（含 `shutdown_joins_and_restart_preserves_unknown`、`resource_waiters_close_records_original_unknown`）。
- **后续环境事实**（联合输入）：定向 async_coordinator 38.92s PASS；Release×2/Clang 95/95 含 async；GCC ASan 中 async_coordinator 于 180.42s 压线超时（timeout_at_deadline，原因未判定）；Make-ASan 中 async_coordinator_tests `RESULT 33 passed, 1 failed`——失败用例 `cross-pool resource heads progress before slot admission`（`tests/async_coordinator_tests.cpp:191` `checked_at < deadline`，原 5 秒内部等待）导致 make 序列中止（§5.5）。上述失败按事实记录，原因 UNKNOWN，未关闭；RED33/1→GREEN34/34 只证明 seam 窗口缺陷与修复，不追认为历史超时根因。

### 3.2 E001 · 中断后仅等待直接子进程，同组孙进程可残留（P2）

- **原证据等级**（审计 E_platform_build，`b6a68dd7ca54f33fd5872216ba1850190c403d66afc6fb7ec718d504609a5b8b`）：真实 Python 父/孙进程树 REPRODUCED——孙忽略 TERM，父 exit -15 后孙仍 S/live，Recorder 只 wait 父；原中断仍 NOT_COMPLETE，不是伪造 PASS 问题。原 runner a567 版 `70f860bd3e0952a8620ce958a17c66d70e3d8ccd0405cc41a7492abb1aed6e14`。
- **修复**（`scripts/run_gcc_asan.py` → `46866558cb0865bd06a1367569cb8c96c968ad167c9fdc7874a59fc2f745f32f`）：记录 owned leader PID/PGID/session/UID/starttime（限定 `pid==pgid==session` 本次身份）；主等待用 Linux `os.waitid(P_PID,…,WEXITED|WNOHANG|WNOWAIT)` 观察直接进程终止而不提前回收（僵尸 leader 锚定组标识防复用）；仅对已验证拥有的组 `killpg`，TERM 5s + KILL 5s 有界收尾证明预算（不是测试 deadline；外部期限/预算耗尽即 NOT_COMPLETE）；组收尾结束才 `Popen.wait` 取原始 returncode；收尾失败 fail-closed；`preserve_ctest_evidence` 与成功路径显式消费 `cleanup_complete`。公开 summary/step 投影保持原字段白名单，`leader_reaped`/`member_exit_observations`/`remaining_live`/`cleanup_complete`/组身份/adopter 信息只进私有收据，不输出 /proc 路径/PID 明细。
- **RED/GREEN**：`EV/validation/e001/red-01/`（真实父先退出、孙仍 live，成功行为 oracle 明确 exit 1）；`EV/validation/e001/green-01/` 完整自检 68 方法 OK（60 原有 + 8 新增，含真实孙进程 oracle、TERM 可退出、父 exit0 留活成员、启动失败无信号、二次中断、观测失败、stat 解析、preserve 消费）。耗时口径见 §2.2。自检不是产品 ASan 成绩。
- **后续使用**：该冻结 runner 即 full-matrix-02 GCC ASan gate 所用（自检按“输入哈希完全一致可绑定复用”引用 e001/green-01，不重跑）；其内层 CTest 证据（Recorder owned-process-group 收尾、waitid(WNOWAIT)+process.wait）与 run-cmd helper 的缺陷无关，保留各自范围（不外推为全机后代退出证明）。

### 3.3 F-01 · 公开错误 reason 携带私有数据库绝对路径（P2）

- **原证据等级**（审计 F_tools_contracts，`f44f86d8dd27d4d1d9f2e6663715d18fb4b8a5a45c412e400e5a48cd2d85471d`）：真实 `build_report` + 合成缺文件路径 REPRODUCED——invalid_evidence.reason 含客户标识 marker 的绝对路径；十格分母仍正确。原 evaluate_r3.py a567 版 `770d2200a9086e53552c39458d0f57e880843b89ed45925a7994d9679d73348e`。
- **修复与公开/私有投影契约**（`scripts/evaluate_r3.py` → `e3f35aa2…`、`scripts/r3_offline_acceptance.py` → `11e447bc…`）：公开 reason 一律稳定原因码——`InvalidEvidence` 仅从已审阅常量消息→固定码精确映射，未知消息回退 `invalid_evidence`；OSError 按有限 errno 分类（`evidence_missing`/`evidence_unreadable`/`evidence_io_error`），SQLite/Key/Type/Value/未知异常各有固定 fallback；不以 str(error)、类名、slug 或正则删路径保留文本等方式绕回公开输出。四出口（build_report invalid 行、evaluate CLI 顶层 stderr、offline CLI 顶层 stderr、离线包 `runner_outcomes`）全部走投影；`runner_outcomes` 从私有 outcomes 逐行显式投影（task/condition/status 有限白名单，未知 status→UNRECOGNIZED 永不 COMPLETED，reason 白名单→`runner_failed` 回退，`outcome_sha256` 仅哈希引用），原 outcomes 字节不变。私有保全：`run_fake` 上下文内部可选 `private_error_sink`（O_EXCL/0600、内容 hash/固定 ID 关联、写失败不回显路径）；纯 `report/build_report` 默认只读不落盘，可返回标明为 fingerprint 的 `error_fingerprint_sha256` 与明确未保留状态，不虚称存在文件。不改十格分母、原数据库只读事务、nullable usage、synthetic/provider 来源、v1/v2/v3 canonical 算法；历史报告不重算。
- **干净双输入 oracle**（`EV/validation/f01/oracle-02-v1/`，补齐 v1 缺口）：从 Git 基线只读导出 a567 原版 `770d2200…` 与当前候选 `e3f35aa2…`；同一 driver（`5d08c3044b914f683ec1097c892f3ad8e9f46ce5dfaf7cbaf005a8c53340c9bc`）、同一 plan fixture（`f938f2cb5784726d5c4880ead21d292959bf7453d3b0ce719bdddb8d657e8b3c`）、同一成功断言集。**基线 RED**：exit 1，断言精确失败于“公开 canonical 不含 marker”；**候选 GREEN**：exit 0，counts={invalid_evidence:1, not_executed:9}、expected_cells=10、row0_reason=`evidence_missing`。`build_report` 仅位置参数调用，原版无兼容问题。
- **测试与执行**：`tests/r3_evaluation_tests.py` 11 处、`tests/r3_offline_acceptance_cli_tests.py` 3 处 reason 断言改确定码；`tests/test_billing_baseline_e2e.py` 恰两处 reason 匹配改动（TLS/签名/预算/期限/业务断言不动）；`tests/r3_offline_acceptance_tests.py` 新增纯 Python 安全投影/旧 outcomes/私有 sink 故障自检（15 项 OK，3.307s 内部/8.506169648s 墙钟）。联合定向全套执行：r3_offline_acceptance 2.98s（30s）、r3_offline_acceptance_cli 76.62s（600s）、r3_evaluation 80.51s（180s）、billing_baseline_e2e 248.51s（900s，受影响完整回归，非只跑末两方法）。只有 synthetic+loopback TLS，无真供应商/付费 API。

### 3.4 C-01 · 两处测试硬编码 /tmp，绕过数据盘 TMPDIR（P3）

- **原证据等级**（审计 C_execution + 非作者复核，`8e6276f2a6607cbb4620a3a60576677333297b95b7d32e4ec79bfe61d90d18d5`）：STATIC_CONFIRMED（`tests/sandbox_tests.cpp:18-28` 直接 `/tmp/wls-s12-sandbox-test-XXXXXX`；`tests/owned_capacity_baseline_repro.cpp:11-15` 固定 `/tmp` 模板）。本轮升级为可执行位置违规观测（下）。
- **修复**：`sandbox_tests.cpp::Temp` 复用 `test::TempDir`（保留 root 成员接口、删除拷贝、无 64 字节固定 buffer）→ `e0dfc5cd…`；`owned_capacity_baseline_repro.cpp` 以 TempDir holder 替换字面模板，mkdtemp 失败仍 `return 8` 早退语义保留 → `b57d8a7b…`；`tests/test_support.hpp` 未改。不改生产 sandbox/owned 容量实现、不新增注册。
- **位置 RED/GREEN**（`--wrap=mkdtemp` observer，仅限 fixture，绝不用于产品门禁；`EV/validation/c01/`）：原版两处逐字节抽取构造器请求 literal `/tmp` 模板，observer 在真实 mkdtemp 前拒绝（`REJECTED_OUTSIDE_PRIVATE_ROOT`），未在 /tmp 创建任何内容；候选仅发 ALLOWED 于私有数据盘根（observer 日志 `cd6e45f4…`/`ee1faa38…`，ELF `2c5892a1…`/`0ab3d894…`）。
- **共同成功 oracle**（`EV/validation/implementation-04/oracle-01/`，`oracle-manifest-v2.json` `b0edad4e62b4afdc9551e8befb61e7c82e92228477b5bdb92be8e3261c05648e`，all_checks_pass=true）：同一 driver 字节、同一成功断言（子进程 exit 0 + observer 恰两条记录且均 ALLOWED 并按目录边界位于本轮独占 TMPDIR + 运行后零残留），对两个已编好 ELF 各执行一次——原版 RED（oracle exit 1，唯一失败断言 `records_allowed_and_inside_tmpdir`，child 0 事实先行保全）；候选 GREEN（exit 0，两条 ALLOWED 位于 176 字节含空格独占 TMPDIR，0700，零残留）。`WLS_C01_ALLOWED_PREFIX` 取规范化 TMPDIR+目录分隔符，拒绝同名前缀兄弟目录；抽取↔仓库映射含字节级一致证明（含 owned holder 语义映射，早退保留、/tmp 字面量不存在）。
- **全套件与容量语义**：impl-03 候选 sandbox（19 方法）对旧库严格编译，205 字节含空格 TMPDIR 下干净运行 10.1s 19/19、observer 链接运行 4.8s（9 次 mkdtemp 全 ALLOWED，stdout 哈希与干净运行相同）、零残留；联合定向 run B 完整 sandbox 19/19 PASS（158 字节含空格数据盘 TMPDIR，LastTest 原件核对）；owned_capacity/owned_capacity_runtime 在 run A 通过；`owned_capacity_baseline_repro` 不属 95 项注册，仅证明临时根修改与可编译，历史容量 probe 语义按原件保留（`REPRODUCED … inventory_error=quota` 记为历史 probe 含义，不重释为今日产品行为）。原 /tmp 历史日志保留不动。

## 4. 定向验证（联合输入，GCC13.3 Release）

来源：联合 aa24f1dd 专有 Release 构建（g++-13 13.3.0、`/usr/bin/python3` 3.12.3、compile_commands）；configure 后 95 名字/95 deadline catalog 核对通过。标记 `validation_under_product_plus_validation_input`。

- **run A（10 套件，一次 CTest 调用，总 527.32s）**：r3_offline_acceptance 2.98s/30s、r3_offline_acceptance_cli 76.62s/600s、config 0.01s/90s、scheduler 0.01s/90s、owned_capacity 39.37s/90s、owned_capacity_runtime 22.51s/180s、async_coordinator 38.92s/180s、persistent_event_runtime 17.86s/180s、billing_baseline_e2e 248.51s/900s、r3_evaluation 80.51s/180s——全部 Passed（JUnit 根属性 tests=10 failures=0；`EV/validation/joint-targeted-02/logs/junit-targeted-10-v1.xml` `9be0087b…`）。
- **run B（sandbox 单独，C-01 spaced TMPDIR）**：19/19 Passed，5.21s/90s，TMPDIR 158 字节含空格、0700、数据盘私有根、零残留（`junit-sandbox-spaced-tmpdir-v1.xml` `2d98eb56…`；LastTest 原件 `lasttest-run1-v1.log` `645389e5…`）。
- **合计 11/11 通过**（含 B 条要求的 config 相关回归）。证据：`EV/validation/joint-targeted-02/evidence-joint-targeted-02-v1.json`（`76c80bb04505ae0b5ea35701e026b3eab03d06121c5c529feeacd01042176169`）。
- **证据缺口（如实保留）**：run A 的 LastTest 在 run B 前被 CTest 覆盖，未封存（方法级原始输出 9/10 UNKNOWN/NOT_RETAINED；9 个 system-out 含 CTest 1024 字节截断标记，config 保留全量 `RESULT 5 passed`）；现存 lasttest-run1 仅属 run B。suite 级通过由 JUnit+stdout+meta 支撑，不声称方法级日志逐行核验。构建 `--parallel 1`（内存受限路由指令，显式记录）。

## 5. 本机完整矩阵（同一冻结联合 SHA aa24f1dd；逐环境独立编号日志）

| # | 环境 | 结果 | 关键原件（EV/ 相对路径，SHA-256） |
| --- | --- | --- | --- |
| 1 | GCC13 Release umask0022 全 95 | **95/95 通过**（JUnit 95 cases/0 failures；LastTest 95 Test Passed+End testing；stdout 100% tests passed；总 714.41s） | `full-matrix-01/logs/junit-release-0022-v1.xml` `22098aad…`；`lasttest-release-0022-v1.log` `a4d78f73…` |
| 2 | GCC13 Release umask0002 全 95（同一批 ELF） | **95/95 通过**（688.69s；运行后 ELF 身份与运行前快照一致：wls `6330616f…`、libwls_s1.a `5bc7ee89…`、libwls_kernel.a `a780803a…`、async `d5b013af…`、sandbox `63766406…`） | `junit-release-0002-v1.xml` `b03aa17a…`；`lasttest-release-0002-v1.log` `7b155f36…` |
| 3 | Clang18 Release umask0002 全 95（独立构建，parallel 1，构建 1004.87s） | **95/95 通过**（1439.78s） | `junit-clang-0002-v1.xml` `87c5f337…`；`lasttest-clang-0002-v1.log` `2ec79443…`；62 项产物身份清单 `binary-identities-clang-01.txt` |
| 4 | UBSan Debug 全 95（原正式运行） | **INTERRUPTED**：86 终态 = 70 过/15 超时/1 失败；#87 启动无终态；中断原因/会话/退出 UNKNOWN。1 失败 = persistent_event_runtime（`two_requests_two_nodes_advance_without_callback_wave_barrier` 超原 5s 等待，`checked < deadline` @ persistent_event_runtime_tests.cpp:262；RESULT 16 过/1 失败）。15 超时：event_runtime、acceptance_runtime、audit24_capacity_repro、r2_rework、r2_amend、sse_replay、r1_management、persistent_event_store、live_workflow、billing_baseline_e2e、r3_evaluation、s13/s14/s15/s16_cli_smoke | 原现场保全 20 件（`interruption-recovery-01/preservation-v1.json` `e57787a8…`）；原 Testing/Temporary、LastTest.log.tmp、Checkpoint 未动；原 JUnit/终态回执缺失 |
| 5 | UBSan 九项补充（原二进制，新私有 driver） | **9/9 通过**（独立补测标签 `UBSAN_UNFINISHED_REGISTRATIONS_SUPPLEMENT_01_ONLY`，264.16s；driver 机械复制 95 条原定义、仅显式补 WORKING_DIRECTORY，show-only 核对恰 9 名/原 deadline；UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1）。不成一次正式 95/95 | `full-matrix-02/logs/junit-ubsan-supplement-01.xml` `9741188a…`；`lasttest-ubsan-supplement-01.log` `b9b7330d…`；`evidence-ubsan-supplement-01.json` `5ce1ecb4…` |
| 6 | 共享 GCC13 Debug ASan/LSan 全 95（冻结 runner 46866558…） | **NOT_COMPLETE**：内层 CTest exit 8（独立记录）；95 例 = 67 过/27 压线超时/1 失败；失败 = persistent_event_runtime 103.99s<180s（作者只读复核：14 过/3 失败方法，原 5s 等待断言，原因 UNKNOWN；与 UBSan 中断运行失败的是同一套件）。证据收集 complete、源/HEAD/二进制前后不变；私有+公开证据包整树备份 | `asan-gate-01-private-evidence/summary.json` `29a38ce3…`（status=NOT_COMPLETE）；`ctest.step.json`（exit 8）`d7bc8164…`；`ctest.xml` `65414618…`（95/28 failures）；`LastTest.log` `24165066…`；`evidence-asan-gate-01.json` `3747478a…` |
| 7 | 原 Make-ASan（`make -j2 asan … CPPFLAGS=-DWLS_CI_USER_CPPFLAGS=1 PYTHON=/usr/bin/python3`，umask0002，实际生效目录 `…-asan`） | **在首个失败测试二进制处中止**：构建与 verify/contract 检查通过；16 条 RESULT = 前 15 条 failed=0 + 第 16 条 async_coordinator_tests `33 passed, 1 failed`（失败用例 `cross-pool resource heads progress before slot admission`，`async_coordinator_tests.cpp:191 checked_at < deadline`，原 5s 内部等待）→ make 日志 `Error 1`→`Error 2`（观测文本，非原始 waitstatus）；其后测试二进制 NOT_RUN，无完整 Make-95 结论 | `full-matrix-02/logs/make-asan-01.stdout` `4bde5d3e…`；`.stderr` `6ba4927b…`；`evidence-make-asan-01.json` `1aa7fc0f…` |

补充说明：

- 27 项 ASan 压线超时名单（秒数见证据）：acceptance_runtime、r2_rework、r2_amend、sse_replay、r1_management、owned_capacity、owned_capacity_runtime、single_agent、single_agent_runtime、async_coordinator、persistent_event_store、live_workflow、billing_baseline_e2e、r3_evaluation、s1/s12/s13/s14/s15/s16_cli_smoke、r1_history/r1_structured/r1_window_cli_smoke、r2_rework/r2_recovery/r2_integration/r2_amend_cli_smoke。均为 timeout_at_deadline 分类，原因未判定，未关闭。
- 方法/子例计数与 95 项 CTest 注册分开：如 sandbox 19 方法、async 34 方法、Make 序列 RESULT 行是测试二进制粒度；不把方法数与 95 注册项相加。
- Make 事后关键产物哈希（wls `06371dd9…` 等）为构建后哈希，不构成运行前二进制身份证明。
- full-matrix-01 的 `build-release-full-01.meta.json` 为 0 字节空文件，不是有效回执；原 UBSan `ctest-ubsan-0002-v1.meta.json` 缺失，其退出/信号/中断原因 UNKNOWN。
- 资源与回收：矩阵阶段盘余最低约 2.0GiB，未触发资源回收；两棵阶段自有构建/测试树（ASan build、Make-ASan build、UBSan driver）按清单保留（`full-matrix-02/matrix-recovery-stage-summary-01.json` `608180e1…`），未删除任何历史证据。
- **§5.6 外层回执限定（适用全部 11 份 run-cmd 外层 meta）**：#1–#3、#5、#7 及构建/configure 步骤的外层 `exit_code=0/signal=null/cleanup_complete=true` 一律按 §2.8 降级为 UNKNOWN；上表结果只以被裹程序自身的权威原件（JUnit/LastTest/runner summary/make 日志）为准。GCC ASan 的内层 CTest/Recorder 证据来自冻结 runner 的独立机制，保留其原有范围。

## 6. 发布、CI 与审核（报告前事实）

1. **首次独立代码/证据硬审 PASS**：product（`sess_8917d049…`）与 validation（`sess_a4270998-8056-4dd3-851e-d9b7bc0f94e8`，`EV/validation/hard-review-01/hard-review-20261105T152856Z-37a6dc.stdout.json` `8ab9f256…`，回执 `45f2e516…`）均明确 PASS；范围＝发布前冻结代码 + 真实 RED/GREEN + 联合定向证据 + 组合依赖 + 披露的过程偏差，不含完整矩阵/最终 CI/合并。指定实施回执 joint-targeted-02 缺 `plan_review_session_id` 原始字段，由 sidecar 补充（原件未改）。
2. **联合分支发布**：实时核验 PR #36/远端 base 后普通 push `test/audit-core-integration-20261105-adp35vg4` = aa24f1dd（`EV/core-integration-01/publication-01/push-v1.json` `67f30cfa…`）。CI：push 事件 run 42058523307，head aa24f1dd，六个 Linux lane（strict、strict-umask-0002、Clang-18、UBSan、ASan、Make-ASan）全部 success；source-snapshot run 42058523351 归档 969 条路径/类型/模式/字节逐一与 git archive 核验一致（`verification-v1.json` `a354b5f7…`）。
3. **验证叶发布**：普通 push `fix/audit-validation-20261105-adp35vg4` = b9cd31cd（`EV/validation/publication-01/push-v1.json` `09bcb015…`）；Draft PR #41 创建成功（base `test/n5-persistent-workflow-20261104-_7wxnkyn` @ a5675e1，`pr-create-v1.json` `a039f15f…`）。CI：push run 42058508989（head b9cd31cd）六 lane success；PR 事件 run 42058840265（合并提交 checkout `509151da…`，tree 与叶 tree 相同）单独登记，不冒充 push 头成绩；source-snapshot run 42058508932 归档 967 条核验一致；六个 lane 的 checkout 日志确认实际检出 SHA（`remote-results-01/checkout-identity-v1.json` `82cabb67…`）。PR #36（base 依赖）与 PR #40（product 叶）状态见 `EV/core-integration-01/report-readiness-01/ready-v1.json`（`3dcca844…`，2026-11-06T03:43Z：三 PR 均 OPEN/Draft、head 符合预期、报告路径不存在）。
4. **绑定边界**：上述 CI 属报告前代码头（b9cd31cd / aa24f1dd）。本报告提交产生的最终 leaf HEAD 的 CI 与 source-snapshot，以及最终独立结果/证据硬审，由后续外部收据绑定；新 CI 绿色不自动关闭任何历史 OPEN 状态。

## 7. 过程偏差全列表（均保留原件/失败尝试，未伪造证据）

1. impl-01：一次 sed 对比在 `/tmp` 误建空文件 `/tmp/x`，同会话自述删除；删除内容无法独立证实（登记为偏离，非授权 /tmp 使用）。
2. impl-03：`stage-evidence-manifest-v3.json` 同路径重写一次（首稿未保留，首版字节/时刻 UNKNOWN，最终字节 `bc547923…` 已哈希）；越界写入 harness 持久记忆一行（~/.zcode/…，超出模块 allowlist 与数据盘证据根；后续任务不再主动更新全局记忆，不回滚全局文件）。
3. impl-03 夹具首版两个编译缺陷（observer 保留字参数名/缺 extern "C"、extract 流式输出 perms）→ 新版本文件；失败运行保留于 `c01/fixture-run-01/02`。
4. impl-04：候选首跑 `--expected-source-sha256` 手抄掉一字符（63 hex）被身份门禁拒绝（infra exit 3，原件保留）；`oracle-manifest-v1.json` 两个手抄期望常量错误 all_checks_pass=false（保留），v2 改为程序化取值后全过。
5. joint-targeted-01：早期无 slot BLOCKED，后获 slot 继续；prebuild 静态门禁 CMake 哈希一字符誊写错误，MISMATCH 日志与 `verify-identity-03` 更正并存（历史文件未改，权威清单三方一致 `2d696863…`）。
6. joint-targeted-02：run A LastTest 未封存被 run B 覆盖（NOT_RETAINED，§4）；9/10 JUnit system-out 被 CTest 1024 字节阈值截断；构建 `--parallel 1`（内存受限）；两次 CTest 调用（隔离 C-01 spaced TMPDIR，不减套件/断言/时限）。
7. full-matrix-01：UBSan 正式运行中断（原因/会话/退出 UNKNOWN；86 终态如实保留）；`build-release-full-01.meta.json` 0 字节；原 UBSan JUnit/CTest meta 缺失。
8. full-matrix-02：intake 脚本两次失败尝试（01/01b，解析缺陷修正为 01c，三次输出全保留）；run-cmd-02 外层退出码捕获缺陷（沿用上阶段同语义）→ 侧车 `evidence-exitcode-capture-defect-01.json`（`b72bfda8…`）+ 同作者裁定统一降级 UNKNOWN（§2.8）；退出码缺陷最小复现的 3 个探针临时文件记录后删除，原件未留存（仅实施者自述，不称独立动态复现原件）；runner 自检复用而非重跑（裁定允许，绑定记录在案）。
9. impl-02 入口回执 conclusion=unknown（stdout 缺规定标记）；joint-targeted-02 回执缺 `plan_review_session_id` → sidecar（原件未改）。
10. 早前作者/阶段消息中“GCC 外层 exit 0 为实测退出码”“UBSan 补测外层 exit 0/cleanup true 已证实”“true_exit_code=1/2”等表述由 wrapper 裁定明确撤回（正确口径：原 metadata 写 0/true，有效直接子进程退出/信号/正常路径清理 UNKNOWN）。

## 8. 门禁状态（独立字段，不以部分通过代填）

| 字段 | 值 | 说明 |
| --- | --- | --- |
| IMPLEMENTATION_COMPLETE | true | 四缺陷修复 + 真实 RED/GREEN 完成；叶冻结 b9cd31cd |
| TARGETED_VALIDATION_COMPLETE | true | 联合输入 11/11（含 config 回归）；标记 validation_under_product_plus_validation_input |
| INDEPENDENT_CODE_REVIEW_COMPLETE | true | 首次发布前代码/证据硬审 PASS（sess_a4270998…）；范围不含最终矩阵/CI/结果 |
| LOCAL_VALIDATION_COMPLETE | **false** | UBSan 正式运行 INTERRUPTED（70/15/1）；GCC ASan NOT_COMPLETE（67/27/1）；Make-ASan 失败中止；失败原因均未关闭 |
| PUSH_COMPLETE | true（代码头） | b9cd31dd 叶与 aa24f1dd 联合分支已推 origin；Draft PR #41 已建。本报告的 report-only 提交/推送为后续根路由事项，不在此字段内虚报 |
| REMOTE_CI_COMPLETE | false | 代码头既有六 lane CI 绿（§6）；最终报告头 CI 待外部收据 |
| INDEPENDENT_RESULT_REVIEW_COMPLETE | false | 待最终矩阵/最终 SHA CI 事实齐备后的全新独立会话 |
| PR_REVIEW_COMPLETE | false | 代理硬审不等同正式 PR 审核 |
| MERGED | false | 无合并授权，任何 PR 不得合并 |
| leaf_standalone_tested | false | 定向/矩阵成绩属联合 aa24f1dd 组合 |
| historical_clang_asan_failures_closed | false | 不因新 CI/新证据编辑为 true |

## 9. 保留的开放状态与非声明

- 历史 Clang/live_workflow/Make-IPC/sanitizer 问题保持 OPEN；strict WAL/SHM 兼容 BLOCKED_NOT_COMPLETE；Windows image 冲突 UNRESOLVED；Windows 11 普通用户 NTFS NOT_RUN；私有原件托管 BLOCKED。
- 无真实 WLS 模型/供应商/付费 API 调用（synthetic+loopback TLS）；工程 ZCode 调用产生了模型服务请求，不宣称零工程模型调用。
- persistent_event_runtime 在 UBSan（中断运行）与 GCC ASan 两个独立环境均失败、async_coordinator 在 ASan 压线超时并在 Make-ASan 失败于原 5s 内部等待——均为记录在案的真实信号，原因 UNKNOWN，未归因、未关闭。
- 本报告只汇总已发生事实；计划能力、已实施接口不作为已完成验收证据。原始审计输入：`AUDIT-REPORT-v1.md`（`6b3fab12f69bd6b6752699ef1cc4adad695aff0618d8a75f68858ccc00f358de`）及 B/C/E/F 分项（哈希见 §3）。

## 10. 关键证据索引（EV/ = 模块私有证据根相对路径）

| 证据 | 路径 | SHA-256 |
| --- | --- | --- |
| FINAL v2 / 记录 | `EV/validation/plan-final-v2.md` / `planner-record-final-v2.json` | `3536c676…05e7` / `9a787715…ab79` |
| 同作者裁定（收尾/结果审） | `EV/validation/planner-adjudication-final-report-and-result-review-v1.json` | `ba7990c10b4d6b90d46651fa073fde37772daefb853b418e9d5c5fe16a75f00b` |
| 同作者裁定（外层回执 UNKNOWN） | `EV/validation/planner-adjudication-wrapper-exit-and-cleanup-evidence-v1.json` | `a29224f91123851c1e42d9de498f197b2559db8660e753ba59a874752f06bc27` |
| 同作者裁定（中断恢复/资源CI/日志缺口/C01） | `…interrupted-matrix-recovery…` / `…core-matrix-resource-and-ci…` / `…joint-targeted-log-gap…` / `…c01-and-deviations…` | `f03d90fa…` / `be44929b…` / `ea11c9ba…` / `692bb6be…` |
| 实施阶段回执 | impl-01 `b690a984…`；impl-02 `86cc27a4…`；impl-03 `cb1a3e1b…`；impl-04 `ce257c28…`；joint-targeted-02 `52d9a3ea…`；full-matrix-02 `1b931604…` | 见 `EV/validation/` 各阶段目录 |
| RED/GREEN 原件 | `EV/validation/b001/{red-b001,green-01}/`、`e001/{red-01,green-01}/`、`f01/{red-01,green-01,oracle-02-v1}/`、`c01/`、`implementation-04/oracle-01/`（manifest `b0edad4e…`） | 各目录收据内逐件哈希 |
| 定向/矩阵 | `joint-targeted-02/evidence…` `76c80bb0…`；`full-matrix-01/evidence-*`；`full-matrix-02/evidence-*`；`full-matrix-02/matrix-recovery-stage-summary-01.json` `608180e1…` | 本报告 §4/§5 表内逐件 |
| 中断保全 | `interruption-recovery-01/preservation-v1.json` | `e57787a8b8c027beb1db19522308c1bd7e742d142508dad80d6b072480a5905b` |
| 硬审/机械核对 | `hard-review-01/hard-review-…stdout.json` `8ab9f256…`；`receipt-hard-review-2fba7318.json` `45f2e516…`；`mechanical-check-v1.json` `2c45d54e…`；`candidate.diff` `3698b8f7…`（恰 13 文件） | `EV/validation/hard-review-01/` |
| 发布/CI | `publication-01/*`；`remote-results-01/{42058508989,42058840265,checkout-identity-v1}`；`EV/core-integration-01/{publication-01,remote-results-01/42058523307,report-readiness-01}/` | §6 所列哈希 |
| 本阶段核验 | `EV/validation/report-implementation-01/report-input-verification-v1.json` / `implement-receipt-v1.json` | 随本报告生成，见阶段收据 |
