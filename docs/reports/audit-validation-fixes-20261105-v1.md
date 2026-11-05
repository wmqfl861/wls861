# 审计修复 validation 模块报告 v1（部分：独立 Python 阶段）

时间范围：2026-11-05 implement 第一阶段（根路由 `stage-routing-v1.json`：仅独立 Python
工作与 B/C fixture 准备；C++ 编译与正式 sanitizer 等待根分配 slot）。本报告只记录
实际执行的内容；未执行项一律 PENDING/NOT_RUN，不以部分通过代填。

工作区 `/mnt/d/codex-workspace/wls861-audit-fixes-20261105-adp35vg4/validation`，
分支 `fix/audit-validation-20261105-adp35vg4`，基线 `a5675e1245bebba41c499e56bd2c8a16be2d5dae`
（tree `fac6f4451f8d95998ce9a22a0d228f33c582dc50`），工作区在实施前干净。
系统解释器 `/usr/bin/python3` 3.12.3 + jsonschema 4.10.3（符合 requirements-test 锁；
未使用 venv，未安装任何依赖）。全部命令以普通用户 uid=1000 执行，TMPDIR 在数据盘私有目录。

## E001：Recorder 同组孙进程在父退出后继续运行 —— 已修复（本阶段完成）

- 原证据等级：审计 E 平台真实进程树反例（`reproduce_recorder_process_tree.py` +
  `recorder-process-tree-result.json`：孙状态 `S`、父 -15、外层回收成功）。
- 新原版 RED（`e001/red-01/`，未修改 runner）：新增真实进程树测试对原
  `scripts/run_gcc_asan.py` 失败于孙存活 oracle（`grandchild_state_after='S'`、
  `process_group` 字段缺失、0.77s、无挂起）。首次 RED 尝试因测试控制器父进程脚本误设
  SIGTERM 忽略而触发 KILL 分支（`-9 != -15`），已修正父进程脚本后取得干净 RED；
  两次运行原件均保留。
- 候选 GREEN（`e001/green-01/`）：`Recorder.run` 重写为 owned process group 有界收尾——
  主等待用 `waitid(WNOWAIT)` 观察直接子进程退出而不回收（锚定 pgid 不被复用）；
  组内仍有 live 成员或发生中断时：TERM→5 秒宽限→SIGKILL→至多 5 秒收尾证明预算；
  私有 step 收据新增 `process_group`（scope/身份 pid,pgid,session,uid,starttime/
  term_sent/kill_sent/member_exit_observations/remaining_live/escaped_members/
  external_reap_needed/leader_exit_observed/leader_reaped/deferred_signals/
  cleanup_complete/logs_status）；输出 hash 仅在组内 writer 退出后标 `complete`；
  `preserve_ctest_evidence` 与成功 require 显式消费 `cleanup_complete` 与
  `leader_exit_left_live_members`；公开 summary/step 白名单不变（新字段仅私有）。
- 回归：完整 runner 自检 `tests/test_run_gcc_asan.py` **68/68 OK**（原 60 方法 + 新 8：
  忽略 TERM 孙进程 oracle、TERM 可退出宽限路径、父 exit0 留活成员判不完整、启动失败
  无组信号、二次中断保全原异常、观测失败判 INCOMPLETE、/proc stat 解析、
  preserve 消费 cleanup_complete），真实 synthetic CTest 成功/非零/中断全部保持。
  自检是 runner 契约检查，不是产品 ASan 成绩。

## F-01：公开错误携带私有路径 —— 已修复（本阶段完成）

- 原证据等级：审计 F 合成 v3 plan + 不存在数据库 marker 反例。
- 新原版 RED（`f01/red-01/`，未修改脚本）：driver 对四个公开出口全部泄露——
  D1 `build_report` canonical 含 marker（`reason=str(error)[:256]` 带 marker 路径）、
  D2 evaluate CLI 顶层 stderr 含路径、D3 离线 `report().runner_outcomes` 原样嵌入
  旧式 outcomes（含 argv_tail/嵌套绝对路径）、D4 离线 CLI 顶层 stderr 含路径；
  分母保持 invalid_evidence=1/not_executed=9。测试级 RED：新 outcomes 投影测试对原
  脚本失败（marker 可见），既有 9 个用例通过。
- 候选 GREEN（`f01/green-01/`）：`evaluate_r3.py` 新增固定原因码投影
  （`public_reason_code`：InvalidEvidence 仅精确常量消息→确定码（覆盖既有全部断言族：
  logical_chain_broken/logical_policy_mismatch/logical_history_changed/
  recovery_projection_unproven/feedback_binding_changed/model_decision_target_changed/
  budget_reconstruction_mismatch/task_material_mismatch/common_condition_mismatch/
  evidence_mode_relabel_refused/transcript_live_admission_mismatch/
  live_source_frozen_mismatch 等），未知回退 `invalid_evidence`；OSError 按 errno 分类
  evidence_missing/unreadable/io_error；sqlite3 按 errorcode（CANTOPEN→unreadable）；
  KeyError/TypeError/ValueError 各固定码）；invalid 行输出固定码 +
  `error_fingerprint_sha256`（仅哈希、明确命名）+ `reason_detail_retained`；
  可选内部 `private_error_sink`（默认不启用，`report`/`build_report` 保持只读）。
  `r3_offline_acceptance.py`：`PrivateErrorCollector`（run 私有 0700 目录、O_EXCL 0600、
  64KiB 截断保真长度/hash）；`runner_outcomes` 显式安全投影（task/condition/status
  有限白名单、未识别 status→`UNRECOGNIZED` 永不 COMPLETED、reason 有限白名单→
  `runner_failed` 回退、`outcome_sha256` 安全 hash 引用、原 outcomes 文件字节不变）；
  CLI 顶层 stderr 均为固定码。
- 回归：driver 四出口全净；errno/SQLite/回退附加 driver（真实 chmod 000 + finally
  恢复、无效 SQLite、空 SQLite）全绿；纯 Python 套件
  `tests/r3_offline_acceptance_tests.py SOURCE` **15/15 OK**（原 9 + 新 6），整套
  墙钟 2.5–3.3s（CTest 30 秒预算内，注册/时限未改）。三个需 wls 二进制的套件
  （`r3_evaluation`、`r3_offline_acceptance_cli`、`billing_baseline_e2e`）断言已更新
  为确定原因码，执行等待编译 slot。成功路径字段/值保持；新报告 hash 自然变化。

## B-001：异步测试 Gate stop 丢失唤醒 —— fixture 已准备（等待 slot）

`tests/async_coordinator_tests.cpp` 保持基线字节未改（`74336e54…`）。
`b001/red-b001/`：锚定式分步补丁器 `apply_b001.py`（check 模式对基线四锚全过；
  red→green 链在副本上演练成功）、RED 态源码预览 `a38113db…`（仅 seam+三测试+注册，
  旧 `condition_variable`+notify-only `stop_callback` 算法原样）、GREEN 态预览
  `73160b1b…`（`condition_variable_any` + stop-aware wait，respect_stop=false 普通等待，
  seam/测试不变）、`B001-PROTOCOL.md`（slot 会话步骤：red 编译运行取确定性
  completed_from_stop CHECK 失败、green 同 seam 同断言、全 suite 回归）。本会话未编译
  任何 C++。

## C-01：两个测试临时根 /tmp —— fixture 已准备（等待 slot）

`tests/sandbox_tests.cpp`/`tests/owned_capacity_baseline_repro.cpp` 保持基线字节未改。
`c01/fixture/`：`mkdtemp_observer.c`（`-Wl,--wrap=mkdtemp` 位置 oracle：记录模板、
私有根外受控拒绝、绝不用于产品门禁）、原构造器抽取 fixture（字节来源与行号见
  `c01-manifest.json`，明确标注为抽取范围 fixture 而非完整 suite 红例）、候选构造器
  fixture（复用 test_support.hpp 的 TempDir 字节组合）、`run_c01_fixture.sh`（未执行）。
  本会话未编译、未运行。

## 受影响 registration 状态

| 套件 | 状态 |
| --- | --- |
| run_gcc_asan 自检（非 95 项） | 本机 68/68 OK |
| r3_offline_acceptance（30s） | 本机纯 Python 15/15 OK（2.5–3.3s） |
| r3_evaluation | 断言已更新；执行 PENDING（需 wls 二进制/slot） |
| r3_offline_acceptance_cli | 断言已更新；执行 PENDING |
| billing_baseline_e2e | 仅两处 reason 断言改为确定码；完整回归 PENDING |
| async_coordinator / persistent_event_runtime / scheduler / sandbox / owned_capacity* | PENDING（B/C 的 C++ RED/GREEN 与定向回归等待 slot） |

95 项 catalog、CMakeLists、CI、registry 与四个锁定测试源未改动（SHA 复核一致）。
历史 Clang/live_workflow/Make IPC/sanitizer 事项保持 OPEN；
`historical_clang_asan_failures_closed=false` 不变。

## 门禁状态（独立字段）

- IMPLEMENTATION_COMPLETE：PARTIAL（E001/F-01 完成；B-001/C-01 仅 fixture，
  其仓库源码编辑按红绿顺序等待 slot）
- TARGETED_VALIDATION_COMPLETE：PARTIAL（本阶段可跑的纯 Python 定向回归全绿；
  C++ 定向与三个二进制依赖套件 PENDING）
- LOCAL_VALIDATION_COMPLETE：NOT_RUN（完整矩阵等待 slot/联合输入）
- PUSH_COMPLETE / REMOTE_CI_COMPLETE：NOT_RUN
- INDEPENDENT_CODE_REVIEW_COMPLETE / INDEPENDENT_RESULT_REVIEW_COMPLETE：NOT_RUN
- MERGED：false（无合并授权）

无真实 WLS 模型/供应商/付费 API 调用；Windows 11 普通用户 NTFS 保持 NOT_RUN；
私有托管保持 BLOCKED。工程 ZCode 会话本身产生模型服务请求，不称零工程模型调用。
