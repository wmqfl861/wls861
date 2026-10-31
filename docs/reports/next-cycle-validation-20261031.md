# A0–A5 执行索引与验证记录（2026-10-31）

本报告区分实际实现、执行证据和正式验收，覆盖本轮获准 A0–A5。
它不批准 main 合并，不启动 B0–B6，不改变运行版本
`0.11.1-r3.3c-audit`、Store 410 或已冻结请求。
报告发布后的精确提交、源码快照、CI 与本机门禁结果保存在同轮
`evidence/final-publication/` 的发布后收据；未完成项不会因提交存在而转为通过。
真实模型、付费请求、生产数据操作均为 0。

## A0：安全接手与输入

实际用户 ubuntu，UID/GID 1000。Ubuntu 24.04.1、Linux 6.8.0-147-generic、x86_64；
`/mnt/d` 实际挂载 `/dev/vdb2`，ext4，独立于系统盘，接手后本轮工作期间约 48 GiB 可用。
全部 checkout、构建、SQLite fixture、临时文件和原日志位于：

```text
/mnt/d/codex-workspace/wls861-next-20261031-065247/
  integration/       固定原 PR #4，正式基线门禁期间始终不修改
  sanitizer/         A1 日志与门禁集合自检
  offline/           A2 离线验收包
  coordinator/       A3 单协调写者
  fair-scheduling/   A4 公平及双预留
  windows/           A5 原生基础
  combined/          各模块正常 merge 后的集成候选
  builds/ tmp/ evidence/ publication/
```

私有 TMPDIR 属于本轮普通用户，未修改全局 `/tmp`、挂载、系统安全策略或其他会话权限。
保留原 worktree 和未提交成果；没有 reset、clean、强推、main 写入、自动合并或部署。
GCC 为 `/usr/bin/g++-13` 13.3.0；Clang 18.1.3；CMake/CTest 3.28.3；Python 3.12.3。
沿用测试解释器 `/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python`，
未解析为系统 Python，也没有安装或替换系统工具链。

接手时 main 仍为 `a6b8559018a599d7a806b05942857a56d69a00ce`；
R2.4、蓝图、边界覆盖与 GCC 对齐形成祖先链，实际基线为
`772feaee62086ddb0e7f605a7472721d3bc61406`。
PR #4 Push [41162941508](https://github.com/wmqfl861/wls861/actions/runs/41162941508)
和 PR [41162986326](https://github.com/wmqfl861/wls861/actions/runs/41162986326)
六路成功。PR 实际检出为 `512ebab34119ecb7c3b66dfbf53c4cd3e3eb1806`，不混称 head。
其源码快照 714 个文件逐项字节/模式匹配，树为
`83743319f6287478ebb1ba5543a1c55c2a0bf670`。接手原证据位于 `evidence/a0-remote/`。

## 模块、接口和直接依赖

| 模块 | 实现与范围 | 分支 / Draft PR / base |
| --- | --- | --- |
| A1 | 即时 flush 失败 traceback；共享 GCC runner 增加预审名称/期限集合核验 | `fix/sanitizer-followup-20261031` / [#6](https://github.com/wmqfl861/wls861/pull/6) / PR #4 |
| A2 | 原 R3 v3 五任务 × multi/single 的十槽离线包、干跑、执行、来源/账目校验及人审模板 | `feat/offline-acceptance-20261031` / [#7](https://github.com/wmqfl861/wls861/pull/7) / PR #6 |
| A3 | 单所有者 Store、事务外异步 FakeModel、原捕获身份、持久唯一 claim、有界业务/控制/结果容量 | `feat/coordinator-async-20261031` / [#8](https://github.com/wmqfl861/wls861/pull/8) / PR #6 |
| A4 | 原 M1 Scheduler 请求/团队轮转、跨池同资源 FIFO、管理/Worker 双预留、有限诊断 | `feat/fair-scheduling-20261031` / [#9](https://github.com/wmqfl861/wls861/pull/9) / PR #8 |
| A5 | 独立原生 config/Scheduler/公共资源/能力诊断可执行切片，未支持业务拒绝 | `feat/windows-native-foundation-20261031` / [#5](https://github.com/wmqfl861/wls861/pull/5) / PR #4 |

集成候选 `work/next-cycle-integration-20261031` 以 A4 为直接基础，正常合入 A2 和 A5；
原 PR #1–#4 的 base 未改变。执行 Agent 的工程交叉复核不等于第二模型审核或协调者正式批准。

A3 的关键源码为 `include/wls/async_coordinator.hpp`、`src/async_coordinator.cpp`、
`OfflineRuntime::reserve_captured_call` 和事务内 `Store::claim_offline_call`。
仅原宿主线程访问所拥有的 Store；回调执行模型等待，返回原 session/request/task/version/epoch。
返回后不补发当前 Actor。首次派发仍需持久准入与唯一 claim；精确重放不再次调用模型。
结算/反馈失败停止新业务并保留原响应，补存证据不重做业务效果。关闭先等待回调退出，再释放 Store。
此范围没有自动 inbox pump、live 适配器、文件/进程/提交执行器或服务化入口。

A4 复用 `Scheduler::submit_fair`，保留匿名旧接口 FIFO 和同资源顺序。
默认管理/Worker 4/8、预留 1/1，普通执行最多 3/7；业务队列不能占用两类审核预留或独立控制容量。
宿主限制需与原冻结上限/预留兼容，不改变旧快照。等待用户释放执行槽而不释放 UNKNOWN 账目。
本批只验证审核类别容量，不声称独立业务审核运行器已经实现。

接口和设计见 [ADR-092](../adr/092-single-writer-async-coordinator.md)、
[ADR-093](../adr/093-request-team-fair-scheduling.md)、
[A3 契约](../implementation/A3-async-coordinator.md)、
[A4 契约](../implementation/A4-fair-scheduling.md)、
[离线包](../implementation/R3-offline-acceptance-package.md)与
[Windows 基础](../implementation/windows-native-foundation.md)。

## A1：原基线、历史失败与日志改进

原基线先执行 runner 自检 20/20、`--help` 和工具链 preflight，再执行唯一已有入口：

```sh
python3 scripts/run_gcc_asan.py \
  --data-root /mnt/d --compiler /usr/bin/g++-13 \
  --python /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python \
  --expected-tests 89
```

正式基线使用原 Debug `-g`、严格警告、ASan/LSan、普通用户和 umask 0002，串行运行原全部 89 项。
原始日志与前后身份位于
`integration/build-gcc-asan-20261031T070135Z-37b2f398/evidence/`，外层记录/资源观察位于
`evidence/a1-baseline-01/`。该轮有原套件超时和断言/错误，不能标为成功。
完整结束后按 JUnit 单独列出通过、失败、超时、跳过和未运行数；没有完整终止记录的运行始终未完成。

已观测 `live_model_store_tls` 的 `effective_live_capacity` 子例触发原 18 秒子进程期限，
`large_escaped_valid_output` 的 `accepted` 为 0 而要求 1。`live_workflow` 原 300 秒截止前
出现 cross-task 和 model-exhaustion FAIL；原汇总 traceback 被整组超时截断。
`billing_baseline_e2e` 在原 900 秒截止前已有子例失败。原始输出保留，不能简化为“仅超时”。
资源观察记录 CPU steal、内存、cgroup、交换和存储；这些观察本身不证明根因。
未杀其他会话进程、未更改宿主策略，也未把重跑到绿色作为验收方法。

历史 Clang 18、Debug `-O2 -g` 的 39 项结果仍是 29 通过、10 超时、CTest exit 8；
原 2949.68765 秒日志保留在上一轮数据盘 evidence。
原 live_workflow 的 model-exhaustion FAIL 缺失 traceback 不能事后编造。
已有单项通过和 ext4/RAM 诊断均不足以关闭整组失败；状态仍为 **UNRESOLVED**。
本轮保留一个原 Clang 二进制/原测试顺序/原 300 秒界限的诊断窗口，
仅加入即时日志工具，结果单独放在 `evidence/a1-clang-workflow-full-diagnostic-v1/`；
它不是新候选完整门禁，也不代替历史 39 项。

新增 `tests/immediate_unittest.py` 在原 unittest 先登记失败/错误/subtest 后立即输出并 flush
原 traceback；不改变断言、顺序、异常、退出码或 deadline。自检先让子进程失败，再用确定性
同步停在下一项，由父进程中止，核验失败原因已保存。原 runner 对照为 3/4、exit 1；候选 4/4、exit 0。
这四项是工具自检，不计入产品套件。runner 名称/期限集合自检从 20 增至 23，模块接续后 24/24；
同数量的测试替换、遗漏、关闭和期限变化均拒绝。集成完整集合是 92，而不是仅自动读取当前数量。
原 89 项名称/期限保持逐项相同，`historical_clang_asan_failures_closed=false` 未改。

A1 工具分支 `1c279f6770f5f4440adcf6b8ee2f51e2401d9a74` 的
[Push 41182062900](https://github.com/wmqfl861/wls861/actions/runs/41182062900)和
[PR 41182174152](https://github.com/wmqfl861/wls861/actions/runs/41182174152)
六路均成功。该结果只属于对应远端输入与环境，不把本机失败或历史失败改成通过。
两轮六个 job 的实际 Push checkout 都为上述 head；PR checkout 都为
`9150ffd5fedf640c1279ffd2004faf7335666ee6`，API 核验树相同。
ASan 两轮各 89/89，无失败/错误/跳过，718 个输入及 60 项构建身份前后稳定，UID 1001、
LSan 启用。源码快照 41182062897 的 artifact 13034813605 逐项匹配该树；
归档 SHA-256 `e9b7b26206969ee1d5f814ae92b9505c150b7e99d2c51c0b83bcedc29a9cbe49`。
完整收据 `evidence/a1-final-remote-20261031/remote-completion-receipt.json` SHA-256 为
`1d7ab70eaa1a730d36b9e04e2875fc45c7bfd70ed5a222ed5317d35f829d759c`。

## A2：离线执行和证据边界

入口 `scripts/r3_offline_acceptance.py --help` 提供 prepare、dry-run、run-fake、report、
check-real-template；版本化材料在 `scripts/offline_acceptance/v1/`。
复用原 `evaluate_r3.py` 和 Store CLI，固定十槽，不另造评分系统。
缺失、失败、UNKNOWN、取消与未执行都保留在分母；真实质量和费用字段为 NOT_RUN。
两条件共享材料、模型、工具、预算、返工限制和验收规则，single 使用原完整逻辑 Agent 历史。

初版纯 Python 合同测试 8/8 通过，非作者重复通过；干跑以确定性拦截 SQLite、网络、子进程、
环境查找和写入证明零外部调用/零发送授权，不只依赖模板文字。真实准备卡无可发送默认端点或 key，
强制外部用户中转、精确模型/协议/计费/凭据引用/预算/调用授权，缺项拒绝，字段齐全也不产生发送权。

十槽 CLI/Store 正向与负例属于一个集成套件，相关实际运行和首次 CI 失败原日志分别保存，
完成前不计为通过。原 v1/v2/v3、签名账单与历史兼容仍由原测试与完整矩阵核验。

首次 PR CI 41183590624 的五个 CTest job 均 90/91，Make-ASan 同因退出 2：
CLI 测试的 `cls.run` 路径字段覆盖 `unittest.TestCase.run`，导致真实用例分派时
`TypeError: 'PosixPath' object is not callable`。这是测试夹具错误，不能称为 Store 生产漏洞。
原始 67 份 job 日志与 GCC artifact 保存在 `evidence/a2-ci-41183590624-20261031/`。
新增真实 unittest 生命周期自检，仅替换昂贵 workload 并禁止进程/SQLite，旧版 8/9、exit 1；
机械改名 `run_directory` 后 9/9、exit 0。十个业务 case 的断言和期限不变；
生命周期自检不冒充十槽业务验收。修正的非作者源码复核见 `evidence/a2-fixture-fix-root-review.json`。

## A3/A4：动态场景与敏感性对照

`async_coordinator_tests` 是 **一个** CTest 注册项，180 秒界限。
A3 有 12 个 case，A4 增加后为 18 个；原确定性 5 秒等待及既有期限没有放宽。
包含实际 S1 plan/order/ack/note、两个模型等待同时在途、第三请求推进、旧 Actor、AMEND、取消、
队列满、唯一派发、结算/反馈写失败、关闭/重启、同资源顺序及 UNKNOWN；
A4 包含两池普通满载后的审核、有限调度轮数公平、原冻结限制校验、实际回调拒绝同池子调用、
真实 ask 释放槽位同时保留未知预算并允许兄弟请求推进。

同源公平对照保存在 `evidence/a3-a4/fairness_counterexample.cpp`，
分别使用原 A3 `a2dfb46322d217f32eb5cc2516de6107c10db4b8` 和 A4 的对应头/库编译，避免 ABI 混用。
旧 FIFO 是新功能缺口，不称为旧安全漏洞；编译缺接口本身也不冒充业务反例。
实际红绿与最终集成矩阵必须有运行记录后才能转为通过。

非作者源码复核和范围限制见 `evidence/a3-independent-review.json`、
`a4-independent-review.json` 与补充覆盖记录；两项追加 case 的作者和 root 交叉复核另行登记。
这些是同模型工程复核，不替代主协调 Agent 的正式独立审核。

## A5：原生 Windows 的真实范围

最终模块提交 `21f5308271505706594f1a03941ec6ab4a804d50`，树
`1ac91dbc312d89cd631d3750f398c3e86d361887`。原生
[Push 41183805412](https://github.com/wmqfl861/wls861/actions/runs/41183805412)与
[PR 41183808777](https://github.com/wmqfl861/wls861/actions/runs/41183808777)
各 4/4 CTest、十个记录命令全部 exit 0。
实际 PR checkout 为 `27f64d933f9aac27a4f92770d676efe8608bcee2`，其树与 head 相同。
源码快照 [41183805396](https://github.com/wmqfl861/wls861/actions/runs/41183805396)
738 文件逐项字节/模式匹配，产物 13036530746；archive SHA-256 为
`71905ec2ab0a7c5d724cdf606f434bc8e1f754991dc36b35644acebde3eaed62`。

实际环境为 **Windows Server 2022 build 20348 / AMD64 / NTFS / elevated token**，
VS 17.14.37824.6，MSVC 19.44.35229.0，toolset 14.44.35207，SDK 10.0.26100.0，CMake 3.31.6。
原配置与 Scheduler 直接编译；35 项公共资源 160549 字节；测试包括 Unicode 重定位、
源目录移开后运行、缺失/篡改资源拒绝、明确不支持的业务入口拒绝。
Windows 使用类型安全 HANDLE RAII，不伪装 POSIX fd；没有用 Job Objects 冒称沙箱。

最初 CMake toolset 身份配置失败保存于 `a5-native-01/` 和 supplement；
最小修正固定实际编译器路径与 SDK/VS 身份后通过，原失败未删除。
四轮被新 head 取代的旧 Linux CI 经记录取消，结论均为 cancelled，不能计为通过。
最终模块 Linux 六路检查另行核验，不由四项 native CTest 代填。
完整收据 `evidence/a5-native-03-publication/post-publication-receipt.json` 的 SHA-256：
`5f53798d9570812de68a9bcacbd7e0cc8ad6b69177776ea0706e977765e4f299`。

**Windows 11 普通用户 NTFS 为 NOT_RUN**；完整 Store/VFS、业务 Workspace、进程监督、
AppContainer/受限令牌/网络和文件隔离、退出证据及 owned cleanup 均仍 unsupported。
当前 native 组件不提供真实模型调用，不证明完整 wls.exe 已移植。

## 完整矩阵与交付状态

原始数据盘证据保留 argv、起止时间、退出码、stdout/stderr、JUnit、源码/二进制身份与实际注册集合。
正式 Release 两种 umask、严格 Clang、相关 UBSan、共享 GCC ASan/LSan 和每个精确 SHA 的六路 CI
分别登记。工具自检、单项诊断、旧二进制、当前候选与 Windows native 四项不得混计。
失败后没有结束记录的后台进程不会视为完成。测试输入变化时建立新编号，不覆盖原轮次。

| 状态 | 本报告所记录的当前边界 |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | A1 工具和 A5 限定切片已有实现；A2/A3/A4 候选代码存在，动态问题与矩阵完成前不宣称正式验收 |
| `LOCAL_VALIDATION_COMPLETE` | **FALSE**；本机原 GCC 门禁有失败，历史 Clang 未闭环；新增集成执行状态分别记录 |
| `PUSH_COMPLETE` | A1–A5 模块 Draft 分支已推送；集成候选的最终 head 以发布后收据为准 |
| `REMOTE_CI_COMPLETE` | A1 对应 SHA 六路成功；A5 native 对应 SHA 成功；其他配置按实际最终运行，不合并成“全部通过” |
| `PR_REVIEW_COMPLETE` | **FALSE / PENDING**；工程复核不替代主协调者决定 |
| `MERGED` | **FALSE**；所有模块仍待审核，main 未由本轮修改 |

后续最小动作是完成新候选实际矩阵、定位已保留失败并交协调者逐模块裁决。
历史运行时原因未闭环时，A3/A4 的正式集成采纳保持阻塞；A2/A5 可按独立范围审核。
B0–B6 的目标、依赖、模块、反例、矩阵、风险与供应商/费用/批准门禁见
[本轮与后继任务卡](../spec/v0.5.0/10-authorized-cycle-and-followups.md)。

最终发布前核对 protected SQL、v0.4.1、third_party、handoff 和历史 reports 字节；
原日志不重写，凭据/数据库/构建二进制不入 Git。完整源码清单与提交树核验属于具体提交收据。
