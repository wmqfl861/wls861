> **HISTORICAL PROJECTION (REDACTED) — NOT CURRENT ACCEPTANCE.**
> Source: `docs/reports/n5-review-final-delivery-20261103.md` as it existed uncommitted in its
> worktree at head `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` (branch `docs/n5-review-delivery-20261103-74bnxwj5`); original 23403 bytes,
> SHA-256 `d9452d6cda66f6060cdc8bfe9f58cfeb4c53c657044ebaf4921c9e0a5ddffd39`. Redacted public projection created 2026-11-06T10:21:09Z under
> `docs/reports/publication-20261106/`. This is a new version, not a byte copy
> of the original; the original is retained locally and is not in Git.
> Redaction scope: absolute local workspace/evidence-root paths, local task plan / version-record / plan-review references, internal session identifiers.
> No engineering result, status, failure, timestamp or six-state value was
> changed; PUSH/CI states remain the historical values of the report's date.
> **Adjacent qualification:** the six-states block near the end of this report
is a historical snapshot whose final states were never filled in
("正式矩阵／CI运行中；完整状态待真实终态填入"), and shared GCC ASan/LSan and the
original Make-ASan were NOT_RUN at that time. This is not a 2026-11-06
conclusion; current module states are in the backfill README / index.

# N5 独立复审整改交付：2026-11-03

本报告属于新交付截点，原审核、失败报告和原始日志均保留。整体状态仍为
**VALIDATION_BLOCKED**，不代表主协调者批准或任何 PR 已合并。执行依照冻结交接
`d843493fafe158f3a6443bafaa0de95cbf6c05c2`；不重做此前 N5 实现，不实施 014／412。

## 单一产品输入与工作区

- 原产品：PR #25，`c2a5cea64aeea261b214cdfd9a968b53f491110a`。
- 中间集成输入：[Draft PR #31](https://github.com/wmqfl861/wls861/pull/31)，
  `f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc`，tree
  `4dce442a166781ff7ea6e4a68def9a1a1a0e4be6`。
- PR #31 base：`work/n5-final-candidate-20261102-csj777e9`，准确 c2。
  使用新候选的普通本地 merge 保留模块祖先；main 和原 PR 引用未移动，未合并 GitHub PR。
- 最终产品候选：[Draft PR #32](https://github.com/wmqfl861/wls861/pull/32)，
  `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`，tree
  `6dab0e33e7ec9d05c32a65649ce9dfe086285282`，分支
  `fix/n5-stop-gate-20261103-74bnxwj5`；准确父／PR base为#31的f15。
- 数据盘（ext4）；普通 UID/GID 1000，用户 ubuntu。
  根任务目录位于数据盘（路径已去除）。
  中间产品checkout为其`integrated/`，最终产品checkout为`stop-gate/`；
  本报告另在`delivery/`，不修改正在验证的源码。
  构建、临时库、TMPDIR 和私有证据均在该盘；未安装或替换全局工具链。
- 本机Linux6.8.0-147-generic／x86_64，Git2.43.0，GCC13.3.0
  （Ubuntu13.3.0-6ubuntu2~24.04.1）、Clang18.1.3、CMake／CTest3.28.3、Python3.12.3。
  测试解释器保留现有data盘venv Python（路径已去除），
  未解析为系统Python。负载／内存／pressure／cgroup可观察项另存各轮资源收据，未知限制不推定为无限制。
- 运行版本 `0.11.1-r3.3c-audit`，Store 411；历史 main 仍 a6b8559／410，蓝图 v0.5.0
  仍是规划版。013 原字节不改，014／412 未获实施授权。

f15远端Clang停于新增destructor方法，随后同源探针复现Gate确实可丢失停止通知的窗口；
原超时没有栈证据定位具体指令。后续以其上的独立测试修正冻结最终输入。
各表的本机产品矩阵只属于明确列出的 SHA，不使用模块或 c2 的绿色补填。报告提交是文档输入，
它与产品冻结提交的关系和最终源码快照另行登记，不宣称报告 SHA 已本机重跑同一产品矩阵。

## 本批实际变化与模块提交

| 模块 | 提交／PR | 实际增量及限制 |
| --- | --- | --- |
| N5-0 公开方法投影 | `a2ca1055db58dab10aa254029a01c15e1e0c9f18`／[#27](https://github.com/wmqfl861/wls861/pull/27) | 四份完整源码 hash 绑定 59 个既有静态 C++ 标签；动态失败值不公开 |
| N5-0 未来库拒绝 | `3888d95a53572e06825d774aae732d01f7996903`／[#29](https://github.com/wmqfl861/wls861/pull/29) | Store 只新增 40 行拒绝专用预读，SQLite open 之前拒绝显式未来主文件头；真实 WAL/version/metadata/锁/权限仍校验 |
| N5-1 故障覆盖 | `3a2dd55d51e32e7e49ed546ccb4c441d3bd96782`／[#30](https://github.com/wmqfl861/wls861/pull/30) | Store 加 5 方法、runtime 加 1 方法，生产代码不改；T4 回滚、真实 postcommit API 错误、部分 ID、旧身份、损坏／容量和 callback 回收 |
| N5-3 策略／来源 | `8ec950f7ffbf7ece0d9192abc31a60f1c1aa2c29`／[#28](https://github.com/wmqfl861/wls861/pull/28) | 只读策略保守拒绝 Administrators 信任，加机器注册表独立观测与 ACE 负例；不称历史漏洞或 Win11 已支持 |
| N5-2 准备 | `c25f667e3bc55822612fbacf50b7337a3b79823b`／独立文档分支 | D1–D6 决策和验收条件，父候选已普通 merge；无独立 PR，无迁移／生产实现 |
| 新覆盖夹具修正 | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`／[#32](https://github.com/wmqfl861/wls861/pull/32) | 修复测试Gate停止通知丢失，加原5秒确定性回归；生产代码不变，runtime14→15，95注册不变 |

相对 c2，生产差异仅 `src/store.cpp` 的 40 行和 native `readonly_resources.cpp`
的7行；公共头、`db/`下11份原SQL以及历史handoff的schema副本逐字节相同。
最终身份审计核对91个受保护路径均未改变（`final-product-identity-audit-01/receipt.json`，
SHA-256 `ea050ac778226549e9863e044bfc77fae8e9f9d338a5bc0109a7fb9fec83e447`）。
其他变化是测试、工具、注册／CI数量和新文档。
完整文件清单以 PR #31 diff 和配套机器索引为准。非作者集成复核收据
`integrated-diagnostics-nonauthor-review-01/review.json` 绑定 f15/tree4dce，SHA-256
`a8a98fc8d55c67007c58aef4d3c0ca34fa7c224b1e256413933713906c6d213a`。
这些本地代理交叉复核不等于第二模型评测、主协调者正式批准或发布验收。

## 红绿与补充覆盖

| 输入与试验 | 实际结果及分类 |
| --- | --- |
| 原 c2 ELF，同源 CLI 九方法 | 6 PASS／3 FAIL，exit 1；真实创建／删除 sidecar 和 chmod 断言，不是编译失败或删守卫变异 |
| 新 Store 对象＋匹配原 c2 依赖，同源九方法 | 9/9，exit 0；明确为派生候选定向验证，不是干净完整构建 |
| 原 runner，同一新增 grouped-label 自检 | 在缺少静态标签的目标断言失败；工具缺口，不是生产漏洞 |
| 候选 runner 自检 | 60/60；对应源码前后相同，原失败保留 |
| 新持久测试＋原 c2 库，最终 release-04 | Store 30/30、runtime 14/14，受影响 CTest 2/2，0 fail/timeout/skipped/notrun；原 21 份 SIGKILL/reap receipts 仍在 |
| 持久测试夹具前三次 | scope、WAL hook 早装、metadata 校准失败原日志全部保留；这些夹具错误不标为生产缺陷红例 |
| 原 native 库＋同源新策略测试 | Linux 三方法 2 PASS／1 FAIL，命中新政策 Denied 条件；政策增量对照，不称历史权限漏洞 |
| 新 native 候选本机 Linux | 两 umask 各 5/5 suites、30/30 方法；Windows 四个额外方法不在本机代填 |
| f15 原 Gate＋相同停止窗口回归，GCC13／Clang18 | 两次编译成功后均在原5秒目标 `CHECK(completed_from_stop)` 失败、exit1；始终release／join收尾，没有以外层超时充当红例 |
| stop-aware Gate＋相同回归，GCC13／Clang18 | 两次均exit0；只改测试TU中的等待原语和确定性夹具，未改生产协调器 |
| 新测试TU＋f15完整GCC Release库 | 受影响runtime CTest1/1、15/15方法，原180秒suite／5秒断言不变；定向派生验证，非新SHA完整构建 |

未来头预读不授予打开权限，不处理旧 main-header 中尚在 WAL 的真实版本，不使用 immutable
隐藏 WAL。预读 fd 与后续 SQLite open 仍不是同 inode 的安全协议；不承诺活跃库、祖先替换
或 atime 完全无变化。旧 410 ELF 无法被这份新程序补丁追溯修改，Compatibility02/03 的严格
raw WAL/SHM 兼容仍 **NOT_COMPLETE**。正确拒绝旧版与 strict inventory 失败分别保留。

持久新增故障模型通过 SQLite 实际 postcommit WAL hook 报 IOERR_WRITE，并核实际 durable
行／receipt／重放／重开，替换该连接默认 autocheckpoint callback；不是物理磁盘故障模拟，
不宣称 checkpoint 时序完全不变。T4 六种 plan/instruction/AMEND × feedback/CLOSE 故障核
全表单快照零半写。callback 销毁测试实际 flock 证明回收前 Store writer lease 仍持有。

## 本机正式验证矩阵：分别登记冻结输入

<!-- LOCAL_MATRIX_START -->
以下只属于最终`3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`：

| 配置 | passed | failed | timeout | skipped | notrun | 实际退出／证据 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| GCC13 Release／umask0022 | 95 | 0 | 0 | 0 | 0 | CTest0，2171.98s；完整LastTest/JUnit／source与binary前后相同 |
| GCC13 Release／umask0002 | 95 | 0 | 0 | 0 | 0 | CTest0，2141.01s；完整LastTest/JUnit／source与binary前后相同 |
| Clang18 strict | 95 | 0 | 0 | 0 | 0 | CTest0，2029.40s；完整LastTest/JUnit／source与binary前后相同 |
| GCC UBSan Debug／`-g` | 82 | 1 | 12 | 0 | 0 | CTest8，4970.52s；summary SHA `a146522f37a14fcd863411149a1b1b40e99ea5c5ec70298dbf7f25ba4da2afd6`；原90s期限 |
| 共享GCC Debug ASan/LSan | NOT_RUN | — | — | — | — | 仅用原共享入口，95项 |
| 原Make-ASan | NOT_RUN | — | — | — | — | recipe与CTest分开 |

UBSan本轮LastTest254,320bytes，SHA-256
`d1dae8ebd8006e602dc3466945a648cc9142f5b903772c2334da3affe76d1262`；JUnit
120,134bytes，SHA-256 `af65fc75fa3b239b53989f8c075033ac8edf84327182a65cda5e25c117476ba4`。
其失败／超时原件均在`evidence/gcc-ubsan-20261103T104700Z/`，不重写。
只读核其中Store30/30、runtime15/15；方法收据SHA
`d42bfcf04265d30efbc2ee850e8173e179b3711a5d02fafe5c7468d00c689a97`。
这些方法属于95个套件内部，不另计产品注册。
<!-- LOCAL_MATRIX_END -->

中间输入f15的Release umask0022完整95/95、CTest exit0，原LastTest/JUnit和前后身份齐全。
第二种umask0002的原CTest自然运行结束，JUnit记录95/95、原日志935.35秒，源码及二进制
前后相同；但停止尚未启动的后续配置时，外层执行器连带终止了记录器，实际CTest退出码
无法追回。因此该阶段保留 **NOT_COMPLETE／actual_ctest_exit=NOT_OBSERVED**，不从JUnit推断
exit0；观察器自身exit0仅说明归档完成。原chain退出143、计划、stdout/stderr、LastTest、
JUnit、原件摘要和外部补档收据均保留，未标为产品期限失败。
f15本机Clang／UBSan／GCC-ASan／Make-ASan均未由该chain启动，登记NOT_RUN。
后继候选逐配置独立启动记录器并等待自然收尾，不再用信号停止外层chain。

注册为 **95**：原 94 名称和期限逐项不变，只新增独立 120 秒的
`store_open_compatibility`。新 Store/runtime 方法、21 个崩溃窗口和工具方法数不计为产品注册。
正式构建最多 2 jobs，CTest 串行；各 sanitizer 及大型编译互不重叠。共享 GCC ASan 只用
`scripts/run_gcc_asan.py`，GCC 13.3.0、Debug -g、LSan 开启；未复用 Clang cache。
Make 仍执行原 asan target 和用户 CPPFLAGS；它没有 CTest LastTest/JUnit，不能伪装成 CTest。

精确f15与最终3a8725分别完成独立工具检查：每次runner60、即时traceback5、transport observer11、私有证据16、
Windows runner27、image comparator17，合计 **136 unittest 方法**全部通过；native observation
另有3个受控场景通过，两轮分别保存`review-integrated-tools-01/`和`review-integrated-tools-02/`。
工具自检内的合成失败／中断CTest不计为产品失败或产品注册。

## 原输入诊断与未关闭原因

详见 [新 sanitizer 诊断报告](n5-review-sanitizer-diagnostics-20261103.md)。最终候选的正式 GCC UBSan
本机尝试为82 PASS／1 FAIL／12 TIMEOUT／0 SKIPPED／0 NOTRUN，CTest exit 8；失败套件为
`event_runtime`、`acceptance_runtime`、`audit24_capacity_repro`、`r2_rework`、`r2_amend`、
`sse_replay`、`r1_management`、`single_agent_runtime`、`persistent_event_store`、
`persistent_event_runtime`、`live_workflow`、`billing_baseline_e2e`、`r3_evaluation`。
其中`persistent_event_runtime`含原5秒内部等待失败，`billing_baseline_e2e`还记录了
single-agent deadline、pending inbox/feedback/accept_candidate/report_experience 等业务断言；
不能把这些归结为外层超时，也不能由远端绿色覆盖。本轮正式 UBSan 原件保留在
`evidence/gcc-ubsan-20261103T104700Z/`，summary SHA 为
`a146522f37a14fcd863411149a1b1b40e99ea5c5ec70298dbf7f25ba4da2afd6`。

原 c2 完整 ASan
54 PASS／38 TIMEOUT／2 FAIL，全部94尝试；按原Make log实际进入20个recipe，前19成功，
第20个Store子程序21 PASS／4 FAIL，余74未进入。旧报告“21 recipe成功”的措辞冲突保留，
这里依据原log分别登记配方和内部方法，未覆盖原文件。

- 原四个 Make 失败组的选组观察 4/4，不能关闭原全套；相同数值 observer 恢复原25方法前缀后
  24/25，W00 实际 poll=0、原5秒超时，子进程未发目标消息，SIGKILL/reap有证据。
  子Store重开4.013秒、capture0.984秒；采样系统CPU4.01秒、用户0.53秒，约1GiB RSS。
  支持调查继承内存／缺页／校验，尚不能归因某一编译器、磁盘或系统原因。
- 最终候选 formal ASan 运行期间，v4 Make IPC 诊断没有启动。阻塞收据
  `evidence/make-ipc-syscall-execution-05/receipt.json`（SHA
  `529eb107724fdd11be01453916b71b58570a8ccd9b5d5561fbdd88c9f68b104e`）绑定 v4 计划和
  canonical runner，记录产品测试／编译／trace／模型／网络均为0；正式 ASan 完成后若仍有
  安全窗口，才允许按计划最多执行一次。当前不能把 `NOT_RUN` 写成诊断结论。
- 原 ASan ELF 的 Store/runtime/TLS/structured CLI 四代表项为2 PASS／1 FAIL／1 TIMEOUT。
  Store120秒超时、runtime原5秒断言仍失败；TLS10/10和CLI6/6只标此轮 NOT_REPRODUCED。
  CPU采样是下界，完整trace、LastTest/JUnit及前后身份保留，不用它替代完整门禁。
- 历史 Clang 和 live_workflow 原因继续 OPEN；新异步或 sidecar 整改不是旧失败的解释。
  无无计划全套重跑、原断言／payload／期限调整或关闭 sanitizer。

UBSan old→new `event_runtime` 诊断已完成执行前置准备，但尚未运行。最终 v8 计划
`evidence/ubsan-old-new-stage-plan-01/PLAN-08.json`（SHA
`2d745cc0a0d36e8b2ca5d2b489e07e9bbbad3d6a80f6b222acea67402e7c0569`）和 runner
（SHA `ebab1384a87236ddf2d95753ca4582db5702c2fc3303d8877036cf524cc25a7d`）已完成非作者静态复核，
receipt `evidence/ubsan-old-new-stage-nonauthor-review-04/review.json`（SHA
`6877da3f0180cb15a9a713093d895b49c7f98ba21f21203701eca4a8ea20669c`）无执行阻断。
计划保持 old-c2→new-3a 各一次、原90秒 CTest期限、LSan、120秒仅作失控收尾；它是诊断，
不能替代正式 UBSan 门禁。正式 ASan/LSan 运行期间不并发释放该诊断窗口。

## 远端 CI 与源码身份

<!-- REMOTE_MATRIX_START -->
最终产品#32有**7个workflow run／17个job全部SUCCESS，均attempt1**。
Linux Push为[41598903806](https://github.com/wmqfl861/wls861/actions/runs/41598903806)，
实际checkout为完整3a872558；Linux PR为
[41599060395](https://github.com/wmqfl861/wls861/actions/runs/41599060395)，
实际checkout`8390b9787296800be2cc886b25a53acb60c31aea`，父为[f15,3a872558]，
tree与候选6dab相同。两run的12个Linux job均由原日志核实际checkout。

| 配置 | Push结果 | PR结果 | 统计单位 |
| --- | --- | --- | --- |
| GCC Release／umask0022 | 95/95 | 95/95 | CTest注册 |
| GCC Release／umask0002 | 95/95 | 95/95 | CTest注册 |
| Clang18 strict | 95/95 | 95/95 | CTest注册；runtime1.53／1.48s，原180s期限 |
| GCC UBSan | 95/95 | 95/95 | CTest注册 |
| 共享GCC Debug ASan/LSan | 95/95 | 95/95 | 公开JUnit／投影，无fail/timeout/skipped/notrun |
| 原Make-ASan | 95个测试recipe成功 | 同样95成功 | 各59native＋36Python调用；不是CTest注册 |

两份ASan公开投影与两份原始Make日志均核runtime15个START/PASS，含停止窗口和destructor；
两ASan的原59个静态grouped标签也齐全。Clang成功job的`--output-on-failure`没有保留方法stdout，
其15方法原日志登记**NOT_AVAILABLE**，不以别的配置补造。
两个A4 run[41598903775](https://github.com/wmqfl861/wls861/actions/runs/41598903775)／
[41599060445](https://github.com/wmqfl861/wls861/actions/runs/41599060445)的SUCCESS来自metadata，
没有从其原日志核实际checkout；17个唯一jobID与冻结PR rollup逐项一致。
Native两job和source snapshot的原件核验另见下节。

最终远端索引`module-32-linux-remote-01/REMOTE_INDEX-v1.json`，SHA
`0b79a22497443a4f248a2f5016090dfd9d46d7a8c6d0481cd0a9a022cf1ce6fa`；
人读索引使用保留旧版后的`REMOTE_HANDOFF-v2.md`，SHA
`edb13868cda9df55cad8a19639691219115ba755cb29b417ff076329a6beca44`。
摘要hash仅用于字节核验，不等于数字签名。

模块#27／#28／#29各自的7runs／17jobs均成功，catalog分别94／94／95；
其输入与最终95项矩阵分开。旧#30与中间#31各有12个Linux job中的10成功／2 Clang失败，
对应94／95注册；原失败不因后续候选通过而改变。
<!-- REMOTE_MATRIX_END -->

新增覆盖模块#30的Push／PR Clang分别为93 PASS／1 runtime TIMEOUT（94注册）；
中间集成#31的Push／PR Clang分别为94 PASS／1 runtime TIMEOUT（95注册）。四份原日志都
停在新增destructor方法的START，缺该方法PASS。run/job/attempt／checkout与私有原日志分别
留存，不以Make-ASan或其他配置绿色覆盖。测试Gate的失通知窗口已有确定性同源红绿，
但不把此夹具缺陷解释成旧c2的38项ASan超时或Make IPC历史失败。

每个job分别记录run/job/attempt、event、head/base及可得checkout/tree；A4仅metadata的限制保留。
PR synthetic merge 不是 main 已合并；不同 SHA 或 module catalog 不混算。
源码快照只审成员和 hash，并与该提交 git archive 原字节比较，不执行下载归档代码。
Linux产品完整hosted私有LastTest/JUnit没有获准托管目的地；公开ASan artifact始终只是脱敏投影，
即使带原 hash 也不能声称 hosted 私有原件仍可下载。

## Windows 证据与目标限制

<!-- WINDOWS_RESULTS_START -->
最终3a8725产品输入的两个原生job已完成并取证：

| 事件 | run／job／attempt | 实际 checkout | 结果 |
| --- | --- | --- | --- |
| Push | [41598903763](https://github.com/wmqfl861/wls861/actions/runs/41598903763)／125792707157／1 | 3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123 | 5/5 suites、34/34方法，0 fail/skip/notrun |
| PR | [41599060437](https://github.com/wmqfl861/wls861/actions/runs/41599060437)／125793198983／1 | 8390b9787296800be2cc886b25a53acb60c31aea | 同样5/34通过；API核parents=[f15,3a8725]，tree与head相同 |

两job完整LastTest/JUnit逐方法核验，886个source文件动态逐字节匹配本地冻结输入，
4个executable identity maps前后不变。27+17工具方法另记。
最终readback索引`stop-gate-windows-readback-handoff-01.json`的SHA-256为
`bb53b393bea38c1333bef5efc601be9d6fb9826d030ac365260d5094ea1c6cbb`。
Native分析使用保留旧版后的`verified-matrix-02.json`（修正job_id字段引用），SHA
`5e45809d1807f7183f731d6422cd40735da613c7069923468cca7abf39a1316b`；原API／ZIP／日志不改。

最终source snapshot run[41598903835](https://github.com/wmqfl861/wls861/actions/runs/41598903835)、
artifact13213721484，完整ZIP27,979,983bytes，SHA
`90a3e5f466c968af66fc227dd54a16c6120ec5993e6ed162b31771c03f6c70e8`；
tar40,581,120bytes与该提交本地git archive全字节相同，886regular files／963members。
首轮45.108秒传输中止的部分ZIP及收据保留；新编号只延长下载预算至120秒取得同一artifact，
未重跑CI或调整产品期限。

中间f15输入的两个原生job另行保留：

| 事件 | run／job／attempt | 实际 checkout | 结果 |
| --- | --- | --- | --- |
| Push | [41593459570](https://github.com/wmqfl861/wls861/actions/runs/41593459570)／125775278062／1 | f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc | 5/5 suites、34/34 方法，0 fail/skip/notrun |
| PR | [41593557500](https://github.com/wmqfl861/wls861/actions/runs/41593557500)／125775579900／1 | a400da93f2bfcfacbf1dd7d937256bdeed21b2bd | 同样5/34通过；API核parents=[c2,f15]，tree与f15相同 |

两 job 均保留原ZIP（各50成员）、完整LastTest/JUnit和source／binary前后身份；884份源码、
4个EXE在各自job前后不变，跨job源码map也相同。实际系统为
Windows-2022Server-10.0.20348-SP0、elevated、NTFS，MSVC14.44.35207、SDK10.0.26100.0。
27+17工具方法也通过，但不计为34个组件方法或95个Linux注册。
完整私有核验收据在 `evidence/windows-integrated-remote-native-01/`。
Push结果收据SHA `77ddfb14640c1b8003da16d165a40913e9f97f4e730bfad04bcaded05f8373fb`，
PR收据SHA `45d14c263439c9ee0102de2d26c62688755ebccf18efdf6f2b4290757c91727f`。
两输入的真实宿主条件相同；这些组件通过均不能填入Windows11普通用户目标验收。
<!-- WINDOWS_RESULTS_END -->

保守策略禁止把 `S-1-5-32-544` 明确加入 trusted owner/writer；普通 token 与资源信任集合
是两个独立事实。默认 NTFS 祖先 ACL 可能因此拒绝，不修改 ACL 获得通过；部署策略仍由
协调者采纳。未开放 Windows Workspace 写、SQLite/VFS、任意进程、强沙箱或 cleanup。

原 image header 为 `20261025.346.1`，process 为 `20261026.347.1`。新 job 增加64-bit HKLM
只读两字段；process/machine匹配不能抹去header冲突。官方runner .setup_info输出路径和
runner-images两套字段写入路径已固定版本核对；旧VM对应原件缺失，因果仍 **UNRESOLVED**。
**Windows 11 x64／普通非提升用户／NTFS实机验收 NOT_RUN**，用户已确认无目标机器。

本轮 Windows／Compatibility 综合复核收据为
`evidence/windows-compatibility-provenance-review-02/receipt.json`（SHA
`d8ee15541c0dbb6fc0582ee8d4d499e747c407c69cb65707caa8d75ce1d084b1`）。它保留 header
`20261025.346.1` 与 process/HKLM `20261026.347.1` 的双来源冲突为 `UNRESOLVED`，并绑定
c2 Compatibility02/03 9 方法 6/9 红例与 3888d95 候选同条件 9/9 定向绿；活动 WAL/SHM、
TOCTOU、普通用户 Win11 目标及完整 strict sidecar 门禁仍未关闭。

## 私有原件、公开投影及复核入口

本地正式／诊断按实际可得范围保留完整stdout/stderr、进程退出收据、原工具生成的LastTest、
JUnit和源／ELF前后身份；f15第二种umask的缺失退出收据单列为NOT_COMPLETE，未补造。
成功、失败和可收尾中断的持久化已由真实轻量CTest工具自检分别验证；不可捕获SIGKILL不假称
完成。私有目录保持0700，失败记录不重写。托管目的地／ACL／retention／authenticated readback
待单独采纳，继续 **BLOCKED**，不上传原件到公开artifact或擅自换服务。

配套机器索引给出每份摘要／原件索引的本地相对路径、大小和SHA-256；公开文件只放经过审阅
的摘要，不含真实凭据、数据库、二进制或原请求／响应正文。公开投影关联原件，不冒充原件。
证据摘要以完成后的执行收据为准；未到达阶段不标PASS。

## 六状态与剩余工作

<!-- FINAL_STATES_START -->
正式矩阵／CI运行中；完整状态待真实终态填入。PR_REVIEW_COMPLETE=false、MERGED=false。
<!-- FINAL_STATES_END -->

主协调者需要独立复核最小生产差异及完整新证据，并决定是否接受各有限整改、如何处置尚未
闭合的发布门禁。没有权限时不把hosted私有托管或Win11实机改成已验证。N5-2 的
[D1–D6 readiness](../implementation/n5-review-dag-readiness-20261103.md)只列具体选择、代价和正负
验收；PR #21及014／412未采纳，先稳定N5-1，再经批准做三节点汇合，之后才独立审核票据。
本轮不扩展DAG、记忆、MCP、服务化、Windows强沙箱，不调用真实模型、不付费、不操作生产数据。
