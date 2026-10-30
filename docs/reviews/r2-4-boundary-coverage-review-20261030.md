# R2.4 边界覆盖独立交叉审核（2026-10-30）

本报告记录本批新增测试的代码审阅及整改，不代替执行验证或主协调 Agent 的正式验收。
并发测试发现的两项断言缺口 F1/F2 已修正并经非作者复核；容量测试的独立审核未发现
阻断项；恢复谓词测试由本轮主执行 Agent 全文审阅。各项结论的责任人和实际范围如下，
不是由本报告编写者独自确认所有部分。

实际构建、基线/修订版运行、隔离敏感性对照、完整回归、sanitizer、最终输入及发布状态，
统一见[本批验证报告](../reports/r2-4-boundary-coverage-validation-20261030.md)。该报告由
主执行 Agent 根据完成记录填写；本审核不预先声明这些门禁通过。

## 1. 输入、分工与审核身份

开发阅读基线为 `079b52766ee0929472165a71a8563ce31671bb46`，承接 R2.4 已验证提交
`55dc989e02f7e3e2cd1806e53d47298dd8742376` 的生产代码。本批专用分支为
`test/r2-4-recovery-boundary-coverage`，工作目录为数据盘上的
`/mnt/d/codex-workspace/wls861-r2-4-coverage-20261030/feature`。
当前工作是补充测试覆盖；本报告不把基线已存在的保护机制改称新修复。

| 对象 | 实现作者 | 本次审阅者 | 审阅归属与边界 |
| --- | --- | --- | --- |
| 容量边界及分配故障两文件 | `/root/concurrency_fix` | `/root/independent_recovery_review` | 非作者全文只读审核；本报告编写者是作者，因此不把自己的实现检查算作这项独立审核 |
| 并发测试 | `/root/independent_recovery_review` | `/root/concurrency_fix` | 非作者全文审阅，提出 F1/F2；作者整改后按新 SHA 复核闭环 |
| 恢复谓词测试 | `/root/evidence_audit` | `/root` | 主执行 Agent 对另一作者的测试全文交叉审阅；本报告引用其结论，没有冒称报告编写者全文复审 |
| 构建、运行、日志与发布 | 各测试作者配合 | `/root` 统一执行与记录 | 执行证据与代码审核分列；执行者自审和绿色 CI 均不等于主协调 Agent 的正式验收 |

上述均为本次同一协作任务中的 Agent 身份，不声称来自独立供应商或不同模型。
正式设计采纳、PR 审核和合并仍归主协调 Agent 及相应授权方决定。

当前记录的测试输入 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| [r24_settlement_capacity_tests.cpp](../../tests/r24_settlement_capacity_tests.cpp) | `f583ad0bf93cec2d3401e445f1f3187da70b8b94b179f4e7f3cc6b39b6c3f58c` |
| [r24_settlement_allocation_support.hpp](../../tests/r24_settlement_allocation_support.hpp) | `9253f8fc962c37d1576c790601efcb1ac0ce567f5e67c55292324ff88473b507` |
| [r24_concurrency_tests.cpp](../../tests/r24_concurrency_tests.cpp) | `a6e8d29629f8a10622e19c564d89924ac7f6ece948f2851baa88fc5cf8aa62a7` |
| [r24_recovery_predicates_tests.cpp](../../tests/r24_recovery_predicates_tests.cpp) | `b66c4d7b13bfb5fa6f03943b0c2909ec2dfd8b42d8c909d8a84aa93f58010ad6` |

并发测试首次审阅输入为 `7418f917faa27cf64a063dc8a44b7aee0bef1f59a7a03ec312be7e70ebb01dce`
（619 行），整改输入为上表 SHA（637 行）。容量两文件分别为 538 行和 92 行，审核后未修订。
恢复谓词的主执行 Agent 全文审阅结论先对应
`06029b44c2993ed66415a8618bf554ec29dc5b58402b349dbb9a8fb55b3c806f`；作者随后将
`Json constraints{parse_json_strict(...)}` 改为 `Json constraints = parse_json_strict(...)`，
修正 initializer-list 导致的嵌套数组夹具，形成
`bef7f68789bd2b9bb0aa7eddc663e764e7ff0563406ba64aaedd5196864959f9`。随后在取消清理
夹具的 WorkerTools 构造后补上 `tools.bind(fixture.actor(fixture.selected));`，通过真实
Store/workspace 绑定校验建立遗漏的前置条件，形成上表输入；没有改动守卫或断言。
作者已确认这两项增量，后续夹具修订及最终执行身份由主执行 Agent 统一核验，
本报告不据此推断任何运行已通过。

## 2. 并发测试：F1/F2 及闭环

审核人 `/root/concurrency_fix` 全文阅读首次输入，并对照实际 WorkerTools、Store
事务/恢复/派发/结果写入路径及完整 `src/store_cleanup.inc`。没有修改被审文件。

| Finding | 首次输入位置 | 问题及可能漏检 | 整改与复核结论 |
| --- | --- | --- | --- |
| F1：原参数字节未进入恢复比较 | `intent_bytes` 第 155–158 行；`immutable_history` 第 146–151 行 | 只比较 `ops.args_sha256` 等字段，未比较 `operation_intents.arguments`。PREPARED、RUNNING、CANCEL_REQUESTED 恢复若改写原参数而保留旧 hash，原断言可能漏检 | 当前 `intent_bytes` 连接 `operation_intents`，将 `arguments` 的类型、长度和完整字节纳入快照摘要；恢复前后继续比较。**CLOSED** |
| F2：旧 claim 掩盖旧 Root 会话检查 | 清理测试第 577–578 行 | 旧 Root＋旧 claim 和新 Root＋旧 claim 均可能只因 claim 失效而被拒绝，不能单独证明旧 Root 会话失效 | 当前第 584–598 行取得合法当前 claim，先证明新 Root 授权成功，再检查旧 Root 使用同一 claim 返回 Stale；主体和 dispatcher epoch 相同、writer session 不同。拒绝后全库、结果文件和 owner 文件的身份及字节不变。**CLOSED** |

这两项是新增测试的覆盖缺口，不是已经复现的生产漏洞。作者保留原权限、断言与期限，
仅补强可辨别性。整改 SHA 已由审核人读回确认，未发现新的阻断项。

审阅确认下列屏障和检查具有明确的源码位置及持久状态依据：

- PREPARED 在真实 prepare 提交后暂停；私有观察点抛出测试异常，留下真实未派发操作。
  当前写者仍在时重开被排斥；原调用结束后真实接管增加 epoch、取消 PREPARED，旧 prepare
  精确重放被拒绝。该异常夹具不冒充 OS 崩溃。
- RUNNING 通过 SQLite `SQLITE_TRACE_PROFILE` 在 start 的 COMMIT 完成后通知父进程。
  父进程核 RUNNING/started_at、尚无结果与文件效果，再 SIGKILL/wait 原宿主子进程。
  接管保存 UNKNOWN，后续原绑定事实不变成确定成功或业务进程退出证明。
- 结果写入在真实文件效果之后、`INSERT INTO operation_results` 的事务内暂停。
  其他连接仍看到未提交前的 RUNNING/无结果；完成后核唯一结果、两个输出记录、工具额度、
  事件前缀和预算。空闲重开与精确重放不产生新的文件效果或数据库变化。
- 迟到 prepare 在进入 Store 前暂停；同一 Store 显式 epoch 切换后，旧调用没有新操作、
  派发、额度或文件效果。另一个精确重放场景让 owner 完整结束，再释放迟到调用，核唯一效果。
- 清理 claim 在真实 COMMIT 完成后暂停；当前写者排斥在线接管。退出后接管更新清理租约，
  保存原 inventory；旧 claim、旧 Root 会话分别验证，新 Root 才能继续合法清理。最终保留
  owner/交付回执，终态不能再次分配业务工作区。

`PausedCall` 异常析构先 release 再 join，观察器在 join 后清除；SQLite C 回调内的
测试异常保存后在 join 之后重新抛出。宿主崩溃夹具的 `Child` 负责 SIGKILL/waitpid 收尾。
线程屏障仍为 10 秒，pipe poll 为 10000 毫秒；四个 CTest 进程组都保留 90 秒期限。
七个子例仍是一套逻辑并发测试，分组不计作额外独立产品套件。

## 3. 容量及分配故障：另一 Agent 的独立结论

本节依据 `/root/independent_recovery_review` 对两份容量文件全文及相关
`src/store.cpp`、`src/worker_tools.cpp` 的只读审阅回传。该审核人不是容量测试作者，
未编译、运行测试或执行 mutation，未修改被审文件；结论为**未发现阻断 finding，
未要求作者整改**。

审阅覆盖：

- 4095 条记录明确标为合成的进程内占用，不代表 4095 次 Store 或业务执行。末两次登记
  通过真实 `fs_write`、结果事件 trigger 中止和生产 WorkerTools 的结算失败 catch。
  第 4096 条时集合满而全局标记仍 false，健康 Store 仍能执行；第 4097 次时集合保持
  4096 条，全局标记 true，失败的新 tuple 没有被加入。
- 指定 trigger 触发后的 ROLLBACK PROFILE 才武装线程局部的下一次 C++ 分配故障。
  对照 `Store::Transaction` 析构、`record_operation_result` 返回至 WorkerTools catch 的
  源码，期间只有销毁/解锁及布尔和大小检查，没有其他 C++ 分配。审核人另核本机
  libstdc++ 13 `stl_tree.h` 节点分配先于 tuple 构造；同类型短键校准、唯一失败次数、
  分配尺寸匹配、空集合及全局标记共同约束真实节点 `bad_alloc` 的命中，不能以一般
  “函数抛异常”代替容器分配失败。
- 新 Actor、新 WorkerTools、另一数据库及新 Store 都检查全局保护；真实文件观察支持的
  Root 核对也不能解除该标记。新 Store 保持相同业务 epoch 时，旧 Actor 的 wrapper 和
  Store prepare 精确重放仍须拒绝，当前合法重放保持只读与原结果。
- 拒绝路径比较全库快照、拒绝目标缺失、原请求/说明/attempt/预算/事件前缀及原文件
  inode/字节；直接调用 Store 的 UNKNOWN 守卫，避免 wrapper 的 NotReady 掩盖该边界。
- 专用分配支持文件覆盖普通、数组、sized、aligned、nothrow new/delete，实际使用
  malloc/free 或 posix_memalign/free。每例独立进程正常退出，给 LSan 检查机会；静态审阅
  未发现明显的 GCC/Clang/sanitizer 冲突，但这不是对应构建和运行已经通过的证明。

该审核明确保留三项适用范围：使用受支持的 Store 403 tools 夹具，完整 event/cleanup
由其他套件覆盖；节点定位依赖当前调用链和运行库，校准失配必须失败调查；文件检查
覆盖原文件及拒绝目标，不是任意工作区全树审计。

## 4. 恢复谓词：主执行 Agent 的全文交叉审阅

本节引用 `/root` 对 `/root/evidence_audit` 所写恢复谓词测试的全文审阅结论：
**未发现阻断项**。报告编写者只核了输入身份、相关声明及夹具修订，没有把这项
结论登记为自己的全文独立审核。

主执行 Agent 确认的范围包括：`only_trigger` 排除其他恢复谓词，避免由别的在途状态
使目标负例错误通过；带 SQLite 类型的快照保全原始业务记录和预算；兄弟请求具有
真实 RUNNING 操作及活动 claim，能检验请求作用域未意外扩大；终态恢复、清理 lease/CAS
与原证据保留有独立断言。

四类缺少当前公开生产入口的历史状态为 CANCEL_REQUESTED、孤立 CLAIMED inbox、
FAILED experience 和 FAILED cleanup。它们使用真实 Store 建立前置状态，再对临时库
最少状态/CAS 字段作明确注入，保留原 schema 和 trigger；不伪造接受、进程退出、模型
结算、资源所有权或成功清理。此类夹具是兼容边界验证，不冒充当前 API 正常生成了这些状态。

## 5. 已观察到的初始编译夹具错误

以下两项来自主执行 Agent 统一构建的原 `.log/.json`，报告编写者已读取相应错误和
退出记录。它们属于测试代码编译失败，**不计作产品旧版行为反例**。

| 原记录 | 实际错误与退出 | 修正 |
| --- | --- | --- |
| `baseline-probes-v1-build` | predicates v1 第 238 行 `allocate_owned_workspace` 未声明；exit 1，18.879937695 秒 | 增加声明所在的 `wls/cleanup_runtime.hpp` 头文件；不改变生产接口或断言 |
| `baseline-probes-v1-build-capacity-concurrency` | concurrency v1 第 364 行忽略 `write` 返回值，`-Werror=unused-result`；exit 1，20.158501561 秒 | 显式检查写入长度，不为 1 时 `_exit(94)`；保留严格告警，不用强制转换或禁告警绕过 |

原记录保存在任务目录 `evidence/`；其归档身份和后续重建由验证报告关联。源码已修正
不能自行证明后续编译成功。恢复谓词的初始化和 workspace 绑定夹具修订另见第 1 节；其实际运行结果同样
只以验证报告记录为准，不在本审核补写推测结果。

## 6. 审核方法、限制与交接

结构查询使用父代理提供的 codebase-memory Verify 信息：项目
`mnt-d-codex-workspace-wls861-r2-4-20261029-feature`，generation
`2026-10-30T02:07:47Z`，fast 索引。主执行 Agent 的 `takeover.json` 记录相关旧索引路径
与本批基线字节相同；新测试未索引，直接全文阅读。`store_cleanup.inc` 全文属于图谱
不可用范围，已由并发审核人直接补读，未从图谱空结果推断安全性。

并发审核所对照的生产源码 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `src/worker_tools.cpp` | `aedf1cfc9c428ae74b68da7922a57558996733ebe08832f809fa8c15467b689a` |
| `src/store.cpp` | `f196be42cd740f9484bf2f8b39f8ea668dd35ce6779ade31d4c59145216b3d7c` |
| `src/store_cleanup.inc` | `c0255d6582a2a2a0faae5b5a981abb818329f9674e20d256bd143f2b3b9558a5` |

静态审阅没有代替以下证明：

- 真实运行时的触发命中、完整测试结束、strict 编译、ASan/LSan、UBSan 及最终源码/二进制身份。
- 故意 SIGKILL/`_exit` 的崩溃子进程自身 LSan 结果；父进程正常退出不能替代它。
- 任意进程退出/清理负例。当前新增清理场景是文件工作流，没有真实 `run` 进程的退出负例。
- 通用多写者一致性。当前测试验证既有独占写者边界；数据库快照依赖受控暂停或 join 后
  的稳定状态，跨连接读取方式不是通用并发一致快照。
- 缺能力平台或真实供应商行为。所有相关夹具只使用临时数据库、私有文件和 FakeModel，
  不建立真实模型、付费 API、生产部署或 Windows 原生运行的验收结论。

既有 4096/4097 和分配失败保护在原实现中已经存在；新增覆盖、编译夹具修正和隔离
敏感性对照都不得换标成新发现的 pristine 生产缺陷。后续敏感性对照必须说明它主动
移除了哪一道保护，以及对应的实际输入和失败，不与历史原版红例混写。

交接时 F1/F2 已闭环，容量独立审核无待处置 finding；恢复谓词按主执行 Agent 的
审阅归属及最新夹具修订继续统一验证。没有从本报告推导 `LOCAL_VALIDATION_COMPLETE`、
`REMOTE_CI_COMPLETE`、`PR_REVIEW_COMPLETE` 或 `MERGED` 状态，最终结果由验证报告和
主协调 Agent 对确切提交的决定建立。
