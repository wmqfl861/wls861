# 审计修复报告 v2：ASTRA-A-001 与 D-CLI-001（product 模块，2026-11-06）

本报告由 report-implementation-01（REPORT_ONLY）阶段排他创建（2026-11-06T04:02:00Z 前后），仅汇总截至
2026-11-06T03:56Z 输入（矩阵恢复阶段已于 2026-11-06T02:09Z 到达终态）的真实已发生事实。计划输入：
`plan-final-v2.md`（SHA-256 `81c40cc3a1de83f1e7d23465a24eb6168f764697151c3f318f3c60718134688f`）与
`planner-record-final-v2.json`（`cbea48bf649b1d77472b1655e3d1b6c3d6946599b7af29e3ccebb24a72e94f62`），
本阶段实测一致；WORKFLOW SHA-256 `f49ff366cacb15df96ade89e4aa3d4412cb50836e4f11f3be407713a844bf990`。
v1 报告（`audit-product-fixes-20261105-v1.md` = `fbac95508a798385bb3cc3e7525a22c3777fd4cbf85913eac87d0883eda0a2bf`、
`.json` = `d632822887a3ac533e1faa50985c618a53470b54cb64e4675d4a5cd8b41c226a`）是不可变历史快照，本 v2 仅以
矩阵恢复终态之后的事实接续其门禁汇总，不改写 v1。报告专用提交由根执行；本报告不自引用未来提交 SHA，
最终 leaf SHA CI 与第二次独立结果硬审由后续外部收据绑定。

## 1. 身份与基线（leaf 与联合输入严格区分）

| 项 | 值 |
| --- | --- |
| 审计冻结基线 | `a5675e1245bebba41c499e56bd2c8a16be2d5dae`（tree `fac6f445…dc50`），两发现的原始反例来源 |
| product leaf | `0542d38dac86d51642d1095c77f7901b3de26c9e`（tree `ee2ee413…54da`，分支 `fix/audit-product-20261105-adp35vg4`） |
| 联合验证输入 | `aa24f1dd02d4a6856aa812b83c64fc7b1407c4a7`（tree `3a6fffc…2fc3`，分支 `test/audit-core-integration-20261105-adp35vg4`，product+validation 两 leaf 顺序 cherry-pick 而成） |
| validation leaf | `b9cd31cd084b2411589128e7268d39eacac83741` |
| 审计原件 | `astra-audit-20261105-kze8b3ss/AUDIT-REPORT-v1.md`（`6b3fab12…f358de`，13 项确认发现，本模块承担 ASTRA-A-001、D-CLI-001） |
| 写入时点核验 | 工作树 clean；8 个源/测试文件实测哈希与 `local-freeze-01/frozen-input-v1.json`、`core-integration-input-v1.json` 逐字节一致 |

**归属规则**：第 5 节全部矩阵与联合定向结果产生于联合源 `aa24f1dd`，只证明该组合输入，**一律不计为任一
leaf 的独立运行成绩**；product 自身的定向红绿（第 2–4 节）来自本模块自有构建与旧 f614 基线产物。

改动文件（对 a5675 的完整 diff SHA-256 `d40afbb2f536abf16699bd083859ac1a82631005619d8026864f966d7038aec3`，
仅触及计划允许的 8 个源/测试路径 + 2 份 v1 报告新增）：

| 路径 | SHA-256 |
| --- | --- |
| `src/store.cpp` | `807b2fd766ee2e253872cf08e26ad858081f9bfdce046394eb6ae62e8148d5b3` |
| `src/store_cleanup.inc` | `4ab028e5a053533e0080d79ea8fc0130d60480a2c87ae39cb6af33ad401cbec1` |
| `src/cli.cpp` | `b426f64c4c4ae0fd4ad760f9b7e9916d2439af83fd2253f12ba1f78ae4d68a04` |
| `src/offline_runtime.cpp` | `717602a52293cc8428d1f103debfe11c69ea45760828dc5da12dcc1b62ec258f` |
| `tests/allocation_recovery_tests.cpp` | `173b2389bb166482aeb9a661631b87373e7070b69c020af24b1adedc36a679b8` |
| `tests/test_s1_allocation_recovery_cli.py` | `2d3c20165369659a1bd832fc4430e7c22ed0fe59a11375015726b5e49cf0741a` |
| `tests/offline_runtime_tests.cpp` | `e33a1db1277864f06f47ea4bb8000b7d38fbf910ba4974628dd709cb64e95075` |
| `tests/s1_cli_smoke.py` | `5d7bf3789dfab988a6fa62d477399a74bab43aa020a12796cffada89be0498c1` |

## 2. ASTRA-A-001（合法长路径 allocation 无法弃置）

**原基线与反例等级**：审计 `A_store/tmp/path-probe-01`（真实旧库执行，parent=346、final=377、stage=379
字节）：prepare+preflight 成功而 abandon 报 `invalid tool identifier`，记录仍 PREPARED、新键冲突；短路径对照
成功。证据等级 REPRODUCED（compile/run/stdout 保留；run 记录未单独列出 binary hash——该粒度限制原样保留，
不提升原件等级）。

**最小修复**：`store.cpp` 新增 `owned_path_strings`——对保留路径应用既有路径域
`text_field(value, "owned path", 4096)`，保留非空/NUL 检查、稳定字典序与重复拒绝（`duplicate owned path`）；
仅替换 `store_cleanup.inc` 两处调用（abandon 的 `retained_paths`、cleanup_status 的 retained `paths`）。
`strings` 工具名域及其 256 字节默认上限原样；`json_text` UTF-8/JSON/字节预算与 prepare 的
`protected_path + "/work" < 4096` 门禁不变；弃置仍为 metadata-only（`filesystem_observed=false`、
`deletion_performed=false`），无任何路径读取/规范化/文件系统观察。

**最终 RED/GREEN（implementation-04 双链接证明，完整命令链留痕）**：最终测试字节（`173b2389…`）按候选构建
原旗标（`-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`）编译一次得到对象
`89f604b1…`，分别链接已核验旧 f614 库（`libwls_s1.a 6cf0984c…` + `libwls_kernel.a a780803a…`）与候选库
（`5bc7ee89…` + `a780803a…`）：

| 侧 | ELF | 结果 |
| --- | --- | --- |
| RED（旧库） | `ceb7a4af…` | exit 1，31 通过 / 3 预期失败：两处长路径弃置 `invalid tool identifier` + 路径专用文案断言（`allocation_recovery_tests.cpp:752`），墙钟 16s（原 90s 期限未触及） |
| GREEN（候选） | `c7fe98e7…` | exit 0，34 通过 / 0 失败，墙钟 15s；与既有候选构建 ELF **逐字节相同**（把 12:45 候选构建确为最终源码字节的推断升级为字节同一性证明） |

两侧运行输出与 implementation-02 的 12:45/12:47 历史运行逐字节一致。CLI 长路径弃置红例：旧 CLI
`fab23641…` 上 10 例 1 失败（`invalid_input / invalid tool identifier`）——原审计未动态复现该分支，
登记为本次新增证据而非审计原等级。

**覆盖**：final/stage 各自 256/257 精确绝对字节边界；>256 字节多组件 UTF-8/空格路径全链（marker/receipt
逐字节保留、无 stage 弃置不产生分配文件、ABANDONED/proof=null、原键精确重放幂等、异参冲突、同键不复活、
active=1/history=2 槽位释放）；`workspace_cleanup_status` 与 CLI `closeout`/`owned-workspaces` 展示两个完整
保留路径；嵌套拒绝置于替换分配之前；伪造 5000 字节路径得路径专用错误（不含工具名文案）。

**工具名域证据分层**：静态层——`text_field`（默认 256）与 `strings` 与 a567 字节一致（成对提取哈希
`2e53456e…`/`1f2f0c0d…`；提取边界与计划作者不同，以成对相等 + 单 hunk diff 为实质证明），256 字节工具域
未放大；动态层——admission 对未注册 257/256 字节工具名的拒绝发生在 frozen template 授权处（`store.cpp:3027`
先于 `:3035` `strings()`），既有 admission 夹具**并未动态命中** 256 长度守卫，报告不如此声称。

## 3. D-CLI-001（run-fake 非法 disposition 被接受并推进新请求）

**原基线与反例等级**：审计 `D_protocol_config/repro_disposition_fresh_v2`（现存精确 CLI 真实执行）：fresh
`run-fake --disposition invalid-enum` exit 0，attempts 0→3、events 1→17、tasks/plans/ops 0→1，请求
RECEIVED→RUNNING（REPRODUCED，exact-source provenance 保留于审计原件）。

**最小修复**：CLI 在读取 disposition 后、`options.finish()` 与任何 Takeover Store 之前要求空或
`update/keep/cancel`（既有 bounded error JSON，不回显任意枚举原文）；`OfflineRuntime::run` 入口先验同一枚举，
随后以 tasks/request 只读快照计算 `pending_amend`，"有 pending AMEND 必须三选一、否则必须为空"的适用性
检查先于任何 `rebind_task`、`reserve_call`、模型 complete、ack 与业务效果；后段防御性检查保留，
任务数/questions/submission/wait 守卫与最终 Actor/CAS 准入不变。

**有意行为收紧（非纯无变化修复）**：既有"无 pending AMEND 时必须为空"规则扩展到 fresh——
fresh + `update/keep/cancel` 虽为合法字面值今后也被拒绝；已披露于 v1 报告、测试注释与红绿 manifest。

**最终 RED/GREEN**：CLI 侧旧 `wls` 13 例恰 2 失败（fresh `0 == 0` 被接受＝审计反例；absent-DB
`'storage' != 'invalid_input'`＝旧 CLI 先开 writer），候选 13/13；直接运行器旧库 ELF `55b03ac4…` 11/2
（fresh `:54` 不抛；published-plan 先 rebind 后拒绝 `:293`），候选 ELF `33657f68…` 13/13。absent-DB 仅对
非法字面值断言不创建 DB/WAL/SHM/writer-lock；合法但不适用值由运行器在调用者 Store 打开后判定，既有恢复
语义不变。**pending-AMEND 补例（FINALv2 必补项）**：`test_run_fake_pending_amend_invalid_enum_rejected_without_store_effects`
构造真实 pending task（`based_on_requirements_revision==1` 且 `paused_for_change`）+ 合法 amended script +
非法枚举，断言有界 `invalid_input`、stdout 空、全用户表排序逻辑快照（BLOB 安全、含兄弟请求）前后相等、
两请求 budget 不变；该子例在旧 CLI 上**通过**（旧运行器本就拒绝该态非法枚举）——如实登记为候选加固回归，
不改写为审计动态反例。三个真实导入该模块的 audit24 suite 在候选上全部 OK。

## 4. 定向验证

- **product 自身 12 项注册套件**（`allocation_recovery`、`allocation_recovery_cli`、`offline_runtime`、
  `s1_cli_smoke`、`r2_amend`、`r2_amend_cli_smoke`、`cleanup`、`cleanup_runtime`、`owned_capacity`、
  `owned_capacity_runtime`、`r24_recovery`、`r24_cleanup_recovery`）：首次运行 **11/12 失败**于
  `allocation_recovery_cli`（`'conflict' != 'unauthorized'`——新 CLI 夹具在嵌套检查前先创建了替换分配，
  原 Unauthorized 断言未命中目标守卫）；失败原件保留（`ctest-targeted.stdout` = `2d1a024e…`）。夹具顺序
  校正（嵌套检查前移）后，最终字节上的 `ctest-targeted-final2`（12:52Z）为 **12/12 Passed（280.79s）**；
  12:43Z 的 12/12（`ctest-targeted-final.stdout`）属 pre-final 字节窗口（`83e0280b`），仅作历史保留。
  守卫未放宽、未接受 Conflict 凑绿。候选构建注册项 95 不变。
- **联合定向 11 套件**（源 `aa24f1dd`，validation 侧 joint-targeted-02）：run A 10 suite + run B sandbox
  （含空格且 >64 字节 TMPDIR 下 19/19 方法）**11/11 通过**。日志限制如实保留：run A LastTest 被 run B 覆写
  未封存（已记录违规）；run A 10 个 JUnit system-out 中 9 个带 1024 字节裁剪标记（仅 config 保留完整输出），
  suite 结论依据 JUnit/ctest stdout 而非逐方法日志。

## 5. 完整本机矩阵（联合源 aa24f1dd；只证明组合，非 leaf 独立）

| 环境 | 结果 | 依据与限定 |
| --- | --- | --- |
| Release GCC13 umask0022 | **95/95 通过** | 完整 JUnit（95 例 0 失败，`22098aad…`）+ 封存 LastTest + stdout「100% tests passed」 |
| Release GCC13 umask0002（同一批 ELF） | **95/95 通过** | JUnit `b03aa17a…` + 封存 LastTest |
| Clang18 Release umask0002 | **95/95 通过** | JUnit `87c5f337…` + 封存 LastTest |
| UBSan Debug umask0002（原运行） | **INTERRUPTED_WITH_OBSERVED_FAILURES** | 86 项终态＝70 通过/15 超时/1 失败；#87 启动无终态；失败 suite 为 `persistent_event_runtime`（`CHECK checked < deadline`，原 5s barrier 等待，`persistent_event_runtime_tests.cpp:262`），原因未确立；JUnit/ctest meta/终态回执缺失，中断原因 UNKNOWN；原 Testing/Temporary、LastTest.log.tmp、checkpoint 一字未动 |
| UBSan 九项补充（原二进制 + 新私有 driver，唯一授权变换为显式 WORKING_DIRECTORY） | **9/9 Passed**（264.16s） | 标签 `UBSAN_UNFINISHED_REGISTRATIONS_SUPPLEMENT_01_ONLY`，非正式 95 项运行；原中断运行状态不变 |
| 共享 GCC13 Debug ASan/LSan（冻结 runner `46866558…`，Debug/-O0/halt_on_error=1/detect_leaks=1/全 95 不筛选/原期限） | **NOT_COMPLETE**：67 通过 / 27 压线超时 / 1 真实失败 | 失败 suite `persistent_event_runtime`（103.99s < 180s，与 UBSan 中断运行失败同一 suite）；内层 ctest exit 8（由 runner 自身 waitid/Recorder 机制记录，独立于 run-cmd helper）；证据采集完整、源/二进制身份不变 |
| 原 Make ASan（umask0002，原 CPPFLAGS 用户通道） | **中止于首个失败测试二进制**：序列未完成，后续 NOT_RUN | 构建成功、verify/contract 检查通过；16 条 RESULT 中前 15 条 `failed=0`，第 16 条 `async_coordinator_tests` 33 通过/1 失败（cross-pool FIFO 用例超原 5 秒内部等待，`async_coordinator_tests.cpp:191`）；`make[1] Error 1`/`make Error 2` 是 make 报告的目标错误文本，非恢复的内核 waitstatus；`allocation_recovery_tests` 在序列中 RESULT 34/0 通过 |

**wrapper 退出码裁定（全面适用）**：`planner-adjudication-wrapper-exit-and-cleanup-evidence-v1.json`
（`a29224f9…`）确认 run-cmd-01/02 外层回执的 `exit_code=0`/`signal=null`/`cleanup_complete=true` 因
CPython ECHILD 伪 0 机制不可信——11 份受影响回执的外层直接子进程退出/信号/正常路径清理一律按 **UNKNOWN**
解释（含全绿 lane 的回执；原字节不改动）；早前阶段摘要中的 true_exit_code 1/2/0 口径据此撤回为 UNKNOWN；
上表测试结果均以被裹程序自身权威原件（完整 JUnit/LastTest/runner summary/make stderr）为依据。三环境
95/95 与 9/9 的**测试结果**保留，但**无任何外层退出/清理完成证明**。该类 helper 不再用于新产品测试。

**结论**：`LOCAL_VALIDATION_COMPLETE = false`。`persistent_event_runtime` 在两个独立环境（UBSan 中断运行、
GCC ASan gate）失败，是已记录的真实信号，原因未确立，未归因于本模块修复、未关闭。

## 6. 首次独立代码/证据硬审

hard-review-01（全新会话 `sess_8917d049-a875-4b78-b321-4b73dc890aea`，与计划审核 `sess_068223e5…`、实施
`sess_5df50839…`/`sess_16606cda…`/`sess_a902f49f…`、联合定向 `sess_1cd40211…` 均不同）：结论
**HARD_REVIEW: PASS**（收据 `3f919426…`，响应 `68170fe2…`）。范围严格限于
PREPUBLICATION_SOURCE_AND_LOCAL_TARGETED_EVIDENCE（冻结源码、最小修复、真实红绿、定向证据、组合依赖、
偏差审阅）；明确不代表完整矩阵、最终 CI 或正式 PR 审核。

## 7. 发布与 CI 现状（报告提交前事实）

- 根在唯一文件清单与 `git diff --check` 审查后本地冻结提交 `0542d38`（ordinary commit），并执行
  `git push -u origin fix/audit-product-20261105-adp35vg4`（exit 0）；以 PR #36 分支为 base 创建
  **Draft PR #40**（OPEN，head `0542d38`，base `test/n5-persistent-workflow-20261104-_7wxnkyn`）。实施各阶段
  均未做任何 Git 写入。
- **head push CI**：run 42058494031（event=push，head `0542d38`）六 Linux lane（strict、strict-umask-0002、
  Clang-18、ASan、UBSan、Make-ASan）全部 success，六 lane 检出日志与 head 一致。
- **PR 事件 CI**：run 42058826214 实际检出合并提交 `9cea9790…`（parents a5675e1 + 0542d38），六 lane
  success——单独登记，不冒充 push-head 成绩。
- **source-snapshot**：run 42058494066（commit `0542d38`，967 条目全匹配，artifact digest `0c2ca84b…`，
  归档未执行）；联合分支 push CI run 42058523307（head `aa24f1dd`，六 lane success）与 snapshot run
  42058523351（969 条目全匹配）。
- 上述均为**报告提交前**的 head 成绩；本报告提交后的最终 leaf SHA CI、source-snapshot 与第二次独立结果
  硬审由后续外部收据绑定（见第 9 节 PENDING 项）。

## 8. 过程偏差与证据保留缺口（全量披露，原件未改写）

1. implementation-01 为基础设施失败（exit 1、无最终模型响应），不计产品 RED。
2. `/tmp/rebuild-alloc3.log` 系统盘外日志违规：副本 `ec835ee5…` 保全、原件未动，违规不合规化。
3. implementation-02 同名日志路径复用（`run-alloc-final.*`、`run-alloc.stdout`）：最终字节已哈希留档，
   中间字节 UNKNOWN。
4. 12:47Z relink argv 未单独记录、中间测试字节 `83e0280b` 未保留：由 implementation-04 字节同一性双链接
   证明为**新链**闭合，历史缺口保持 UNKNOWN、未回填。
5. `implement-summary-v1.json` created_at 异常：不作为事实使用，排序以真实入口/命令回据为准。
6. 候选构建 `-j1`（实测约 1.4GiB 可用内存、无 swap）：已记录非静默。
7. 首次定向 11/12（夹具顺序）→ 12/12：两份原件与原因均保留，守卫未放宽。
8. implementation-04 compile-01 引号失误失败原件保留；权威 argv 以 compile-02.meta 为准。
9. implementation-04 收据缺 `plan_review_session_id`：根 sidecar 补记（`missing_original_field=true`，
   原件未改）。
10. validation 侧联合证据链披露偏差：自述 `/tmp/x` 使用、manifest 首稿覆盖、越界 agent memory 写入、
    run A LastTest 覆写、9 个 JUnit system-out 1024 字节裁剪、prebuild-static-gate CMake 哈希誊写错误
    （新记录校正，历史文件未改）、joint-targeted-01 早期 BLOCKED 后获 slot。
11. full-matrix-01 UBSan 原运行中断（86 终态，原因 UNKNOWN，缺 JUnit/meta/终态回据）；
    `build-release-full-01.meta.json` 为 0 字节空文件，非有效回执。
12. run-cmd-01/02 外层退出码捕获缺陷（伪 0）：11 份回执按第 5 节裁定降级 UNKNOWN；原字节不动；
    该类 helper 已停用于新产品测试。
13. GCC ASan NOT_COMPLETE 与 Make-ASan 中止为真实测试结果：不重跑、不筛选、不放宽期限/断言；
    make 序列的 16 条 RESULT 为方法级行，与 95 个 CTest 注册项分开计数；Make 构建后关键产物哈希不构成
    执行前二进制身份。
14. 本报告阶段仅写入两份 v2 报告文件；无源码/测试/构建/CI/registry 改动，无 Git 写入，无产品测试运行，
    无新 helper 实验（run-cmd-01/02 未使用）。

## 9. 门禁状态（分别记录，互不替代）

| 门禁 | 状态 |
| --- | --- |
| 计划审核 | PASS（既有 `sess_068223e5…`，DRAFT-v1 审核 + 同作者 FINALv2 续行裁定） |
| 生产代码 | leaf `0542d38` 已 push；PR #40 Draft OPEN |
| 定向验证 | product 自身最终字节 12/12（首次 11/12 保留）；联合 `aa24f1dd` 11/11 |
| 第一次独立代码/证据硬审 | PASS（范围限定，见第 6 节） |
| 完整本机矩阵 | **未通过**（三环境 95/95；UBSan 原运行中断 70/15/1 + 补充 9/9；GCC ASan NOT_COMPLETE 67/27/1；Make-ASan 中止于首个失败二进制）；`LOCAL_VALIDATION_COMPLETE=false` |
| push | 完成（根执行，非实施者） |
| 远端 CI | 报告前 head `0542d38` push CI 绿；PR 事件合并 CI 绿（单列）；联合 `aa24f1dd` push CI 绿；**报告提交后的最终 head CI PENDING**（后续外部收据） |
| 最终独立结果/证据硬审 | NOT_RUN |
| 正式 PR review | NOT_RUN（`PR_REVIEW_COMPLETE=false`） |
| MERGED | false |

## 10. 仍开放项

- 历史 Clang/live_workflow/Make IPC/sanitizer 问题保持 OPEN；`historical_clang_asan_failures_closed=false`
  未被改写为成功。
- `persistent_event_runtime` 在 UBSan（中断运行）与 GCC ASan 两环境的失败：已记录，原因 UNKNOWN，
  未归因、未关闭。
- Windows 11 普通用户 NTFS：NOT_RUN（本批不并入）。
- 私有原始托管：BLOCKED。
- 严格 WAL/SHM 兼容限制按原状态保留。

报告仅含安全事实、相对证据引用与哈希；私有 raw 日志、凭据与敏感 marker 不入 Git。缺失字段一律
`unknown`，未补造 0。证据根：`…/audit-fixes-20261105-81xzfrt5/product`（implementation-02/03/04、
hard-review-01、local-freeze-01、publication-01、remote-results-01、report-implementation-01）、
`…/audit-fixes-20261105-81xzfrt5/core-integration-01` 与 `…/validation`（joint-targeted-02、full-matrix-01/02、
interruption-recovery-01、各 planner 裁定）、`astra-audit-20261105-kze8b3ss`（审计原件，只读）。
模型调用仅为工程 ZCode 会话；零真实/付费供应商调用，不构成 WLS 供应商验收。
