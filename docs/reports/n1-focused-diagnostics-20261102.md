# N1：2026-11-02 聚焦诊断与受控 SSE 修复记录

日期：2026-11-02 UTC。范围截止于本报告索引所列诊断及 01:09:25 的调试工具后验检查。
本报告记录 **局部诊断、最小修复及未解决边界**，不是整合候选的正式矩阵报告。
SSE 容量场景在受控替换一个库成员后通过原期限；原 UBSan async ELF 的一次完整单套件运行
也通过。原 partial-upload、large-escaped、完整 SSE 超时与历史 Clang 问题没有因此关闭。

执行者：主 Agent `/root` 完成 N1a/N1b、调试工具和 SSE 受控实验；
`/root/delivery_report` 完成 N1c 两次诊断及本文证据核对；
`/root/phase_observation_review` 提供下述限定范围的非作者静态复核。
这些角色记录不等于主协调者正式采纳、PR 批准或发布。

## 输入、构建身份与计数口径

| 输入 | 完整 SHA | 本报告中的用途 |
| --- | --- | --- |
| 原 code08 | `08dad7e815b9c3c84d3bd2393818b2030a6e0cde` | 四个原 selected 方法、两次 GDB 观察、原 ASan/UBSan 库与 ELF、N1c 原函数及资源 |
| N0 整合基线 f3 | `f3fc9bfaf345030ee994097d2a2443e3c290e9fc` | 候选分支的正常本地整合起点；两个父为 PR14 `4289589384d75f29d42233cbaad256ceb711e9bf`、PR13 `ceaa9b3e18618c592345001dfc597bdaf579cf81` |
| SSE 修复 2ff | `2ff3b759469152d20ceb4a45a120f699189c4ee0` | f3 上的两行生产修改和一个新行为方法；受控库替换只取 SSE 生产源码 |
| 测试编译修正 756 | `756946d2a812de3d3a22d800beb244e943c7f47e` | 仅修正两个 JSON/string 比较表达式；生产修改与 2ff 相同 |

本批环境为数据盘上的普通用户 UID/GID 1000，GCC 13.3 Debug；ASan 路径保持
`-fsanitize=address -fno-omit-frame-pointer` 和原严格警告，UBSan 路径保持
`-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`。
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`、
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` 保留。
UBSan 进程继承 ASAN_OPTIONS 不表示其 ELF 启用了 ASan。
各轮使用独立数据盘临时目录、FakeModel 或项目既定 loopback TLS 合成夹具；没有使用生产数据。

原 selected 诊断冻结并核对 794 份源码及 39 项 ELF/解析后库身份；N1c 也核对原 794 份源码、
ELF、库、构建控制文件及原日志。实际命令、cwd、allowlist 环境、前后身份和进程回收回执
通过 [证据索引](n1-focused-diagnostics-20261102-index.json) 定位。
私有原件仅索引路径、大小和 SHA-256；本文不复制模型内容、完整配置/doctor、请求正文或 header。

一个 selected 方法不是一个完整 CTest 套件；async 的 29 个方法属于同一个套件。
受控 archive/ELF、派生 TU、原 ELF 单套件和正式整合构建分别记账。
所有本批结果均不回写原完整矩阵的失败/超时分母。

## 四个原 ELF selected 方法

以下四轮源码均为 code08，原 ELF 未修改。表中“方法”时间来自 unittest 原输出，
“观察器”时间来自 completion 回执；它们不是被测子进程的同一个计时边界。

| 原方法 | 本次实际终态 | 计时与退出 | 能确定的事实及限制 |
| --- | --- | --- | --- |
| `HttpsTransport.test_partial_request_upload_deadline_is_not_free_retry_authority` | 被测子进程 **15s TimeoutExpired** | 方法 15.569s；观察器 15.986s；launcher exit 1 | peer 已 listening；关闭时 accepted/prefixes/requests 均为 0。此次没有走到历史 `len(prefixes)==1` 得 0 的断言，不能把两种失败合并成已确定根因 |
| `LiveStore.test_effective_live_capacity_can_persist_actual_limit_observation` | 被测子进程 **18s TimeoutExpired** | 方法 18.490s；观察器 18.699s；launcher exit 1 | peer 收到请求、完成合成响应发送并关闭；进程随后仍未在原 18s 内结束。这是后述受控修复的原红例 |
| `LiveStore.test_large_escaped_valid_output_fits_original_settlement_event` | 被测子进程 **18s TimeoutExpired** | 方法 18.329s；观察器 18.637s；launcher exit 1 | 响应约在观测开始后 1.537s 发送完毕。没有走到历史 17.29s 不小于 16s 的 elapsed 断言；此轮也未对它执行修复后绿例 |
| `StructuredCli.test_capacity_still_blocks_without_silent_truncation` | **PASS** | 方法 26.361s；观察器 26.678s；launcher exit 0 | 原 `pump-fake` 50s 期限未改变；历史 50s TimeoutExpired 本次 **NOT_REPRODUCED**，没有据此归因或宣称已修复 |

前三轮不是新的外层 CTest Timeout；它们是 selected 方法内的被测子进程 TimeoutExpired。
`outer_timeout=false` 只说明外层观察器正常取得错误结果，不证明被测产品进程正常退出。
四轮均保留原 traceback、源/库身份、数值阶段和已回收的自有进程记录。

## 调试工具、输出污染与有限栈样本

初次 perf/GDB 试命令只有会话工具回显，没有独立文件回执，本文不补造早期日志。
`tools/gdb/download.receipt.json` 记录 00:26:30 的 `apt-get download` exit 0；
包和依赖仅在数据盘解包，保留两版 deb SHA 清单，没有系统安装或 sysctl 修改。

主 Agent 后补的 `n1-debugger-preflight-postcheck-01` 是 **01:09:25 新采集的后验检查**：
GDB 15.1 `--version` exit 0，`perf stat -- true` exit 255；当时
`perf_event_paranoid` 原字节为 `4\n`、`ptrace_scope` 为 `1\n`。
这不是早期预检回执、历史原因对照或产品测试，缺失的早期文件证据仍然缺失。

GDB v1 对原 partial-upload 方法采了六个只含函数名的栈样本，观察到请求编码、
canonical JSON 和结构化上下文路径。GDB 停止通知污染了产品 stdout，原 JSON parser
先发生 `JSONDecodeError`；launcher exit 1，方法 8.178s、观察器 8.522s。
该轮属于 **观察器污染导致的无效业务诊断**，不是新的产品红例，也不是产品通过。
未以这六份样本证明 partial 的历史零 prefix 原因。

v2 将 debugger console 单独重定向，再对原 capacity 方法采六份样本；产品仍发生原 18s
TimeoutExpired。样本 0 位于 YAML/config 恢复路径，另外五份位于 `Frames::feed` 路径：
其中两份包含 allocator/operator new 与 string 构造，一份在 string 构造，一份在 `within`，
一份在 string 析构/operator delete。不能将五份都说成直接采到 malloc/free。
这支持选择逐字节消息构造进行最小修复，不能证明它是全部延迟的唯一来源。
调试器会改变时序，六份有限样本也不是完整 CPU profile。

## SSE 修改、编码工具使用及独立复核

2ff 仅将 `src/sse_replay.cpp` 匿名 namespace 中的
`within(bool, const std::string&)` 改为 `within(bool, std::string_view)`，
并在失败分支构造 `std::string(message)`。原逐字节成功检查不再为静态错误文本先构造 owning string；
原比较条件、错误 code/text、UTF-8、流状态、容量、取消、原字节及 UNKNOWN 守卫保留。
这不表示整个 SSE 路径无分配，也没有改变公开 API、SQL 或 Schema。

生产两行由既有本地 **ZCode 编码工具**产生，新增行为方法由 `/root` 添加。
两次编码工具使用与 WLS 产品调用须分开：

- 第一次 240.016s 超时，completion 的 exit_code 为 null，没有最终 usage 回执；
  主 Agent 的安全摘要记录未应用修改。请求数和费用均不能推定为 0。
- 第二次 71.502s、exit 0、leader 已回收。私有原输出的 usage 与安全摘要一致，
  **报告 4 个模型请求、91,366 input tokens、1,362 output tokens、92,728 total tokens**；
  其中 cache-read 74,624。没有货币费用总额，不推定免费。
- 本批记录的 **WLS 产品真实供应商模型调用为 0**。这不等于编码工具没有模型请求，
  也不授予后续真实 WLS 调用或付费接入权限。

`/root/phase_observation_review` 对 f3 上的未提交冻结 diff 做了非作者静态复核，
diff SHA-256 为 `b9251076a653e3490c67ce758d442f20b7390ec94f45282d9628893466a99dd1`。
其复核覆盖 14 个静态字面量调用点的生命周期、失败时消息所有权、原限额与新增行为断言，
没有发现该范围的阻断项；它发生在后述编译修正之前，不冒称审阅了最终整合提交或完成运行验收。

新增方法 `long_comment_preserves_raw_bytes_and_exact_line_boundary` 使用实际 SSE replay：
65536 字节 comment 跨 chunk 和 CR/LF 分割时验证原 raw/hash、UTF-8、binding、nullable cost；
多一个字节时验证原 `stream_limit` 文本、完整保留 raw、准确 parsed 字节数、无工具和无重试。
它是一个行为兼容方法，原实现也应满足，不能把它称为性能旧版失败反例。

## 受控红绿、编译错误与仍失败的 SSE 整套

`n1-sse-controlled-build-01` 从原 code08 ASan archive 的**副本**中替换唯一
`sse_replay.cpp.o`，采用 2ff SSE 源码和原 GCC13.3 Debug ASan 严格参数，再链接原 driver object、
原 kernel 库和原动态库。输入清单记录唯一生产差异为 `src/sse_replay.cpp`；原 archive/ELF 保留。
这是受控派生二进制，不是 f3 或 2ff 的全量新 CMake 构建。

SSE 源编译、单成员替换和 driver 链接均 exit 0；新增测试首次编译 exit 1，原因是
`boost::json::value` 与 `std::string` 比较不匹配。此时产品测试尝试数为 0。
756 将两个 hash 比较改为 `std::string(value.as_string())`，没有降低断言或改变生产修复。
**该编译错误是新增测试的类型错误，不能作为原产品缺陷反例。**

| 受控实验 | 实际结果 | 解释范围 |
| --- | --- | --- |
| 原 capacity 方法，原 fixture、18s 子进程期限及 16s elapsed 断言 | **PASS，exit 0**；unittest 5.856s、观察器 6.206s、外围命令回执 7.595s | 三种时间边界分别保留。与原 18s TimeoutExpired 构成此场景的受控红绿；不能代替完整整合验证 |
| 新长行行为方法，756 测试源码及受控 ASan 库，独立派生 TU | **1 方法 PASS，exit 0，0.265548s** | 验证该方法的原断言；不是额外 CTest，也不是整个 SSE 套件 |
| 完整 SSE 测试程序，原 90s 套件期限 | **首个既有 every-split 方法只有 START，0 PASS，0 FAIL marker，31 方法未开始** | 外围 launcher exit 1，内层正常退出未观察到；是直接套件诊断超时，没有 CTest 注册执行结果。根因 OPEN |

capacity 的绿例保留了业务拒绝结果，而非伪造业务成功：数值观察为 `limit_exceeded`、
attempt `ERROR`/accepted=0、`total_limit`、无工具，真实 settlement 存在、ledger consistent；
unknown cost 保留 100，重放没有发送或新发送权。本文只报告数值/状态，不公开原 response/header。
large-escaped 与 partial-upload 没有取得对应修复后绿例。

| 二进制身份 | SHA-256 |
| --- | --- |
| 原 ASan `https_transport_driver` | `52204f0c0be513ea800958321950ae80ada3020b69a023f9a5f55e2cc935e244` |
| 原 ASan `live_model_store_driver` | `663d039f0f9d6cab7421e3a78ea04933844d39a82ed88aca3b659eb2596d42a3` |
| 受控 SSE 替换后的 `live_model_store_driver` | `24ca347ddebb57f3e66b5714c7e99b98b9b7e0cdfa2973892475832b3d253d6e` |
| 新长行方法派生 ELF | `299ab33da363aec33e7cb4af131038fca72c8ef5cec09587e0f5b97d43ad3aed` |
| 原 UBSan `wls` | `986355051db9f0de94dddbbb4f9c8ec574b306222ae555f8ec7d4be275481994` |

## N1c：方法隔离与原 ELF 全 29 方法

原 code08 UBSan 完整日志中，`async_coordinator` 在 180.02s 超时：前 27 方法 PASS，
方法 28 `resource_fifo_spans_normal_review_handoff_and_allows_siblings` 只有 START，
方法 29 未见 START；没有 FAIL/RESULT/UNTIL-DEADLINE。没有原方法时间，不能从缺少 marker 推断死锁。
原构建日志、原执行证据日志和审计私有副本的全文 SHA-256 一致：
`0ca37c0bc52fabaf75dd59fa621419b786d49e7380e05bc9f76ed050046c383b`。

| 本轮唯一一次实验 | 输入及原约束 | 实际结果 |
| --- | --- | --- |
| `n1c-runtime-run-01` | 派生 TU 直接 include 原测试；逐字复制原 main 的 SchemaRegistry/六份 SQL 初始化；原库、原方法和 5s 断言；60s 独立诊断上限 | 1 方法 PASS，exit 0；初始化 0.101566745s、方法进程内单调时间 **2.808231052s**。回执 3.339s 包含退出后采集，不能代替方法时长 |
| `n1c-runtime-original-run-01` | 不编译，直接运行原 UBSan ELF 的全部 29 方法；整套原 **180s** 和内层 **5s** 不变；新日志目录 | **29 START、29 PASS、RESULT 29 passed/0 failed，exit 0，60.898724s**；stderr 0 字节、无终止信号 |

原 ELF SHA-256 为 `0637684240c9fa3149b36da2f5efd0f02b359aa16a544b217e2ad03680df3890`，
Build ID `b55b3af10d19238c6f70b592321a56a714e17c89`。
方法 28 派生 ELF 为 `87d68e3bb52a03b1806ca856a0af462fcd0cbbf27915538956cd43879e615261`，
Build ID `842f1362a3ab97dc296b2df4081ef3da4c3419f6`；两者不混称同一二进制。

原 ELF 运行中，方法 28 的外部 START/PASS 到达时间分别为 56.650178s/59.377260s，
间隔 2.727082s；方法 29 在 60.885459s 输出 PASS。
最长两个方法是原双审核容量与公平轮转方法，外部间隔约 13.227s/10.864s。
这些是输出到达时间，含管道/调度/观察器延迟，不是函数内部精确耗时或栈归因。
当前没有逼近总期限，**累计成本耗尽 180s 和旧阻塞均未在该次运行复现**；历史缺失的时间仍缺失。

第一次取得 6 次进程数值采样：到约 2.555s 时累计 2.39 CPU 秒，主线程采样为 runnable；
线程 futex 状态不足以证明死锁。第二次取得 122 次数值采样、15 次 cgroup 层级观察，
自有子进程累计 user/system CPU 约 56.079s/1.108s。
第二次从实际 `/proc/self/cgroup`、cgroup2 mount root/mountpoint 和 PID 归属核对实际叶节点，
并观察叶节点、两个中间祖先和可见挂载根共四层：前三层 cpu.max/memory.max 读为 max，
根层相应文件缺失明确为 UNAVAILABLE；可读层未观察到 throttle 或 memory event 增量及配置变化。
祖先计数包含其他工作，分层读取不是原子快照；不可见限制及历史宿主状态未知，不能据此认定环境根因。

两次诊断均前后核对身份、保留退出码并回收自有进程组，无残留；执行槽及时释放，没有自动重跑。
原 ELF 这一次单套件通过不覆盖原 UBSan 完整矩阵的 64 PASS/2 FAILED/26 Timeout，
也没有改变原日志、套件期限或历史 Clang 状态。

## 未闭环项与后续证据归属

| 项目 | 本报告截止时状态 |
| --- | --- |
| 原 partial-upload 零 prefix | **OPEN**；本次原 ELF 15s 超时，GDB v1 被输出污染，无对应修复后绿例 |
| large-escaped 历史 elapsed 失败/本次 18s 超时 | **OPEN**；没有取得对应修复后的运行结果 |
| 原 capacity 18s 失败 | 有限定输入的受控红绿；正式整合回归与完整 SSE 边界尚不能由它替代 |
| 完整 SSE 首方法 90s 超时 | **OPEN**；0 PASS、31 未开始，保留失败和未执行分母 |
| UBSan CLI 原 50s 超时 | **NOT_REPRODUCED**；本次原方法通过，无根因关闭声明 |
| UBSan async 原 180s 超时 | **NOT_REPRODUCED**；派生方法及原 ELF 单套件通过，历史根因 **OPEN** |
| 历史 Clang/其他完整 sanitizer 失败 | **OPEN**；`historical_clang_asan_failures_closed=false` 保留 |
| 最终整合候选完整矩阵、CI、发布 | 不属于本报告的已执行证据；由主 Agent 在另一个精确输入的新执行报告中记录 |

本文编写阶段只读核对实际证据及 Git 源码差异，没有执行归档、新增产品运行或修改测试期限。
后验工具检查、编译器错误、观察器污染、selected 方法与正式产品套件分别计数。
本文不授予整体 `IMPLEMENTATION_COMPLETE`、`LOCAL_VALIDATION_COMPLETE`、`PUSH_COMPLETE`、
`REMOTE_CI_COMPLETE`、`PR_REVIEW_COMPLETE` 或 `MERGED`。
证据归档/公开选择仍需主 Agent 按原隐私边界处理；完整私有原件通过索引与 hash 交回。
