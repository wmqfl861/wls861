# Compatibility 证据工具缺陷修复报告 v1（2026-11-05）

## 0. 范围与时间声明

本报告只覆盖 **2026-11-05T11:27:35Z 至 2026-11-05T11:41:33Z（UTC）** 内、在
`compatibility` 工作树（分支 `fix/audit-compat-evidence-20261105-adp35vg4`，基线
`efcd4547bd7627fa821a4b7bce097e633b514982`）执行的 N5 证据工具自检/机制验证运行。
报告生成时刻：2026-11-05T11:43:19Z。本轮**没有**重跑历史严格 WAL/SHM 兼容矩阵、
C4、Win11 或产品 95-suite 目录中的任何产品测试，也不据此改写任何历史结论；
历史报告 `n5-compatibility-strict-20261104-v3.md/.json` 字节未变
（SHA-256 `566773960c01167b94628a5a8f1ea73feee3a3eea932d3dc524ba2582c499266` /
`acf54201cdc4cc604a09c4562e50d3d412ff4f0843494b77e6634c07fdd4f15a`，已复核）。

本报告记录的是**工具正确性修复的完成范围**：三个工具/自检文件中已确认的三项
审计缺陷（F-04/F-05/F-06）与一项附带最小修正（F-O3）的修复、其确定性基线反例与
修复后机制回归。它不宣布任何历史 STRICT_PASS 是错误的，不构成产品兼容性通过，
不构成独立审核、发布、CI 或合并批准。

输入与源版本：

| 项 | 值 |
| --- | --- |
| 被测基线（红例时） | observer `50d8efa0…47f6`；lifecycle `fd8b70ea…4895`；selfcheck 原基线 `38fa9422…9999`（红例运行使用回归版 `ce68cdc8…960a`） |
| 修复后（最终绿例时） | observer `1533f062…efdc`；lifecycle `7297b70e…f4e`；selfcheck `e92eb5ed…5ca` |
| 实际运行集合 | 3 次 R1–R4 选择红例（`--only`，原基线源上）+ 3 次完整入口回归（33 case，修复源上）；全部运行、失败与中间版本均保留于模块私有证据根 `audit-fixes-20261105-81xzfrt5/compatibility/implement-01`（run 目录 `red-run-*` / `green-run-*`，含逐次收据），私有原件不随本报告公开 |

## 1. 修复内容

**F-04（完整性与相等分离，observer + 消费方贯通）。**
observer 新增 `snapshot_completeness` / `logical_dump_completeness` 与逐成员缺证
原因（`_member_incomplete_reason`）：逻辑 dump 的有效条件为有效 64 位十六进制
digest、核心载荷类型正确、无 sqlite_error、`quick_check == [["ok"]]`、真实 finally
路径写出的 `close == "ok"`、复制前后成员观测完整，并可用同一规范化核心载荷复核
digest。`diff_logical` 区分 `comparable/observation_complete_*` 与 `logical_equal`
（缺证侧 `logical_equal=null` 并列双方原因码）；`compare_snapshots` 新增
`records_equal`（相同错误字典的字面相等事实）、`strict_equal` 同时要求双方完整且
原有差异为空，逐文件 `content/metadata/raw_equal` 仅在有证据时判定，缺证为
`null` + `evidence_complete=false` + 原因；`stability_check` 不足两轮直接拒绝，
判稳要求每轮完整、每次比较可比较且相等；`frozen_copy` 在建目录前拒绝不完整成员
集合（被拒副本不可能被 SQLite 当合法空库打开），读回必须覆盖含 main 的完整期望
映射；`logical_dump` 在任何 opener 调用前验证副本前缀完整（不完整时
`sqlite_opener_calls=0`），后置观测失败记为不完整原因且不覆盖先前 sqlite_error。
lifecycle 侧 `strict_window` 将稳定、完整复制/独立读回、完整逻辑 dump（含
quick_check 与 close）统一为 OK 前提，可预期 ObservationError 保存失败观察并返回
DEGRADED/WINDOW_UNSTABLE；C0/C1a/C1b/C2a/C2b/C3a/C3b/C4receipt/C5/C7a/C7b 的
最终判定全部加完整/可比较门（online 行要求 main 与非空 WAL/SHM、C3a 另要求持有
writer-lock、其他实际存在前缀成员均需完整证据，空字典不再经 `all()` 滤空后成
功）；`_state2_fixture` 的 CONSTRUCTED 另要求构造副本与构造逻辑 dump 完整；
C5 缺可比逻辑时 `business_product_rows_unchanged=null` 并保持
RECORDED_DEVIATION/ERROR，不再由空 differences 推导业务未变。

**F-05（C5 全生命周期 finally 与自有资源收尾）。**
`row_c5` 在建立 holder/child 前初始化部分记录与所有权状态，从 holder_start 到
close 的每个异常路径进入同一 finally：保留首个异常/中断、已完成 phase 清单与
`error_phase`，后续清理错误仅追加；仅对自己创建并持有的 Popen/连接发信号
（优先 `send_signal/terminate/kill`），暂停已确认的 child 直接 KILL， runnable
child TERM→有界升级 KILL，各 wait/drain ≤5 秒，无无限 `communicate()`；正常路径
仅在 SIGSTOP 确认 T 且 holder commit 成功后继续子进程，未确认即记 deviation 并
回收、不执行 concurrent WRITE；communicate 超时保留 timed_out 后 kill+有界 drain；
child 的正常/部分 stdout/stderr 以原始文件保存并标注完整性。直接调用 `row_c5`
同样安全（不依赖 main 兜底）。`HolderClient.close` 幂等并在所有分支关闭本地
socket，EXIT 回复与实际进程退出分列两条事实，EXIT/等待失败时有界终止/回收自有
holder，强制结束不伪称 graceful SQLite close；`holder_start` 对 socketpair/Popen/
HELLO 各失败边界负责（HELLO 的 `socket.timeout/TimeoutError` 属启动失败：关
parent 产生 EOF 后有界回收，不重试握手、不追加业务命令、不再等一轮 15 秒）；
HOLDER_SOURCE 增加最小 finally（连接/flock/journal/socket 各一次，保留实际 EXIT
close 错误，重复关闭不产生二次业务命令）；main 行循环以幂等兜底 finally 清理
该行 holder，清理未证明完成时后续行显式 NOT_RUN，KeyboardInterrupt/SystemExit
先收尾自有资源再维持中断语义。

**F-06（真实 timeout 建模与 C4receipt 不升格）。**
`_trace_old(kill=True)` 仍在任何效果前抛错；`row_c4` 保持结构化 NOT_RUN。kill=False
路径显式区分轮询窗口耗尽、communicate TimeoutExpired、清理超时并分别记录
phase/reason/action；一旦超时 `timed_out=true` 不可被后续 kill/reap 改回，deadline
后才返回的数据不还原为正常窗口；`TimeoutExpired.output` 只拼接一次，失败只取得
前缀时标明 prefix/incomplete；tracer/pipe 的 read/select、阻塞设置、communicate、
kill 与证据落盘全部纳入本 helper 自有 finally（有界），完整取得 trace stdout 与
原 stderr 字节写新文件（`stderr_text` 仍为摘要）；收据保留 child_role、PID/
start_ticks（读不到为 UNKNOWN）、exit_code（-9 如实为 -9）、真实 timed_out、
终止原因、kill/等待动作、tracer_reaped、stream/output/cleanup_complete，tracee
identity/exit/reap 恒为 UNKNOWN/NOT_ESTABLISHED。`row_c4receipt` 在原等价分类
之外要求双方 BEFORE/AFTER 窗口 OK、双方比较可比较、双方收据完整且 timed_out=false，
新增独立 `pass_eligible` 与 `not_pass_reasons`（区分 traced/untraced timeout、
窗口/逻辑不完整、tracer 流/清理不完整等），双方同样超时不能 PASS，
`traced_untraced_equivalent` 保留按原分类字段算出的字面值。

**F-O3（附带最小修正）。**
`Identity._environment` 的 libsqlite 去重由误比较首字符（`[item[0] for item in
libsqlite]`，对字符串列表取首字符，去重完全失效）改为按路径本身去重，保留首次
出现顺序、既有 maps 筛选与每唯一文件一次散列；无性能测量或声明。

## 2. 基线反例（R1–R4，均在实际原基线源上确定性复现）

依据运行 `red-run-20261105T112954-1354919`（前两次红例运行含新夹具自身缺陷，
一并保留；三次运行收据均绑定原基线源 SHA）：

| 组 | 反例事实（原基线行为） |
| --- | --- |
| R1-L1 | 真实 `diff_logical` 对双方 `sqlite_error`+`close=ok`、无 hash 的记录返回 `logical_equal=true` |
| R1-L2 | 真实 `compare_snapshots` 对相同 refused 成员（alias symlink）返回 `strict_equal=true`、该成员 `content_equal=true` |
| R1-L3 | 上述真实失败比较结果经 mock `_online_row`（两个 DEGRADED 窗口）送入真实 `row_c2a`，返回 `STRICT_PASS` |
| R2 | 真实自有 stock Python child SIGSTOP 确认 T 后，stub holder 在精确 concurrent WRITE 抛错：原 `row_c5` 向外抛出 RuntimeError，child 未回收、holder 未 close，由测试自有有界兜底回收（`harness_cleanup_applied=true`、`runner_cleanup_complete=false`） |
| R3 | inert mock tracer 首次 communicate 抛 TimeoutExpired、kill 后 returncode=-9：旧收据 `timed_out=false`；TimeoutExpired 的部分输出（`partial-trace\n`）被丢弃，trace 文件仅 5 字节；无 stderr 原始文件 |
| R4 | 合成 maps 同一路径 3 段：旧 `Identity._environment` 记录该路径 3 次、对同一文件散列 3 次 |

F-04 原证据边界保持收窄：以上证明的是工具前提遗漏与误判定机制，不证明历史 WLS
矩阵本身误 PASS；F-05 原证据不证明真实 holder 持续泄漏；F-06 原证据不证明完整
正常比较误 PASS。

## 3. 修复后回归（最终运行 `green-run-20261105T114123-1361791`）

完整入口 33 个 case 全部 PASS（22 个既有 observer case 原语义保留、条件性
atime/fchmod 正例本轮成立；11 个新机制 case 覆盖 §1 全部门与边界：双缺证/有效
与无效逻辑变体/复制与稳定性门/共享行门传播/暂停后故障/异常边界矩阵/真实
test-only holder 正常 EXIT 与命令失败后 close 及 HELLO 超时回收/tracer 超时收据/
晚完成仍超时与清理失败不完整及 C4 围栏/libsqlite 去重）。要点：

- R1-L1/L2：`comparable=false`、`logical_equal=null`、`strict_equal=false`、
  `records_equal=true`、refused 成员 `content_equal=null`/`evidence_complete=false`。
- R1-L3：真实 `row_c2a` 对同输入返回 `STRICT_FAIL`，gate 原因列明
  before/after 窗口与逻辑比较缺证。
- R2：行内完成暂停 child 的 KILL/回收与 holder close（`runner_cleanup_complete=true`、
  `harness_cleanup_applied=false`，0.08 秒内完成），首异常与 `error_phase=
  concurrent_write` 保留。
- R3：`timed_out=true`、`communicate_timeout=true`、`exit_code=-9` 保留、
  tracer_reaped/stream/output/cleanup_complete=true，trace 文件为完整
  `partial-trace\nrest\n`，stderr 原始字节文件匹配。
- R4：唯一路径各散列一次、顺序 `[A,B]`、无关库不散列。
- 不完整副本（缺 main）时 SQLite opener 调用数为 0；refused 源在建目录前被拒；
  main-only 家族（可选 sidecar 由完整枚举证明不存在）完整通过。
- 运行环境：普通 UID 1000、ext4 数据盘、每次运行私有 TMPDIR、umask 0077、
  `python3 -B`（Python 3.12.3、SQLite 3.45.1，详见各次收据）；全程零真实
  strace/ptrace、零旧 ELF/WLS 执行、零产品矩阵/C6 suite、零真实模型或付费调用；
  全部 mock 信号/Popen 在任何真实效果前拦截，测试只创建自有小夹具。

## 4. 完成状态分列与保留状态

| 标志 | 状态 | 说明 |
| --- | --- | --- |
| IMPLEMENTATION_COMPLETE | true | 范围限三个授权工具/自检文件与本报告 pair |
| LOCAL_VALIDATION_COMPLETE | true | 仅本轮自检/机制证据（上述 33/33）；不含产品套件 |
| PUSH_COMPLETE | false | 实施会话无任何 Git 写入；commit/push/Draft PR 由根协调者按授权执行 |
| REMOTE_CI_COMPLETE | false | 未发生 |
| PR_REVIEW_COMPLETE | false | 硬审核为另一全新会话，尚未进行 |
| MERGED | false | 未发生 |

保持不变的历史状态：严格 WAL/SHM 兼容 `BLOCKED_NOT_COMPLETE`；C4
`NOT_RUN/NOT_ESTABLISHED`；Windows 11 普通用户 `NOT_RUN`；私有原托管 `BLOCKED`；
`historical_clang_asan_failures_closed=false`；本机 sanitizer/Clang/live_workflow/
Make IPC 未闭环。C5 仍仅主张进程生命期重叠，本轮修复不补造任何 SQLite 阶段或
双方同时访问证据。产品注册项保持 95 个、兼容 suite 120 秒/CLI 20 秒界限、
`ROW_TIMEOUT_S=25`、`HANDSHAKE_TIMEOUT_S=15` 未变（新增 `_CLEANUP_WAIT_S=5.0`
仅为自有资源清理的有界等待，未提高任何原期限）。
