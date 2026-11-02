# N5-0 执行验证 v2（本地执行 Agent 待补充草稿）

本草稿记录已提交代码 `77cf51265211d9f766926dfb5e6aad59c1604d94` 的实际执行证据，
不把该提交、单例绿侧或远端绿色等同于 N5-0 整批完成。首次正式本地 Release
两种 `umask 0022 / 0002` 均已结束：**每轮 90 pass / 2 fail / 0 timeout / 0 skipped /
0 notrun，各 CTest exit 8；matrix launcher exit 1**。PR18 两事件各六路远端工作流已
完成并全部 success，但不关闭这两轮本地失败。本地执行 Agent 须补入最终候选的后续完整矩阵。
hosted 私有原件
持久化仍为 **BLOCKED**。原 sanitizer 失败和历史报告保持原有状态。

草稿在独立数据盘 worktree `n5-0-report`、分支
`docs/n5-0-validation-20261102-csj777e9` 编写，基于上述代码提交；未修改正在运行的
`n5-0` 源码、ELF 或资源，也未为编写本报告重跑产品测试。配套
[执行索引](n5-0-execution-index-v2-20261102.json) 给出各回执、完整命令所在文件及 SHA-256。
索引中的 `evidence/` 路径相对于本次本地任务根
`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/`；原始私有日志未进入公开仓库。

## 提交范围与实际实现

- 原始项目基线：`a6b8559018a599d7a806b05942857a56d69a00ce`。
- 本批直接开发基线及 PR18 base：`aa2c592b8aed6509189710cc6f6d69e0959098a8`，
  分支 `work/coordinator-n0-n4-20261102-0k9`。
- 本批代码：`77cf51265211d9f766926dfb5e6aad59c1604d94`，tree
  `74452124aec039bd783cdaf3e7d04845fabb2e19`，分支
  `fix/n5-0-sanitizer-evidence-20261102-csj777e9`。
- [Draft PR18](https://github.com/wmqfl861/wls861/pull/18) 已存在，未合并；
  PR metadata 的 merge SHA 与每个 job 实际 checkout 分别记录，不混用。

相对直接基线，代码提交修改 8 个文件：

| 路径 | 实际变化与边界 |
| --- | --- |
| `src/runtime_types.cpp` | JSON 字符串及对象 key 按普通连续字节段追加；保留原 Boost 转义字节、排序、数值处理，以及完整输出的原 Boost 校验 |
| `tests/runtime_types_tests.cpp` | 原 suite 内新增 1 个字符串字节/UTF-8 守卫方法；覆盖所有 ASCII 控制字节、NUL、Unicode 边界、早期换行后 900000 字节尾部和非法 UTF-8 |
| `scripts/private_evidence.py` | 本地实际退出捕获、排他私有封装、原件关联核验和缺远端前提时明确 BLOCKED |
| `tests/test_private_evidence.py` | 16 个工具方法，真实临时 Git/CTest/Python 子进程及合成私有文本 |
| `.github/workflows/ci.yml` | strict job 增加离线私有工具自检；没有增加私有原日志上传 |
| `docs/implementation/N5-private-evidence-tools.md` | 本地权限、实际退出、原件关联与尚未采纳的远端持久化前提 |
| `docs/reports/n5-private-evidence-tools-validation-20261102.md` | 先前工具验证的原始时间范围，保持不变 |
| `docs/reports/n5-private-evidence-tools-validation-20261102-inputs.json` | 先前工具执行输入清单，保持不变 |

未修改 SQL、third_party、历史蓝图/报告、handoff、原 92 个 CTest 注册名/期限或
sanitizer 配置。新增工具方法与原产品 CTest 分开计数。没有真实供应商、付费 API 或
生产数据操作。

## 原 ELF、派生候选与字节 oracle

原冻结 UBSan ELF 来源为 `7bf3a16d9683ed6a8de65d88023bae9687abd20d` 的既有矩阵。
观察时 checkout 已为 `aa2c592`；`frozen-input-check.json` 明确核对相关源码/测试/fixture
未变且原 ELF 与其冻结 manifest 相符，不能只凭 checkout HEAD 推断 ELF 来源。

同一 `HttpsTransport.test_partial_request_upload_deadline_is_not_free_retry_authority`
使用原 **3500 ms / 900000 字节** 条件和原断言：

| 证据目录 | 执行性质 | 实际结果 |
| --- | --- | --- |
| `https-partial-ubsan-stage-01` | 原 ELF，加 GDB 阶段观察；非 gate | exit 1；外层 13.092 s |
| `https-partial-ubsan-original-02` | 原 ELF 单方法；非完整 suite/gate | exit 1；方法 9.033 s；实际 `peer.prefixes` 为 0，原断言要求 1 |
| `https-partial-ubsan-hotspot-03` | 原 ELF，加 GDB 热点观察；非 gate | exit 1；方法 12.538 s；仍为相同 prefixes 断言 |
| `https-partial-candidate-04` | **派生候选 ELF** 单方法 | exit 0；方法 6.717 s，外层 8.772 s；期限/断言不变 |
| `canonical-oracle-04` | 新字节 oracle 分别链接旧库与候选对象 | 两次 exit 0；它是兼容守卫，不能称为旧版字节错误反例 |
| `sse-ubsan-original-hotspot-05` | 原 SSE ELF，90 s 内 GDB 名称/CPU 观察 | 外层 timeout 实际 exit 124、91.785 s；不推断未观测到的 inner exit，不是通过结果 |

原 HTTPS driver SHA-256：
`7c1a05870db8224be29777e806fe2f8cd2d173cccd1af628669ea7d6fcf22581`。
派生候选 SHA-256：
`c3ba0bab6b583fbcc65e0fd1ebc9cc3b9b06659614b30164cae3f89f34c1a734`。
候选只重编译新 `runtime_types` 对象并链接既有原 archive/driver 对象，**不是 clean
formal build**。通用诊断回执的旧分类字符串不改变这一点；候选的实际 inputs/plan 和
`binary_matches_original_manifest=false` 明确区分了来源。原 ELF、原日志未被覆盖。

热点诊断支持在字符串序列化路径减少逐字追加开销；实现没有移除完整 JSON 解析、
UTF-8、深度、有限数、字节预算或既有业务权限检查。非作者 `/root/concurrency_fix`
已核对源代码与 Boost 1.83 转义实现并要求补显式 `<vector>`，该补充已进入代码。
这份 source-only 复核及单方法候选绿侧不能关闭完整 UBSan/ASan 历史失败。

## 三项后续原 ELF 诊断

第一次 GDB launcher 的 `tree_sample` 使用 `set <= dict`，实际抛出 TypeError；父进程
提前退出后，三个诊断 child 意外重叠。原 launcher/GDB 脚本副本与错误说明保留在
`remaining-diagnostic-harness-error-01`。三个 `*-original-gdb-stage-01` 都缺少
`completion.json` 和 `identity-after.json`，进程采样也未完成，统一标为
**HARNESS_ERROR / INCOMPLETE / DIAGNOSTIC_NOT_GATE**：

| 原 GDB 尝试 | 仅从保留下来的原件可确认的观察 |
| --- | --- |
| structured | unittest 输出 1 方法、37.150 s、OK；不能把该输出替代缺失的控制器完成及前后身份回执 |
| capacity | unittest 18.325 s、1 error；被观察 subprocess 抛出原 18 s TimeoutExpired，returncode 未取得 |
| escaped | unittest 18.345 s、1 error；被观察 subprocess 抛出原 18 s TimeoutExpired，returncode 未取得 |

实际 GDB 栈分别有 7 / 8 / 7 条带 `main` 的记录，包含 JSON/Store/SSE 业务函数。
因此“未到 main”的早期判断不成立，也不能认定 GDB 启动时间消耗了全部期限。
GDB 包装器的返回码与目标 ELF 的退出码需要分别解释；超时后不得推定目标成功或失败。
这些有重叠与缺尾证据限制的观察不用于受控因果比较。

本地执行 Agent 保留原件后修正私有 sampler，另建 v2；去除 GDB 是明确改变的诊断因素，
不是事先采纳的根因。以下三个原单方法按受控顺序各执行一次，没有并行、重编译、
放宽期限或重复直到变绿：

| v2 原 ELF 单方法 | 原来源 | 方法 / 外层秒数 | 真实退出与原期限 |
| --- | --- | --- | --- |
| `StructuredCli.test_capacity_still_blocks_without_silent_truncation` | 原 `7bf3a16` UBSan ELF | 26.946 / 28.021 | 方法 exit 0；三次实际 CLI child 均 exit 0；各原 50 s，61100 字节文件和 16 Workers 不变 |
| `LiveStore.test_effective_live_capacity_can_persist_actual_limit_observation` | 原 `08dad7e` ASan ELF | 11.159 / 12.086 | 方法及直接 child exit 0；原 18 s child / 16 s elapsed、33×100000 字节 SSE comments、精确 3 MiB prefix/UNKNOWN 断言不变 |
| `LiveStore.test_large_escaped_valid_output_fits_original_settlement_event` | 原 `08dad7e` ASan ELF | 4.660 / 5.602 | 方法及直接 child exit 0；原 18 s / 16 s、1 MiB 转义输出公式和 accepted/settlement/hash 断言不变 |

三份 completion、原件 SHA 清单与 source/ELF 前后身份已逐项核验；实际执行区间不重叠，
无 outer timeout、无记录到的剩余 owned PID、无清理 SIGTERM。被动进程样本分别为
115 / 48 / 21 条；按每个 PID/start_ticks 首末观测计数，CPU 增量下界分别为
25.95 / 10.49 / 4.19 s。逐 PID CPU、IO counter 差值、wait channel 和原 TLS 阶段均
留在分析回执；这些有限采样不是完整生命周期计费，也不能反推原失败时的瓶颈。
v2 未取 GDB 栈，没有用 v1 栈填补 v2 的栈证据。

三例状态均为 **NOT_REPRODUCED_IN_THIS_SINGLE_DIAGNOSTIC**。它们只记录原失败在
这次单独观察中未复现，不能关闭旧原因、历史 sanitizer 失败或任何完整 gate。
原件及分析见 `remaining-original-diagnostics-command-02`、三个
`*-original-passive-stage-02` 和 `remaining-diagnostics-analysis-01`。

## 私有工具的原红、旧绿、新超时与定向观察

[先前工具报告](n5-private-evidence-tools-validation-20261102.md) 的 16/16 结果保留其
时间范围。后来的原输入再次执行出现错误，必须同时保留：

| 记录 | 原始退出/结果 | 能证明什么 |
| --- | --- | --- |
| `private-tools-red-01` | 1 方法 failure，exit 1 | 同一真实轻量 CTest 完成后，旧 projection-only 生命周期删除临时 job 时丢失私有原件；能力缺口，不是新增 API 导入失败 |
| `private-tools-green-01` | 16/16，exit 0；方法总计 20.294 s | 当时这轮本地 capture/pack/verify 原件与拒绝路径通过 |
| `n5-0-final-tools-01` 的共享 runner 自检 | 57 方法，exit 0；外层 26.401 s | 工具契约自检，不是产品 sanitizer suite |
| 同目录的私有工具自检 | **15 pass / 1 ERROR，exit 1**；unittest 85.813 s | SIGTERM 方法内原 `subprocess.run(timeout=20)` 超时；不能沿用旧绿宣布此轮通过 |
| `private-sigterm-diagnostic-01` | 原 SIGTERM 单方法 4.216 s，exit 0；外层 5.917 s | 一次 strace/进程采样观察通过；未复现、未解释原超时 |

定向观察只把临时目录保留下来，三份工具源码字节不变。原 20 秒期限、原 assertions
不变；它没有重跑全 16 直到变绿。真实 wrapper 记录 `INTERRUPTED`、signals `[15]`、
child exit 1、`reaped=true`、`forced_kill=false`；CTest exit -15，方法通过原 partial
bytes/bundle 断言。信号从 wrapper 到 fixture 约 80 ms，再到 CTest 约 294 ms；
18 次 fsync 合计 0.238 s、最大 0.0681 s。该次无 watchdog、无残留清理 kill。

原失败 TemporaryDirectory 已被原测试清理，缺少当时的阶段原件。本次成功不足以把
负载、Popen 窗口或 fsync 猜测认定为根因；状态保留 `OPEN_NOT_REPRODUCED_IN_THIS_SINGLE_DIAGNOSTIC`。
没有因这次观察修改生产 capture/runner、放宽期限或重写原失败记录。

## 首次完整 Release：真实失败与源码诊断

普通 UID/GID 1000；checkout、build、evidence、TMPDIR 均在 `/mnt/d`。本轮使用
GCC 13、Release、`--parallel 2` 构建，并按已审查 92 注册清单串行运行 CTest。
配置/build/完整 CTest argv 与输出见 `gcc-release-20261102T120350Z`。

| 阶段 | 当前已取得结果 |
| --- | --- |
| configure / build | exit 0 / 0；完整 build 2125.833 s |
| Release `umask 0022` | **92 registrations；90 pass / 2 fail；CTest exit 8**；1948.131 s；源码和二进制前后身份相同 |
| Release `umask 0002` | **92 registrations；90 pass / 2 fail；CTest exit 8**；751.213 s；源码和二进制前后身份相同 |
| 最终 clean GCC ASan/LSan、UBSan、Clang 与集成矩阵 | **本草稿未取得本地完整完成回执；待本地执行 Agent 补齐** |

0022 完整 `LastTest.log` 为 252792 字节，SHA-256
`237febde9607eb7807e574fd4218cd9ea375425e3b9e98589060b470b5e64c1c`；JUnit 105985 字节，
SHA-256 `efdc8896bd232f247dd8d74668911577dcfef67b192e5fdcd250ccf2bfab38f5`。
0002 完整 `LastTest.log` 为 252780 字节，SHA-256
`5429893aa0f8ce30621fe70c599373102f0379685422aff48f61a146dc4baff2`；JUnit 同为 105985 字节，
SHA-256 `b5f46a646c1590155c455c8eb83c6b3890d454efe3ad43e5d0a85da9de67fe86`。
两轮完整采集与失败同时成立；phase `status=NOT_COMPLETE` 不能改写成 PASS。
总 launcher 实际 exit 1，`reaped=true`、signals 空、`forced_kill=false`；总摘要截止
`2026-11-02T13:24:38.907325+00:00`，全文 SHA-256
`103ef6f682d131a784b1f662687880147f7212157d2845e33671c752481fc55c`。

两项失败的源码诊断如下，没有为诊断再次执行 C++：

1. `doctor_diagnostics_cli`：12 方法中 1 fail。第四方法特意不传 `--source-root`，
   要求由真实 executable 相对路径发现 `SOURCE`；实际得到 Unsupported exit 2。
2. `review_cli_smoke`：14 方法中 3 fail。三项合法 UTF-8 unknown-command 错误表示
   用例只传 command，不传 source/resource/db；CLI 在 unknown-command 检查前先
   discover resources，因此同样先得到 Unsupported，而原断言要求 InvalidInput。

私有 matrix 脚本把 build 放在 `TASK/build-gcc-release-*`，与 `TASK/n5-0` 并列。
`RuntimeResources::discover` 的四个候选都缺 manifest：

```text
TASK/share/wls861/runtime-manifest.json
TASK/build-gcc-release-20261102T120350Z/share/wls861/runtime-manifest.json
TASK/build-gcc-release-20261102T120350Z/resources/runtime-manifest.json
TASK/resources/runtime-manifest.json
```

真正的 `TASK/n5-0/resources/runtime-manifest.json` 存在；build 中的
`wls-runtime-manifest.json` 只是 configure 复制件，不是完整资源树，也不在候选路径。
CMake 的完整 bundle 仅由 install 规则生成。单纯安装 bundle 仍不能满足 doctor 原例
要求返回 root 等于 `SOURCE` 的断言。本地执行 Agent 已决定下一正式构建采用共享 GCC runner/
CI 一致的 `SOURCE/build-*` 布局；不得在进行中旧 attempt 上移动 ELF、补资源或改断言。

该结论由 `doctor-matrix-layout-diagnostic-01` 和
`review-cli-layout-diagnostic-01` 的 source-only 回执固定，不声称找到生产恢复/权限缺陷。
两种 umask 的失败注册均为上述两项。原失败保留，后续布局修正须由新的实际运行证据验收。

## PR18 当前远端观察与源码快照

第一次有限时间快照 `n5-0-pr18-remote-status-01` 保留了当时的 in_progress 状态。
后续最终状态时点以 `n5-0-pr18-remote-status-02/status-summary.json` 的各次 API GET 为准。
Push run [41445539633](https://github.com/wmqfl861/wls861/actions/runs/41445539633)，
PR run [41445621086](https://github.com/wmqfl861/wls861/actions/runs/41445621086)，均 attempt 1、
head `77cf512`，**两个 workflow 均 completed/success**：

| 六个 lane | Push 当时状态 | PR 当时状态 |
| --- | --- | --- |
| strict | success | success |
| strict-umask-0002 | success | success |
| Clang-18 | success | success |
| UBSan | success | success |
| Make-ASan | success | success |
| ASan | success | success |

全部 12 个 job 的原始 checkout 日志已保存并绑定到原 GET 回执：Push 实际检出
`77cf51265211d9f766926dfb5e6aad59c1604d94`，PR 实际检出
`a276ded542b6fc17a21b2f61b613735db19214c4`。先前完成的 9 个 job 复用其已冻结原日志，
另取回新完成 3 个 job 的完整日志；没有伪造新捕获时间或自动 rerun workflow。
PR merge commit API 的 tree 也是 `74452124aec039bd783cdaf3e7d04845fabb2e19`，与 head
代码树相同。每个实际 checkout 都来自该 job 原日志，不把 metadata 当作检出见证。
这些远端结果不关闭本机 layout 失败、原诊断未复现原因或 hosted 私有持久化前提。

源快照 [41445539636](https://github.com/wmqfl861/wls861/actions/runs/41445539636)
实际 success，job `125277578779`、artifact `13142416424`。其 COMMIT/TREE 与
`77cf512` / `74452124aec039bd783cdaf3e7d04845fabb2e19` 相符；tar 内 **828 个文件**
逐文件 bytes/SHA-256 与提交树相符。`source.tar.gz` SHA-256：
`fc31ad70dd78b280d8a1302040ab545b160dbdbc241f2e2f54047cf62f5d0479`。
只下载并读取数据 ZIP/tar，没有提取或执行 archive 代码。源快照成功不是测试通过证明。

## 相关 Windows 切片与仍开放的门禁

Windows 工作在独立 PR17/提交 `af3bd2b14ed497a57453c0261f26d0c6b52190be`，没有
混入本批 `77cf512` 源码。该提交两个 native hosted jobs 已真实通过 5 suites/34 methods、
原 16 runner 工具方法及 12 provenance 工具方法，836 个 native 输入与提交树/源快照相符。
完整后置回执为 `windows-af3-final-delivery.json`；旧 `540fd7d` 和 `8d4bf58` 工具失败
仍保留。该结果只闭合该 hosted native slice 的执行，不代表本批 Linux 矩阵完成。

同 run 原始 Runner Image header `20261025.346.1` 与 inherited ImageVersion
`20261026.347.1` 的冲突仍经工具真实 exit 1 判为 **UNRESOLVED**，没有选择偏好来源。
Windows 11 普通用户完整祖先/权限验收仍 **NOT_RUN**，显式部署 SID/build 策略仍
**POLICY_PENDING**，完整原生运行时仍 **NOT_IMPLEMENTED**。

hosted 私有持久化仍缺已采纳的受限目的地、授权审阅者 ACL、保留/删除策略、最小写入
身份与实际远端读回。当前本地 0700、仓库写权限、公开日志/源快照不替代这些前提。
没有上传私有 bundle 或改名为“私有”的公开 artifact。

## 当前状态与本地执行 Agent 待补项

以下状态针对 N5-0 整批及代码提交 `77cf512`，不针对本报告草稿的发布：

| 状态 | 本草稿截止时 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | false，已提交本地范围；hosted 私有持久化仍 BLOCKED，整批不能关卡 |
| LOCAL_VALIDATION_COMPLETE | false，两种 umask 正式失败；最终候选后续矩阵未完成；工具 SIGTERM 原超时未解释 |
| PUSH_COMPLETE | true，仅上述代码分支/77cf512，PR18 已存在 |
| REMOTE_CI_COMPLETE | true，仅 `77cf512` 两事件 C++20 六路工作流与源快照；不关闭本地失败或其他验收 |
| PR_REVIEW_COMPLETE | false，Draft PR；源代码非作者阅读不等于正式批准 |
| MERGED | false |

本地执行 Agent 后续应在新证据上补入：布局正确的最终候选完整矩阵、原 sanitizer/历史兼容
及计费 E2E 边界、最终整合提交的源码/ELF/CI 清单。正式审阅与持久化政策决策仍由主协调者负责。
保留本稿引用的全部原失败、超时和诊断限制。
不从这批结果推断真实模型或付费调用授权。
