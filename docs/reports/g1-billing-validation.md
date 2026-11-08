# G1-1 冻结 token 价格与显式估算账目 — 实施验证（2026-11-08）

一页当前报告：记录本批 G1-1（billing）在本工作区的实际实施与本地验证结果。
基线 `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631`（main，G0 #43–47 已并入）；
分支 `feat/g1-billing-20261108-y1wf1f4k`。Store 保持 **410**，未改任何已发布 SQL、
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
  - **31 次真实调用门禁**（`tests/test_billing_baseline_e2e.py`
    `test_10_unsigned_estimated_billing_completes_thirty_one_original_calls`）：一次
    授权、无签名 policy，真实 `submit → live-status → pump-live → Store → TLS →
    settlement` 链路。断言 Dispatcher `submit_plan`×1 + `accept_candidate`×3（4）、
    每任务 TeamLead `issue_work_order`/`check_submission`/`report_experience`（9）、
    每任务 Worker 5 次文件工具 + `submit`（18），合计 31 个实际模型调用；
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
  - **全量本地 Release ctest：78/78 注册目标全部通过**（2056s；树内 `build-g1-release`
    布局，日志 `billing-implement-01/evidence/13-green-full-ctest-intree.log`）。
    说明：`doctor_diagnostics_cli` 与 `review_cli_smoke` 依赖"构建目录父级＝源码树根"
    的可执行相对资源发现；在源码树外的外部构建目录（计划建议的 `billing-build-release-v1`）
    中运行同一套件时这两个目标因该布局差异失败（76/78，
    `evidence/10-green-full-ctest.log`），产品行为本身不受影响。

## 规模与边界

- 产品 delta（`git diff --numstat`，added+deleted）＝ **610**（目标 ≤730，硬上限 800）。
  单文件 `src/store_live.inc` 356 行超出其 260 行规划预算，但总 delta 仍低于目标值；
  未拆 PR、未删除安全校验。SQL/Store 版本/旧 profile 输出逐字未变。
- 测试新增均在既有注册内子例（Python CLI harness 与既有 C++ 套件），未新增 CTest
  注册（仍为 78）。

## 限制（如实）

- Windows 11 保持 `NOT_RUN`；私有托管 CI 证据保持 `BLOCKED`（原始日志仅本地）。
- 远端 CI（三核心 + 完整六配置 + source-snapshot）尚未运行：本批到具体 PR 可审为止，
  未合并、未推送；main 侧完整验证列为待授权后的门禁。
- 估算不是供应商报价或签名收据；真实 provider 计价、付费调用与 411/013 语义仍按
  冻结边界留给后续批次（G1-2/G1-5 由其各自计划与作者分区）。
