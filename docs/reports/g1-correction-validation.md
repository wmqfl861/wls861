# G1-3 有界纠错组件验证记录（Fake 三入口 + 受控 local-TLS）

日期：2026-11-08。实施会话：root 经固定入口路由的 fresh implement（FINAL v6 /
record eedd7478…；阶段输入 `RESOURCE_PREFLIGHT_THEN_CONTROLLED_FOCUSED_RED_GREEN`，
e09a73b8…）。工作树 `/mnt/d/codex-workspace/wls861-g1-20261108-y1wf1f4k/correction`，
branch `feat/g1-correction-20261108-y1wf1f4k`，PR base=merge-base=HEAD 起点为
`4987f2c6a6fc0da4b91e53d3689019ba3f0e8631`（tree `efdfce03…`）。

## 范围与产物

生产增量（base→工作树，added+deleted）：`src/correction_detail.hpp` +74（新）；
`src/offline_runtime.cpp` +69/-3；`src/event_runtime.cpp` +47/-12；
`src/workspace_posix.cpp` +23/-4；`src/config_loader.cpp` +15/-1；
`src/protocol_codec.cpp` +4；`include/wls/config_loader.hpp` +4。合计
**product 236 added / 20 deleted = 256 ≤ 800**。tests 1470/27；docs 本文件与
R3-live-workflow 追加节。未改 Store/SQL/migration、live_model、https_transport、
provider_profile、structured_history/structured_context、worker_tools、Workspace公开头、
示例配置、build/config/scripts。

## 同条件业务 RED（未改生产字节，最终测试字节）

精确基线（HEAD 生产 + 本轮定稿测试）上，失败恰为纠错行为用例：
event_runtime 19/13（R1-R4、cap/call/script/budget/overflow 门禁、sibling、
workflow 入口四类与 cap）；worker_tools 18/5（run 宿主四类 + cap=0）；
config_loader 21/1（schema_retry_max 仍在无条件 deferred 列表）；
protocol_codec 31/1（false-parallel wire 无 tool_choice）；TLS 25 套仅
L1/L2/L3/L4 失败（首个普通失败即停批）。守护与两态负例（multi-tool/undeclared/
管理错误 STOP、非前置条件Conflict、反馈持久化故障、重复accept、sibling 隔离、
0-tool 回执错配×4、gates×3、unclassified×3、model/offline 全部）在基线通过。
原始日志：私有证据 `correction-v6-execute-01/logs/red-baseline-final-*.log`。

## 同条件 GREEN（最终候选）

六套件 22/17/17/32/23/32 全过；TLS 25/25 OK（L1=8 请求、L2=8、L3=9、L4=8，
quiescent 完成 submit→check→accept→experience，feedback/send claim/reconciliation
计数与请求数一致、provider_calls=0、cost NULL、spent=N×17、账目 ready）。
ctest 注册运行 7/7（config_loader、model、offline_runtime、event_runtime、
protocol_codec、worker_tools、live_workflow）100% passed，77.12s。
二进制/库身份与哈希：`correction-v6-execute-01/{baseline-artifacts,candidate-artifacts}/SHA256SUMS*`。

## 计数与资源

- 本地 loopback TLS：测试进程内真实 HTTPS（network_started_calls 与请求数一致）；
  真实/付费 provider 调用 0；`provider_enabled=false`；凭据仅 fixture bearer。
- 构建：Release、非 root、-j2，构建树与全部产物/日志在 /mnt/d 私有证据目录；
  资源门全程 df 复核，收尾 /mnt/d 可用 ~522MiB ≥ 512MiB 保留门。
- Git 写入 0（无 add/commit/push/PR）；全局配置未改；Store schema 410 未动。

## NOT_RUN（如实保留，不由本组件替代）

全量 52-ELF 本地树、make test/make ubsan 全矩阵、ASan/Clang/双 umask、
完整 78 注册 Release、Windows 11（NOT_RUN）、私有托管（BLOCKED）未在本地执行；
按 FINAL v6 §8 由精确候选的等价 CI 承担，缺证据项保持 NOT_RUN/PENDING。
集成组合（冻结 Billing+修复 Protocol+本组件同条件三类 estimated 无收据路径、
两轮 native 原件/Store 重开/第三次 wire 正例、clean31≥30/3 任务总门）
归集成计划，不由本组件 GREEN 替代。G1-6 墙钟/停滞/自动跨批不在本模块。

## 本轮测试侧修正（准备缺陷，非断言弱化；详见 baseline-red-summary.md）

1. driver 0-tool 分支临时值悬挂引用（-Werror）改按值拷贝。
2. untenable-envelope 守护对齐 FINAL v6 §3（不可合法序列化的 envelope 不强造
   settlement、预留不释放；基线实测 settle 前抛出、response NULL、spent 300）。
3. driver `schema_retry_cap` 注入从冻结后 JSON 突变改为 system.yaml 文件级
   注入+目录重载（原方式破坏依赖哈希，两态皆败）。
4. 反馈持久化故障触发器限定 Worker 反馈行（原触发器连 Dispatcher/TeamLead
   反馈一并中止，两态皆 0 worker 调用）。
5. evidence cause_code 断言改为 error_name 实际输出（conflict/invalid_input/
   unauthorized；准备稿用 CamelCase，两态皆不成立）。
6. fs_read 的 `operation_for_key` 断言不可用（Store 业务键只认业务工具，诊断读
   两态皆抛 Unsupported）；C++ 侧以设计事实+TLS L3 的 SQL 级 ops=0 断言覆盖。
7. python `correction_common` 的 `network_count_complete` 键位从顶层改为
   report（该字段仅存在于 pump_live 报告）。

上述修正后基线/候选同条件复跑，RED 集与 GREEN 全集如上；失败原件与逐次编译
日志均在私有证据目录保留（red-baseline-*.log、build-*-failed-compile-1.log 等）。
