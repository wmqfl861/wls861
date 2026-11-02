# N2a/N2b 常驻回归实施记录

时间截点：2026-11-02。原始集成基线为
`f3fc9bfaf345030ee994097d2a2443e3c290e9fc`，含精确 PR #14 与 #13 的普通本地合并。
本子任务位于数据盘 `/mnt/d` 的独立 `n2` worktree，分支
`test/n2-permanent-evidence-20261102-0k9`，UID/GID 1000。
主执行 Agent 统一安排重编译和 sanitizer；此提交时 C++ 构建/运行 **NOT_RUN**。
该记录不关闭既有本机 sanitizer 失败，也不代签协调者审核。

## 实施范围

现有 `async_coordinator` 注册新增两个方法，29 → 31；现有 `store` 注册新增两个方法，
32 → 34。没有新增 CTest 注册，原 92 项名称、期限、构建参数和生产代码均不变。
这些是已有正确行为的常驻覆盖，**不宣称发现新的生产缺陷，不制造基线红例**。

- `adapter_exception_releases_resource_only_after_original_evidence`：真实 Store/M1、2/2 池及
  原 1/1 预留，三个有界调用按 Normal Dispatcher → Review Worker → Handoff Dispatcher
  共用资源。adapter 经真实 gate 抛出原异常；观测 Ready 尚未提交时后继无 callback/claim。
  结算与反馈后验证原异常 future、INTERRUPTED 原观察、unknown 预算、原请求字节及 FIFO。
  满业务/结果容量时独立 control 仍可完成；最后 close、callback 数和唯一 claim 数收敛。
- `adapter_exception_persistence_failure_keeps_cross_pool_successor_fenced`：在异常观察的结算或
  feedback 两个真实 SQLite 事务点注入故障（同一方法内两个子例）。原结果与未入池后继均保留，
  后继 callback 为零；验证 control 可达、close join、修复后仅 flush 原证据且永久业务 fence 保持。
- `event_original_bytes_hash_and_replay_guards_are_durable`：独立构造包含中文、转义、嵌套及数字
  的原 command，逐列读取真实 SQLite bytes/type；验证 payload/hash/result、原 Actor/CAS、
  epoch/预算/文本和完整历史前缀。重复 replay、foreign session、foreign principal、同键异参及
  陈旧 CAS 均检查原行未变；不以 canonical 调用次数替代语义。
- `event_encoding_limits_roll_back_plan_fences_and_original_rows`：UTF-8 拒绝，以及 body 自身合法、
  完整 event command 的 depth=65 或 bytes>1MiB 的拒绝。保留实际 RUNNING operation，比较
  request/plan/team/task/op/event/原文本/预算行与容量，证明写入和 fence 整体回滚。
  完整 command 恰为 1MiB 的合法正例按原语义提交，并保留全部旧历史。

代码位置：`tests/async_coordinator_tests.cpp`、`tests/async_resource_lifecycle_cases.hpp`、
`tests/store_tests.cpp`、`tests/store_event_compatibility_cases.hpp`。
原 GNU ld 同源序列化计数探针仍为限定工具链探针，未修改。
ADR-092/093 的 original capture、durable reserve/claim、UNKNOWN、Done 后释放规则保持。
Store 的函数内 event 字节复用和原 strict JSON、会话、权限、CAS 与事务边界保持。

## 验证状态

已完成本树源码/契约阅读、变更自查及 `git diff --check`（exit 0）。
父代理提供的旧图 Async 路径缺失、Store 部分解析，故本次采用精确源码回退；没有声称重新索引。
执行前已核 `/mnt/d` 为 `/dev/vdb2` ext4、普通用户、无覆写其他 worktree。

待主执行 Agent 排期构建 `async_coordinator_tests`、`store_tests`；受影响现有 suite 为
`async_coordinator/store/event_store/runtime_types/r24_recovery`，M1 `scheduler` 兼容也应保留。
本记录不填入旧 SHA 的绿色结果。后续实际命令、原日志、退出码及二进制身份由统一执行收据追加引用。
N2c 五方法由主执行 Agent 另跑，N2d runner 工具自检另计，均不增加产品 CTest 注册数。

`IMPLEMENTATION_COMPLETE`：测试代码已实现，待实际编译和独立工程复核。
`LOCAL_VALIDATION_COMPLETE=false`；`PUSH_COMPLETE=false`；`REMOTE_CI_COMPLETE=false`；
`PR_REVIEW_COMPLETE=false`；`MERGED=false`。本子任务未推送、未合并、未调用真实 WLS 供应商。
