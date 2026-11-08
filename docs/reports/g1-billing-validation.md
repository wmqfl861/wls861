# G1-1 冻结 token 价格与显式估算账目 — 实施验证（2026-11-08）

一页当前报告：记录本批 G1-1（billing）在本工作区的实际实施与本地验证结果。
基线 `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631`（main，G0 #43–47 已并入）；
分支 `feat/g1-billing-20261108-y1wf1f4k`，本批产品提交
`50e85fa26486e00cf19eea0e190fc67581cea2de`（已由根普通推送至该分支；无 PR、无
合并、无强推）。Store 保持 **410**，未改任何已发布 SQL、
迁移注册或 `docs/handoff/` 历史证据。

## 实施内容

- `provider_profile.hpp/.cpp`：公开值类型 `TokenPricing`、`UsageDimensionState`、
  `CostBasis`、`PricingEstimate`；严格 parse/serialize/validate（rate 0…10^12 整数、
  reported 必带 rate、not_applicable 禁 rate、拒绝未知键/null/float/负数）；纯函数
  `estimate_token_cost`（仅在总和末尾一次向上取整，全程 int64 checked，无 UB/浮点）与
  `minimum_cost_reservation`（输入侧取适用 rate 最大值）。`ModelUsage` 与旧
  ModelResponse JSON 形状未动。
- `store_live.inc`/`store.cpp`/`store_capacity.inc`：统一有效账目视图（effective
  accounting）。估算只基于原结算 usage + Completed 原响应；取消/deadline/HTTP 错误/
  host unobserved 不定价；N/A 冻结维度不计未知，原响应却提供计数则 `policy_mismatch`
  并停新业务。settlement 在原事务把估算 E 记为有效费用（原五列仍写 null）；后到可信
  实际 A 只调差额（A==E→0、A>E 全额、A<E 减少有效预算），不二次释放预留。
  `has_unsettled_model`、`live_accounting_ready/status`、capacity `pending_calls`
  共用同一完成判断；priced 状态用完整 SQLite 读快照枚举并输出 estimated/reported/
  unknown 分项；`Store::budget` 追加 `accounting_format=wls-estimated-budget-v1` 与
  cost breakdown；`usage_projection`/`live_attempt` 增加
  `budget_accounting_complete`、`budget_unknown_dimensions`、`not_applicable_dimensions`
  和版本化 `estimated_cost`；`verify_usage_ledger` 先验冻结价格身份（settings 原字节
  SHA、profile SHA、冻结角色 provider 一致，不符报 Corrupt）再重放核对有效差额。
- `event_runtime.cpp`：捕获后、encode/凭据前的最低预留预检；Store reserve（INSERT/
  预算 CAS 前）与 claim 首发前重复同一复核。带 `billing_challenge` 的 profile 不用
  估算完成账目（签名路径不回退），估算永不标注为 actual。

## 验证结果（本地 Release，普通用户，/mnt/d ext4）

- RED（基线产品 4987f2c + 新测试，构建自本树 `billing-build-release-v1`，wls 二进制
  SHA-256 `8d154917aaaa3e0a713f1d1ae2d7ad8a72933ee658dd0a6f102a9af843538d8d`）：
  - `test_live_workflow_cli.py` 15 例中 3 个新 pricing 例失败于配置拒绝
    （`missing or extra provider profile fields`）＝配置 RED；新增的基线计数例在基线
    通过并记录 send 计数：首 pump 新增 1、累计 1；同请求再试新增 0、累计 1，
    `needs_accounting`、nullable cost 不变。
  - `test_billing_baseline_e2e.py` 新 31-call 用例失败于同一配置拒绝。
  - 日志：私有证据目录 `billing-implement-01/evidence/04、05`。
- GREEN（实施后同一构建目录增量构建）：
  - `test_live_workflow_cli.py` 15/15 通过；`billing_tests` 14 子例、
    `live_model_store_tests` 21 子例通过（新增严格解析/整数向量/最低预留、有效账目
    完成、A==E/A>E/A<E 差额、超预留停止、cancelled/N-A-mismatch 不定价、重开/只读、
    有估算+迟到收据的读快照竞争、settings 漂移 Corrupt、容量释放）。
  - **31 次受控本地合成调用门禁**（`tests/test_billing_baseline_e2e.py`
    `test_10_unsigned_estimated_billing_completes_thirty_one_original_calls`）：一次
    授权、无签名 policy，`submit → live-status → pump-live → Store → TLS →
    settlement` 链路在本地真实执行；这 31 次调用全部是发往**本机受控合成 TLS peer**
    的「模型形状」HTTP 请求，**不是真实供应商/模型评估，也没有任何付费调用**。
    断言 Dispatcher `submit_plan`×1 + `accept_candidate`×3（4）、
    每任务 TeamLead `issue_work_order`/`check_submission`/`report_experience`（9）、
    每任务 Worker 5 次文件工具 + `submit`（18），合计 31 个实际链路调用（对端为本机
    合成 peer）；
    `call_attempts`/`live_send_claims`/`live_transport_observations`/settlement/反馈
    各 31 行、peer 请求数 31；每调用 I=11/O=7/Cr=3、Cw 缺失，价格 1.5M/4M/0.25M/NA，
    手算 `ceil((8*1.5+3*0.25+7*4))=41µUSD`、总计 31×41=1271µUSD，与
    `budget.spent_cost`、估算分项及 Python 独立整数计算一致；原
    `cost_micro_usd`/`cache_write_tokens` 与 actual projection 保持 null；
    `reserved_cost`/`unknown_cost` 归零、无 dispute/超预留、accounting ready。
  - 带 `billing_challenge` 的 profile 即使同时携带 pricing，坏签名收据仍使业务停止、
    估算分项为 0（`test_91_pricing_does_not_replace_a_required_signed_receipt`）。
  - 专项 ctest 12/12 通过（billing、live_model_store(_tls)、live_workflow(_cli)、
    billing_baseline_e2e、single_agent(_runtime) 等，712s）。
  - **全量本地 Release ctest：78/78 注册目标全部通过**（日志头 12:08:07Z 起、
    总时长 2056.28s；树内 `build-g1-release`
    布局，日志 `billing-implement-01/evidence/13-green-full-ctest-intree.log`；该次
    运行于全量构建完成后单独执行，与下列首跑事实分开记录、不混同）。
  - 首次全量运行（源码树外外部构建目录 `billing-build-release-v1`，即计划建议布局）
    为 **76/78**（exit 8）：`doctor_diagnostics_cli`、`review_cli_smoke` 失败于错误
    路径断言 `unsupported != invalid_input`；首跑原始日志保留于
    `evidence/10-green-full-ctest.log`（日志头 10:55:16Z、总时长 2444.29s、stdout
    末次写入 11:36:01Z）。根 11:43:57Z 的路由观察记录该首跑日志已终结稳定，当时
    并发活跃的是**后续本树内编译**（为第二次全量运行准备）与 **Protocol 模块的
    CTest**（其 full-ctest 证据 11:43:55Z 仍在写入）。该观察证明的是首跑结束后、
    两跑之间这一时点的跨模块资源并发，**不是**首跑 CTest 批次内部的并发；首跑
    自身是否严格串行现有证据不足以判定，该观察也不与第二次 78/78 混同。两目标
    在同源码树内构建下通过，失败证据与「构建目录父级＝源码树根」的可执行相对
    资源发现布局差异**一致**；但该资源/布局成因**未经独立证明**，本报告不宣称
    其为已证实的唯一原因，也不据此断言产品行为不受影响。

## 规模与边界

- 产品 delta（`git diff --numstat`，added+deleted）＝ **610**（目标 ≤730，硬上限 800）。
  单文件 `src/store_live.inc` 356 行超出其 260 行规划预算，但总 delta 仍低于目标值；
  未拆 PR、未删除安全校验。SQL/Store 版本/旧 profile 输出逐字未变。
- 测试新增均在既有注册内子例（Python CLI harness 与既有 C++ 套件），未新增 CTest
  注册（仍为 78）。

## 限制（如实）

- Windows 11 保持 `NOT_RUN`；私有托管 CI 证据保持 `BLOCKED`（原始日志仅本地）。
- 发布与远端状态（2026-11-08 报告修正时点）：本地 Release 78/78 与独立硬审
  **PASS**（全新会话，区别于计划审核与实施会话）已完成；提交 `50e85fa` 已由根
  普通推送至 `feat/g1-billing-20261108-y1wf1f4k`，无 PR、无合并、无强推。PR、
  远端 CI（三核心 + 完整六配置 strict/umask0002/UBSan/ASan/Make-ASan/Clang18 +
  source-snapshot）以及本地 Clang/sanitizer/Make 变体、严格串行复跑组合仍为
  PENDING/NOT_RUN；远端将按既有 workflow_dispatch 在目标 ref 执行，属常规 CI、
  无需新授权。main 合并授权仍缺失，main 侧完整验证保持待授权后的门禁。
- 估算不是供应商报价或签名收据；真实 provider 计价、付费调用与 411/013 语义仍按
  冻结边界留给后续批次（G1-2/G1-5 由其各自计划与作者分区）。

## 补充单次 pump 观察（2026-11-08 14:14:48Z—14:15:29Z，HEAD `febd6c0`）

- 受控观察（原 ELF `build-g1-release/wls`，SHA-256
  `df3dddf7995cb3a75576afdf4fd3bf7644ca182937dc191eba090ab0b4da93b2`、原 test_10、原
  fixture/参数）下，单个原 unittest 通过（`Ran 1 test in 39.888s`，exit 0），期间**实际
  开始并完成 2 次 pump**；原 CLI 输出完整留存于私有证据 `billing-pump-observation-01/`
  （结果 JSON SHA-256
  `39e5b5452b3077b98a5bfeb1c39494a1317181fd7c2372cccd2a5654dbee0d2d`）。**历史其他运行的
  pump 次数仍 unknown**。
- 第一轮：30 次调用（Dispatcher 4/TeamLead 8/Worker 18）、`spent_cost` 1230µUSD、
  `manager_calls` 12；三任务 ACCEPTED、alpha/beta 经验作业 DONE、**gamma 经验作业
  PENDING**；顶层 `blocked` 恰 1 条 `manager_limit`（gamma TeamLead），而
  `accounting.ready=true`、`accounting.blocked_count=0`（管理额度阻塞不等于计费未知）；
  `stop_reason=quiescent`；没有为第 31 次调用产生任何预约或发送。第二轮只新增 gamma
  经验调用 1 次（`workflow_kind=experience`）：累计 31 次/1271µUSD、经验作业全 DONE、
  `blocked` 空。两轮 `request_status=RUNNING`、`business_delivered=false`。
- **原 FINAL（`plan-final-v1.md` §7）的「一次 pump 完成 31 次调用」条款维持
  NOT_MET**：本观察是该条款的明确反例。上述 31 次计费证据本身（30+1、手算一致、账目
  ready）不受影响；观察硬审仅 PASS 诊断完整性，不批准 full G1-1、不追认全量门禁或历史
  pump 次数。
- 同一授权文件（一次创建、两轮前后字节哈希一致）被显式复用于两个 `pump-live` CLI
  调用；**续批由测试代码执行，不是产品自动跨批推进**，两轮请求均 RUNNING 且未交付。
- 自动跨批推进、单入口多 batch、全请求墙钟与累计预算归 Controls/集成批次审定；原每
  batch 经理上限 12（`system.limits.manager_max_tool_calls_per_wake`，
  `config/examples/system.yaml:65`，`src/config_loader.cpp` 1..12 校验）与保留工作规则
  在 Billing 中继续有效，未放宽。

## 本轮 test_10 回归改写与状态（准备阶段，2026-11-08）

- 依据补充 FINAL `plan-pump-regression-final-v1.md`（独立计划审核 PASS，六项建议均采
  纳），现有 test_10 的至多十次重试循环改写为**两次显式 `pump-live`**：首批断言从本请
  求冻结 config_snapshot 只读取经理上限 12、30 次调用/1230µUSD/角色 4/8/18、gamma 经验
  作业 PENDING、无第 31 次预约或发送、30 条原 attempt 的逐角色逐节点工具序列、accounting
  与顶层 blocked 的区分及请求/配置/授权哈希绑定；第二次以完全同一 request/db/授权/工作区
  文件仅新增 gamma 经验调用，保留原全部终态断言，并把原循环退出条件改为显式三任务
  ACCEPTED、三经验作业 DONE。两轮 RUNNING/未交付断言保持。
- **本轮新测试执行状态：NOT_RUN——等待 root 明确移交本补充任务的独占产品构建/测试
  槽**（当前槽由 Correction76420 持有；准备阶段未运行任何产品/测试/CTest/构建/语法执
  行）。获槽后最小验证为串行三个既有 CTest 注册（`billing_baseline_e2e`/`config_loader`/
  `event_runtime`，原 900 秒期限不变），绑定原产品 ELF + 新测试源；真实命令、目标数、退
  出码、精确 HEAD/差异与证据绑定只在实际执行后追加。
- 此前本地 78/78（12:08:07Z 树内复跑）与上文各时点结果均为**旧测试源**的历史陈述，不覆
  盖本轮新测试源；本报告上文原时点记载（配置 RED、首跑 76/78、专项 12/12、11:43:57Z 资
  源观察等）保持原样不改写。
- 旧 head 远端终态（root 2026-11-08T14:29:40Z 保存的
  `billing-ci-febd6c0-terminal-zeidnfpn/observation.json`，SHA-256
  `c2f8df5700bfdfbea5d3f647b8a30b9e5849079ed2b39a88308e919c8c879481`）：run
  `42485945254`（workflow_dispatch）在精确 head `febd6c0366a9744c20077fea114f13eea48fbc4c`
  六配置（strict、strict-umask-0002、UBSan、ASan、Make-ASan、Clang18）terminal
  success；ASan 走 CTest 注册（78/78，2155.52s），Make-ASan 走**直接 Make 测试路径**且
  用户 `CPPFLAGS=-DWLS_CI_USER_CPPFLAGS=1` 传入成功，两者按不同路径分别记账、不混同。
  该证据只覆盖旧 head 与旧测试源，**不验证本轮新测试源，也不关闭原 single-pump
  NOT_MET 或 G1 总门禁**；上文更早时点的 CI PENDING 记载为当时事实，保持原样。
