# G1-3 纠错整改 v8：事实勘误与 source-only 测试准备报告

状态：**SOURCE_TEST_ERRATA_PREPARED**（2026-11-08）。本报告只陈述事实与哈希；
它不是运行验收，不声明任何 RED/GREEN 执行结果，也不修改任何历史原件字节。

依据：plan-final-v8.md（SHA-256
`1d5d55a1f96f8cf74bfed9518d2373afd55b6d8e2734a5b36e8395fc3203478f`）、
planner-record-final-v8.json（SHA-256
`cd8540331dd2b50f7b8592c2774619c0a39a4ed471463770332370353ea70ffa`）、
review-decisions-final-v8.json（SHA-256
`97709b87a8d292ac0408a3728bb78ec492a15fbc8c4e0c4d929ae8ce1d425279`）与阶段输入
correction-v8-source-errata-input-01.json。plan-review-05（session
`sess_8d5d27eb-b2c2-4616-bc02-121013782124`）对其审核对象 plan-draft-v7 的结论保持
`CHANGES_REQUIRED` 原值；本阶段是同作者 FINAL v8 采纳 P1–P5 后经 root 固定入口
路由的默认 SOURCE_TEST_ERRATA_PREPARATION 实施。

## 1. 阶段范围与未运行项

本阶段只做了三件事：核验并冻结当前 v6 候选输入、在计划允许的测试宿主中准备
C1/C2 回归源码、新增事实勘误/linkage 补件。**没有**：生产源码/头文件/配置/构建/
CI/SQL 修改；configure/build/ctest/产品或 TLS 运行；Git 写入/提交/推送/PR/dispatch。
运行状态逐项如下：

| 项 | 状态 |
| --- | --- |
| C1/C2 行为 RED 与 GREEN | NOT_RUN（本机执行禁止；等待 root 按 §7 冻结输入路由远端 CI 或获准环境） |
| F1 复跑（新物理 ELF 与依赖） | NOT_RUN / EVIDENCE_BLOCKED（CI 不保留实体；本机 RESOURCE_BLOCKED） |
| F1 冻结原最终测试字节 | DONE（见 §3；字节级副本已存私有目录） |
| F2/F3/F4 勘误补件 | DONE（新增独立文件；原报告/收据/日志字节未动） |
| 本机 /mnt/d 资源 | RESOURCE_BLOCKED 维持；本阶段写入约 0.6 MiB，全程高于 512 MiB 预测门 |
| 生产 800 行门 | 本阶段产品增量 0；既有 v6 产品 236+20=256 不变 |
| 私有证据托管 | BLOCKED（用户选择保留本机私有原件） |
| Windows 11 | NOT_RUN |

## 2. C1/C2 静态缺陷事实（运行时复现仍 NOT_RUN）

对当前 v6 候选字节（src/event_runtime.cpp SHA-256
`c72c73f41426986af675677278292dc30182d9924816ecbdb8026d9c5e424cfe`、
src/offline_runtime.cpp SHA-256
`1185080762b2310282b1bd045efdfc162b1ba8ba43193076a7aa6c61cdd399fc`）的静态核读：

- **C1**：`pump_workflow` 的 `correction_admissions` 计数声明在每轮每任务的
  claim 块内（src/event_runtime.cpp:1019），成功完成 claim 后（:1039-1041）同
  Worker 下一轮新 claim 重建计数、获得新额度。事件 404 入口（:367）与
  `OfflineRuntime::run`（src/offline_runtime.cpp:1110）的计数已覆盖各自整次
  调用，仅需回归保留。
- **C2**：workflow 入口耗尽时只置 stopped 并空完成 claim（:1024-1027），无
  blocked 条目、无 live 批停与 stop_reason；404 入口同样静默 break（:378）；
  Offline run 耗尽无专用输出（:1165）。live 的 worker_limit/call_limit 守卫
  分支（:592-603）不置 live_halted，失败后其余角色可继续。

这些是静态事实；按 FINAL v8 §8.4，未取得真实同条件业务 RED 前不得修改生产。
本次未改任何生产文件。

## 3. 已准备的 C1/C2 回归源码（未编译、未运行）

编辑严格限于计划允许的测试宿主。相对冻结 v6 字节（冻结副本与 SHA 见 §6）：

| 文件 | 阶段增量 | 新 SHA-256 | 新增内容 |
| --- | --- | --- | --- |
| tests/event_runtime_tests.cpp | +190/−1 | `19abb0446e3df8823be388168c356b3f6793eedfbb2f871e3fd3789baf00cb6e` | 5 个内部例：workflow 跨成功/跨 claim 累计（cap=1，恰 3 次调用、frozen_binding/config 不变、无第 4 次、correction_limit 可见）；cap=0 与 call_limit 同轮共存；404 入口 cap=2/cap=0 报告与 worker_limit 共存；双 Worker 兄弟隔离（第二个 Worker 独立运行）；二次显式 pump 新链（防跨泵持久限制）守卫 |
| tests/offline_runtime_tests.cpp | +189/−1 | `43a8825a4a4752c993a882aa49ccd87cc51fad5fcde081f895cf2c1f02afe64d` | 真正启用纠错宿主的 CorrectionRunSession fixture（structured-v2 + Store 004/005/006 + WorkerTools）；cap=2/cap=0 耗尽报告（blocked 数组 + stop_reason=correction_limit + 精确次数 + feedback 完整）；成功路径不误报守卫；跨成功累计保留守卫 |
| tests/worker_tools_tests.cpp | +41/−0 | `cdc411deb354b55e92eed8f4054ded58e43903233eddc10187eab083bc766bfe` | run 宿主守卫：cap=1 时 fail→成功→fail 恰 3 次无第 4 次；host call cap 未遭拒绝时不伪报 correction_limit |
| tests/test_live_workflow.py | +146/−0 | `48738c8f09eea229257d9d8ab306cf793702165780f129e0b88eb9f9be99f9f9` | 3 个新 peer 模式（correction_cross_claim / correction_exhaust / correction_siblings+guard）与 4 个 live 回归 L5–L8：跨 claim 累计与 correction_limit stop_reason、连续失败精确计数（cap=2 与 cap=0）、失败后 worker cap 全批停且可运行兄弟零新增网络/凭据、耗尽后全角色停批 |
| tests/live_workflow_driver.cpp | 0/0 | `234e0a47ded6200d1dba6bc17bfa77f579b1bea772fcfc7e8fb3c9736956ac78`（未改） | 全部场景可由既有 config 旗标（schema_retry_cap/worker_calls/phases/reconcile）表达，无需 driver 改动 |

全部断言走公共 API 与运行时 JSON 边界（calls/blocked/pending/stop_reason/
network_started_calls、Store 公共读、SQL 只读查询）；不引用任何尚不存在的
C++ 成员。期望的新输出契约（待修复实现）：blocked 条目
`code="correction_limit"` 含 role/node_id/task_id；仅 live 批次置
`stop_reason="correction_limit"` 并停全批；Fake 仅停受影响 actor；Offline run
新增 bounded blocked 数组与 stop_reason；更强既有 stop_reason 不被覆盖；
feedback/usage/hash 不被改写。旧候选上的预期 RED 差异即上述字段缺失或多余
调用（例如 workflow cap=1 跨 claim 场景旧候选产生第 4 次 Worker 调用）。

注册数不变：仅内部例新增（event 32→37、offline 17→19、worker 23→24、
live_workflow py 25→29）；CTest 注册仍为 focused 7 / 完整 78，无任何注册删除。

## 4. F1–F4 勘误摘要（公开事实；私有原件正文不复制）

- **F1（终跑物理 ELF 缺失）**：终跑清单声明的 event_runtime_tests
  `66a6cfe3…`、worker_tools_tests `5f17801d…` 与 baseline-artifacts 留存副本
  `0b3c9334…`、`7ff07e23…` 不一致，原件未找回。本阶段在私有目录冻结了当前
  工作树中的原最终测试字节（event `a7b09cb1…`、worker `1f0450e6…`）及另三个
  测试宿主字节，供未来 F1 复跑/RED 输入绑定；复跑本身仍 NOT_RUN。
- **F2（资源违规与摘要误导）**：preflight 预测后余量 ≈510MiB 已低于 512MiB
  门仍继续执行；执行中观测 517680KiB（≈505.55MiB）低于门；为恢复水位删除了
  4 个本会话自建诊断 ELF（repro-untenable/repro-suite/repro-wt/repro-ev，
  源码保留）；收尾 534904KiB（≈522.37MiB）不能证明全程维持。详见私有
  resource-erratum；原 preflight-resource.md（`33f1d7b4…`）、repro-note.md
  （`d2ae2689…`）、implementation-report.md（`e552e23b…`）字节未改。
- **F3（计数错误）**：私有 baseline-red-summary 的 event 30/13、model 20/0 与
  实际日志不符。按日志 RESULT 行：event 首跑 17 过/15 败（32，日志
  `63ec0eb4…`），终跑 19 过/13 败（32，`8cd5a8ca…`）；model 两跑均 17 过/0 败
  （17，`6f1fc924…`）；worker 终跑 18 过/5 败（23，`90e6b70c…`）。详见私有
  count-erratum；原日志与摘要字节未改。
- **F4（收据缺字段）**：receipt-implement-0c3781f9.json（`58679695…`）缺
  `plan_review_session_id`。新建私有 receipt-linkage 补件（类型为补充元数据，
  非新收据）：绑定原实施 session `sess_a6ca4e7a-6e4f-422a-8bd1-31fc3663dcd9`、
  真实 plan-review-04 session `sess_fad3a5f1-c352-4d8d-b465-320bd6802e27`
  （receipt `84e5f953…`）、独立硬审 session `sess_844f42f4-1afa-4cfc-8c94-65368007ea83`
  （receipt `b1290c15…`，结论 PASS 仅适用于 v6 focused 组件候选）。原收据未改。

历史结论保持原值：hard-review cf6759e4 对 v6 focused 组件候选 PASS、F1–F4 为
其强制整改项；review-05 对 draft-v7 CHANGES_REQUIRED；两者均不因本阶段改写。

## 5. 输入与保护

- 17 文件 v6 候选在编辑前与 planner-record-final-v8 绑定逐一匹配（含
  tracked diff SHA `73f64c7c…`）；生产 7 文件字节与既有
  correction-v6-execute-01/candidate-sources 副本一致（只读引用，未复制）。
- 5 个测试宿主编辑前字节以 0600/0700 防覆盖存入 root 指定私有目录
  `.wls861-local/g1-20261108-y1wf1f4k/correction-v8-source-errata-evidence-01/`
  （清单见该目录 SHA256SUMS 与 source-prepared 报告）。
- docs/reports/g1-correction-validation.md 与
  docs/implementation/R3-live-workflow.md 的既有字节未动；本报告是该目录下
  新增文件。

## 6. 后续门（未满足前不得推进）

生产 C1/C2 修改必须等待真实同条件业务 RED（测试到达指定第二 claim 并观察旧
候选多余调用，含可核 ELF/原始输出身份）；随后同测试字节 GREEN、完整 ≤800 生
产门、新独立硬审（session 不同于 review-05 与本次实施）。远端验证按 FINAL v8
§7 以冻结非 main ref 手动 dispatch 现有 CI，且不替代本地验收。真实/付费
provider 调用 0；Store 410 不变。
