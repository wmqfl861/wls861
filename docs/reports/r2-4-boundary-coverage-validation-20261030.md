# R2.4 边界覆盖验证记录（2026-10-30）

本批为已实现的 R2.4 补充容量、恢复谓词与并发边界测试。新增 **3 套逻辑测试、15 个
子例、8 个 CTest 进程分组**；既有 81 个 CTest 注册及期限保持不变，总数为 89。
固定的 `079b527` 基线通过最终新增场景，feature 新测试为 8/8 分组通过。历史 `a6b8559`
的六个谓词场景均先完成必要恢复，再在空闲重复打开的全库快照比较失败，复现已有的
无条件恢复问题。八项主动删守卫的隔离对照均在预期断言失败，它们是测试敏感性证据，
不能登记为 pristine 生产缺陷。用户已在本轮明确接受“基线通过＋移除守卫对照失败”
作为既有正确守卫的覆盖证据方式；这不代替确切提交的正式审核。

**本地结果：完整 Release 两种 umask 与 Clang 18 各 89/89 通过，相关 UBSan 39/39
通过；默认 ASan/LSan 完整相关轮次为 29/39 通过、10 项既有套件超时，退出码 8。**
本批新增八组在这些配置中均通过，但 `LOCAL_VALIDATION_COMPLETE` 保持未完成。
报告冻结于提交前；确切提交、正常推送、远端 CI 和源码快照另由发布回执及 PR 关联。
原始失败和中断记录完整保留，远端绿色结果也不能改写本机 ASan 失败事实。

## 1. 基线、范围与实际输入

| 项目 | 身份或范围 |
| --- | --- |
| 仓库 | `https://github.com/wmqfl861/wls861` |
| 历史 main / R2.4 原始反例基线 | `a6b8559018a599d7a806b05942857a56d69a00ce` |
| 已实现 R2.4 的生产代码提交 | `55dc989e02f7e3e2cd1806e53d47298dd8742376` |
| 本批固定开发基线 | `079b52766ee0929472165a71a8563ce31671bb46` |
| 本批分支 | `test/r2-4-recovery-boundary-coverage` |
| 数据盘任务目录 | `/mnt/d/codex-workspace/wls861-r2-4-coverage-20261030` |
| 独立工作目录 | 上述目录中的 `baseline` 与 `feature`；历史 a6 工作区另在 `/mnt/d/codex-workspace/wls861-r2-4-20261029/baseline` |
| 最终提交 / 远端分支 SHA | 本文冻结于提交前；实际身份、Actions 和源码快照由同一提交的 PR 及数据盘 `publication/` 发布回执记录，避免自引用提交 |

本批没有更改 `src/`、`include/`、`db/`、`third_party/`、`docs/handoff/` 或
`docs/spec/v0.4.1/`。生产语义沿用上述 R2.4 提交；不合并 PR #1、#2，不扩展到其他蓝图
任务。修改范围为 CMake/Make 测试注册、下列四份新增测试文件、STATUS 接续说明及本批
新审核/验证记录和证据归档。
没有修改既有 SQL、历史迁移、旧报告或原测试期限。

| 新增源码 | 作用 | 冻结 SHA-256 |
| --- | --- | --- |
| [r24_settlement_capacity_tests.cpp](../../tests/r24_settlement_capacity_tests.cpp) | 容量边界与真实结算登记分配故障 | `f583ad0bf93cec2d3401e445f1f3187da70b8b94b179f4e7f3cc6b39b6c3f58c` |
| [r24_settlement_allocation_support.hpp](../../tests/r24_settlement_allocation_support.hpp) | 线程局部分配失败注入及完整 new/delete 配对 | `9253f8fc962c37d1576c790601efcb1ac0ce567f5e67c55292324ff88473b507` |
| [r24_recovery_predicates_tests.cpp](../../tests/r24_recovery_predicates_tests.cpp) | 单一恢复触发条件、历史字节与终态隔离 | `b66c4d7b13bfb5fa6f03943b0c2909ec2dfd8b42d8c909d8a84aa93f58010ad6` |
| [r24_concurrency_tests.cpp](../../tests/r24_concurrency_tests.cpp) | 真实提交窗口、宿主退出、迟到调用与清理授权 | `a6e8d29629f8a10622e19c564d89924ac7f6ece948f2851baa88fc5cf8aa62a7` |

`evidence/final-runtime-inputs.json` 于 `2026-10-30T04:58:31.052223+00:00` 冻结
316 项实际运行输入，包括源代码、构建文件、测试、资源和依赖。JSON 文件 SHA-256 为
`875eed9b8c9972d30a2be848a8537852e82058148f6aa604d723c61c83fa4cc1`，配套
`evidence/final-runtime-inputs.sha256` 文件自身 SHA-256 为
`12c03dd56861c0c08a0bf6176ca9f9088587fdda98ec95492f83ba228c3a5f2d`。
本报告和发布回执不属于运行输入，不能将文档补齐误称运行输入已改变。

`evidence/test-registration-and-timeouts.json` 确认 old_count=81、new_count=89、
`all_old_deadlines_unchanged=true`；文件 SHA-256 为
`033727b59a8bd9fefddd5fae34e693127a35f6736c7268eed087f83885ae9de0`。
八个新增进程组均为 90 秒；并发夹具的线程屏障仍为 10 秒、pipe poll 仍为 10000 毫秒。

## 2. 环境与证据记录方法

主执行 Agent 统一运行构建与测试，并依据原始记录完成本文；报告初稿由文档 Agent
核对已有日志、结构化记录及审核材料。测试记录中的 UID/GID 均为 `1000/1000`
（ubuntu），没有使用 root 验证普通用户权限。接手时 `/mnt/d` 为 `/dev/vdb2` 的 ext4
数据盘，79G 容量、55G 可用；
该数值是 `evidence/takeover.json` 的接手快照，不是最终磁盘剩余量。

Release 使用 GNU C++ 13.3.0、Ninja；Clang 严格构建及 sanitizer 实际使用
`clang++-18`。测试 Python 为既有离线测试环境
`/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python`。
常规临时目录位于本批数据盘任务目录的 `tmp/`；sanitizer 后续独立运行使用同一任务
目录的 `tmp-ext4-probe/` 或私有挂载的 `tmp-ram/`，具体介质和权限见第 7、8 节。
普通构建并行度为 2，测试并行度为 1；
单守卫控制构建使用并行度 1。仅使用 FakeModel、独立临时库、私有工作区及既有 loopback
TLS 合成夹具，真实供应商和付费调用均为 0。

每条运行的 `evidence/<名称>.json` 保存实际 argv、cwd、UID/GID、环境选项、umask、
开始/结束时间、退出码、耗时、运行前后输入和原始日志 SHA-256，原始输出保存于同名
`.log`。下文耗时取结构化记录，CTest 内部报告的秒数可能略小。最终门禁要求
`runtime_inputs_unchanged=true`。初始 `feature-build-release-v1` 预编译与测试源码整理
交错，记录为 exit 0、551.438904652 秒、输入前后不同；该次不计为冻结输入后的最终门禁。
后续构建和测试单独保留记录。尚无退出码和结束时间的记录仍为运行中。

## 3. 新增测试及其可观察边界

| 逻辑套件 / CTest 分组 | 子例数 | 具体检查 |
| --- | ---: | --- |
| capacity / `r24_settlement_capacity` | 1 | 4095 条明确标注的合成占用，加两次真实 `fs_write` 后结算失败，检查第 4096/4097 条边界及全局保护 |
| capacity / `r24_settlement_allocation_failure` | 1 | 在真实结果事务 ROLLBACK 后使结算集合节点分配失败；校准节点尺寸、唯一故障次数、集合及全局标记 |
| predicates / `r24_recovery_predicates_active` | 3 | 单独的 CANCEL_REQUESTED、孤立 CLAIMED inbox、FAILED experience；必要恢复、旧原始记录与预算、后续空闲重开 |
| predicates / `r24_recovery_predicates_terminal` | 3 | DELIVERED 的 FAILED experience、DELIVERED 的 FAILED cleanup、CANCELLED 的 FAILED retained cleanup；不增业务权限、清理 lease/CAS 及保留证据 |
| concurrency / `r24_concurrency_phases` | 3 | PREPARED 暂停、RUNNING 提交后宿主 SIGKILL/wait、隔离的历史 CANCEL_REQUESTED；在线写者排斥重开及退出后单次恢复 |
| concurrency / `r24_concurrency_replay` | 2 | 迟到 prepare 遇显式 epoch 切换；迟到精确重放只复用唯一原效果 |
| concurrency / `r24_concurrency_settlement` | 1 | 文件效果已发生、结果 INSERT 未提交的窗口；无第二执行、唯一结果与额度/预算/原事件 |
| concurrency / `r24_concurrency_cleanup` | 1 | 活动清理 claim 排斥在线恢复；退出接管后分别拒绝旧 claim 和旧 Root 会话，合法新身份才能清理 |

容量两例使用生产 `worker_tools.cpp` 的测试翻译单元私有包含，没有增加生产 hook。
4095 条预填条目是进程内集合夹具，不能报成 4095 次真实业务故障。最后两条经过真实
文件写入、Store 结果事务故障和生产 catch；全局保护在 Actor、WorkerTools、Store
重建后仍拒绝新业务效果，并单独验证直接 Store 的 UNKNOWN 守卫及合法精确重放只读。

谓词夹具使用 `only_trigger` 排除其他恢复触发条件；兄弟请求保有真实 RUNNING 操作
和活动 claim，检验作用域未扩大。当前公开 API 不生成的四类历史状态，仅在真实 Store
创建前置状态后对临时库必要状态/CAS 字段作明确注入，保留 schema/trigger；不注入
成功接受、进程退出、结算成功、资源所有权或成功清理。每例比较带 SQLite 类型的原始
记录、预算、请求 epoch、事件、原操作/参数及空闲后的全库快照。

并发夹具在生产事务真实阶段设屏障，在线写者重开失败后仍检查库与文件；正常 join
或真实宿主 wait 后才接管。RUNNING 的宿主死亡保留 UNKNOWN，不把恢复或宿主退出
当作业务进程退出证明。清理授权采用同一当前有效 claim，先证明新 Root 可用，再单独
证明旧 Root 的 writer session 被拒绝，避免两个过期条件相互掩盖。

## 4. 固定基线与 feature 的真实运行

以下相对命令的 cwd 均记录在对应 JSON。baseline 是固定的 079 基线；新增测试通过
独立 probe 构建使用该基线生产实现，未改写基线源文件。

| 证据记录 | 实际命令 | 结果 / 退出码 / 秒 |
| --- | --- | --- |
| `baseline-probes-v2-all`，cwd=`baseline` | `ctest --test-dir ../build-baseline-probes-v2 -V --parallel 1` | 整体 **6/8 分组，exit 8，14.658949313**；容量 2/2 子例及并发 7/7 子例通过，两个谓词组因初始化夹具错误失败 |
| `baseline-probes-v4-predicates`，cwd=`baseline` | `ctest --test-dir ../build-baseline-probes-v4 -V --parallel 1 -R '^r24_recovery_predicates_'` | 最终谓词 **2/2 分组、6/6 子例，exit 0，3.956955153** |
| `feature-new-release`，cwd=`feature` | `ctest --test-dir ../build-release -V --parallel 1 -R '^r24_(settlement_|recovery_predicates_|concurrency_)'` | **8/8 分组、15/15 子例，exit 0，18.656331077** |

三条命令的 umask 均为 0002。079 基线的最终结论来自 v2 已通过的六组和 v4 修正夹具
后的两组，不能把 v2 整条失败命令写成通过，也不能声称这些既有守卫先在 pristine 079
上失败。feature 的八组来自一次完整的新测试选择运行。

Release 的生产静态库还与固定 079 基线作字节比较：

| 库 | baseline 与 feature 共同 SHA-256 | 结论 |
| --- | --- | --- |
| `libwls_s1.a` | `81965f89acee235b7267fb54e776bf4565d2efa50495dfee4bded044d5a60f68` | 字节相同 |
| `libwls_kernel.a` | `3732f83f7402a947123203ab7599f7a25805c60a178d3034fd6a029253d1b2cd` | 字节相同 |

记录为 `evidence/production-release-library-identity.json`，SHA-256
`7f0008f9692416387665f0f9f9e2990b7292202b1edc7adb3e13a0962415f2cb`。
该检查证明本次 Release 两个生产库的身份，不代替所有最终测试二进制或提交树核验。

## 5. a6 历史真反例：必要恢复后仍重复写入

`controls/a6-api-and-library-provenance.json` 先只读核对 a6 clean 工作树、原四份构建/
编译/链接记录及日志、308 项原输入和五项历史二进制身份。本次随后使用 **a6 headers**
重新编译新的谓词测试，并链接哈希匹配的 a6 原 Release 库。不能用新增
`StoreActor.writer_session` 的新 headers 混合旧 archive，以免 ABI 差异造成伪反例。
只读 provenance 文档本身不声称执行成功，实际构建与运行以下列后续记录为准。

cwd 为 `/mnt/d/codex-workspace/wls861-r2-4-20261029/baseline`：

```text
cmake -S /mnt/d/codex-workspace/wls861-r2-4-coverage-20261030/legacy-a6-probe -B /mnt/d/codex-workspace/wls861-r2-4-coverage-20261030/build-legacy-a6-probe -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /mnt/d/codex-workspace/wls861-r2-4-coverage-20261030/build-legacy-a6-probe --parallel 1
ctest --test-dir /mnt/d/codex-workspace/wls861-r2-4-coverage-20261030/build-legacy-a6-probe -V --parallel 1
```

`legacy-a6-probe-configure` 为 exit 0、0.396772546 秒；`legacy-a6-probe-build` 为
exit 0、14.073904012 秒；`legacy-a6-probe-tests` 为 **exit 8、3.834879915 秒**。
两个 CTest 组中的六个子例均先输出：

```text
CHECKPOINT isolated recovery passed; checking idle reopen
```

然后均失败于 probe 源码第 386 行：

```text
Database(fixture.path).snapshot() == stable
```

这证明六种业务前置状态及必要恢复已经建立，失败发生在随后空闲重开的持久快照。
它复现 a6 的无条件恢复及额外写入，**没有证明 a6 缺少这四类必要恢复谓词**。
同类业务条件在 079 最终 v4 和 feature 六个子例中均通过。
`evidence/legacy-a6-probe-inputs.json` 冻结 probe C++/CMake，文件 SHA-256 为
`ca9d7e71120d3c2597d7b5c4fb62b331978775230c6828cce636b5a011274124`。

归档复核发现首轮新增 a6 probe 没有在执行前后捕获其可执行文件身份，因此保留首轮
记录，并用 `python3 run-a6-identity-proof.py` 增补同命令的确定性重跑。
`legacy-a6-probe-tests-identity-v2` 为 **exit 8、13.480220676 秒**；仍为六次必要恢复
CHECKPOINT 后的六次空闲快照失败。包装器本身成功退出 0，表示上述预期红例及身份断言
通过，不能将其写成旧版测试通过。`legacy-a6-identity-proof-v2.json` 记录运行前后相同的
probe 源码、CMake/Ninja 配方、两个旧 archive 及探针二进制身份；探针 SHA-256 为
`a950707fd0c212a91ba5a5213b2dfb3baa59aec4b399653d905d244c8b86ba3d`（4,581,664 bytes）。
此次只补证据链，不把相同六个场景重复计算为新增套件。

## 6. 单守卫敏感性对照

所有变体只在任务目录的 `controls/generated-v1/` 或 `controls/concurrency-v1/` 独立
副本内修改，feature 与 baseline 生产文件不变。生成器固定 079 commit/tree、校验 Git
blob 和 SHA-256、要求目标替换恰好一次，并保存原/变体清单和精确差异。
`optimized-driver-v2` 校验原库、原构建记录、编译器和参数；Store 变体额外编译变异
`store.cpp`，WorkerTools 变体私有包含变异源码。各 `control-v2-*-link-identity.json`
及链接 map 记录实际对象、二进制 SHA 和 `archive_member_exclusion_verified=true`，
避免实际仍链接 pristine 对象造成失真。

全部九个配置及构建退出码均为 0。pristine 正向控制通过 4 个 CTest 组（容量两组、
谓词两组），exit 0；以下八个变体测试退出码均为 **8**，且命中对应断言：

| 变体 | 主动移除的保护 | 实际失败 |
| --- | --- | --- |
| `allocation-catch-no-global` | 分配 catch 设置全局故障标记 | capacity 第 512 行：空集合且全局容量标记必须为 true |
| `capacity-limit-no-global` | 容量满时设置全局故障标记 | capacity 第 424 行：`active.settlement_capacity_exhausted` |
| `predicate-no-cancel-requested` | CANCEL_REQUESTED 恢复谓词 | predicates 第 361 行：dispatcher epoch 必须增加 1 |
| `predicate-no-claimed-inbox` | 孤立 CLAIMED inbox 恢复谓词 | predicates 第 361 行：dispatcher epoch 必须增加 1 |
| `predicate-no-failed-cleanup` | FAILED cleanup 恢复谓词 | predicates 第 496 行：DELIVERED/CANCELLED 两例均未重置为 PENDING/无 claimed_by |
| `predicate-no-failed-experience` | FAILED experience 恢复谓词 | 活动请求第 361 行 epoch；终态请求第 372 行恢复事件数量 |
| `late-prepare-replay-omitted` | prepare 精确重放的提前返回 | 迟到精确重放错误地报 `command_id reused with different intent` |
| `session-guard-omitted` | Store 的 session require | 清理测试未产生期望的 Store 拒绝，报 `expected Store rejection` |

每个变体实际 argv、CTest 选择和退出码见
`evidence/control-v2-<变体>-{configure,build,tests}.json/.log`；聚合结果为
`evidence/control-v2-results.json`，SHA-256
`38929158898643a74b7e9cc8df6a2f10630ee386116d5d44846091a6dedc20bb`。
聚合文件的 `expected_exit` 不是实际退出码，本文退出结论已核对各 `*-tests.json`。
这些结果证明断言能识别主动删除的守卫，不扩大为任意 mutation、任意竞争或历史旧版
缺陷的证明，也不把每个变体算作一套新增产品测试。

## 7. 保留的失败、夹具修正与执行路径修正

| 原始记录 | 真实结果 | 原因与处置 |
| --- | --- | --- |
| `baseline-probes-v1-build` | exit 1，18.879937695 秒 | predicates 第 238 行 `allocate_owned_workspace` 未声明；补声明所在头文件 |
| `baseline-probes-v1-build-capacity-concurrency` | exit 1，20.158501561 秒 | concurrency 第 364 行忽略 `write` 返回值，严格告警失败；显式核对写入长度并保留告警 |
| `baseline-probes-v2-all` | exit 8，14.658949313 秒 | 两个谓词组的 JSON initializer-list 将约束嵌套为数组；改为赋值初始化 |
| `baseline-probes-v3-predicates` | exit 8，3.936715006 秒 | 活动组通过，终态组取消清理例报 `team has no bound workspace`；补真实 `tools.bind(...)` 前置步骤 |
| `control-pristine-configure` | exit 1，1.500220054 秒 | 原 driver 的 `load_cache` 忽略空 CMAKE_CXX_FLAGS，未加引号的比较误判；v2 明确读取/比较空值，未降低身份校验 |
| `final-release-umask0022` | **87/89，exit 8，843.311742881 秒** | 外部二进制输出目录不满足既有 CLI 测试的 executable-relative 资源布局假设，见下文 |
| `prebuild-asan-libraries` | exit 2，1240.769886859 秒 | 可选预编译与 Release 测试争用计算资源，执行器主动以 SIGINT 结束该 Ninja；不是 sanitizer 测试失败或已完成构建 |
| `prebuild-ubsan-libraries` | exit 2，559.108645724 秒 | 同次停止可选预编译；原始中断日志保留，最终门禁须重新完成构建 |
| `final-v2-asan-tests` | **CTest exit -2，2051.707071470 秒**；32/39 项结束，其中 24 通过、8 Timeout；剩余 7 项未完成 | ext4 条件下八项既有套件达原期限；八个新增分组均通过。执行器在工作流项中发 SIGINT 停止已失败轮次，记录器返回 254、顺序驱动退出 1；不计为完整门禁 |
| `asan-storage-ext4-probe-v1` | **0/1，exit 8，90.639767563 秒** | 同一 ASan 二进制的独立 event_runtime 对照，原 90 秒期限；ext4 下再次 Timeout |
| `asan-storage-ram-probe-v1` | **0/1，exit 8，90.519495382 秒** | 同一二进制、原期限和权限；RAM 条件仍 Timeout，不能据此将 ext4 认定为唯一原因 |
| `asan-depth10-ram-probe-v1` | **0/1，exit 8，90.422687074 秒** | 单独评估 10 帧分配栈诊断配置，检测仍开启；event_runtime 仍在原期限 Timeout，没有作为通过的最终门禁替代 |

以上早期编译、初始化、缺失绑定和 driver 错误属于测试/构建夹具问题，不能记为产品
红例。原日志保持原文件名，不用成功重跑覆盖。driver 的独立诊断另存
`controls/cache-value-diagnostic.json`。

两个可选 sanitizer 预编译中断后，`prebuild-after-interruption-processes.json` 核验本任务
无遗留编译进程。Release 测试未被中断，所有原测试期限不变；后续最终门禁顺序构建并运行。
已完成的 Clang 库预编译为 exit 0、674.756634696 秒，仍不单独算完整 Clang 门禁。

ASan ext4 轮的八项 Timeout 为 `event_runtime`、`workflow_runtime`、`acceptance_runtime`、
`worker_tools`、`cleanup_runtime`、`r24_recovery`、`single_agent`、`single_agent_runtime`。
`asan-ext4-stop-v2.json` 保存信号前进程身份和日志快照；`asan-ext4-stopped-process-check.json`
确认被跟踪的 CTest/子进程均退出、没有该 ASan 输出目录的可执行程序残留。停止动作不表示
业务进程退出证据，也不将正在运行的 `live_workflow` 或未开始项目算作通过。
原 `LastTest.log` 另存 `final-v2-asan-stopped-detail.log`，后续重跑不覆盖这份记录。

独立只读比较见 `asan-environment-comparison-review.json`：旧通过轮使用私有 `/tmp` tmpfs，
本轮初始使用数据盘 ext4；既有生产/测试输入与实际 Clang/ASan flags 相同。CPU steal 和
I/O pressure 仅为时点观察。两次新探针分别保留 `time -v`、挂载、UID/GID、附加组、CPU
affinity、四项 capability 及二进制前后身份；ext4 为 user 71.12/sys 11.59 秒，RAM 为
user 78.99/sys 11.38 秒，二者都被原期限截断，不能比较完整任务耗时。
RAM 挂载只在私有 namespace 中覆盖任务目录下的空 `tmp-ram`，不改变全局 `/tmp`；真正测试
以 UID/GID 1000 执行，`CapInh/Prm/Eff/Amb` 为零，`CapBnd` 非零并如实记录。
源码、构建和日志仍在数据盘，RAM 临时库不作为 ext4 或断电持久性验证。
追加只读二进制比较见 `asan-binary-comparison-review.json`：旧通过轮与当前生产 archive
的 31 个成员、2,590 个可执行段逐项字节相同；测试对象的可执行段也相同。链接后的差异
位于源码路径引起的只读数据地址重定位，没有发现实际优化、指令结构或链接对象集合差异。
该核查不能证明超时根因，也没有把旧通过结果改称为陈旧二进制造成的假通过。

首次完整 Release 的 `doctor_diagnostics_cli` 报
`no compiled-compatible executable-relative runtime bundle`；`review_cli_smoke`
三个错误文本子例先遇到 `unsupported`，未进入期望的 `invalid_input` 路径。
本批外置 build 目录使二进制与源码的相对关系不同于原测试要求。主执行 Agent 仅将
CMake runtime 输出配置到数据盘、已忽略的 `feature/build-exec-release`，没有修改
产品资源发现逻辑、原测试、断言或超时。

修正配置使用：

```text
cmake -S . -B ../build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/mnt/d/codex-workspace/wls861-r2-4-coverage-20261030/feature/build-exec-release -DPython3_EXECUTABLE=/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python
cmake --build ../build-release --parallel 2
ctest --test-dir ../build-release --output-on-failure --parallel 1 -R '^(doctor_diagnostics_cli|review_cli_smoke)$'
```

对应 `release-layout-corrected-{configure,build,cli-tests}` 均 exit 0；两项 CLI 重跑
**2/2，通过，9.117927431 秒**。这只能关闭定位检查，完整门禁仍须重新完成。

## 8. 最终本地门禁及未关闭项

修正输出布局后的 `final-v2-release-build` 已结束，exit 0、0.053790590 秒。下列门禁
只在同名记录同时具备退出码、结束时间、原始日志及输入一致性时才可填写通过。

| 门禁 | 要求的选择 / 环境 | 当前记录状态 |
| --- | --- | --- |
| Release，umask 0022 | 完整 89 个 CTest，`ctest --test-dir ../build-release --output-on-failure --parallel 1` | **89/89，exit 0，1096.576007352 秒**；`final-v2-release-umask0022`，316 项输入一致 |
| Release，umask 0002 | 同一完整 89 个 CTest | **89/89，exit 0，2202.107831704 秒**；`final-v2-release-umask0002`，316 项输入一致 |
| Clang 严格 Release | clang++-18，完整 89 个 CTest | **89/89，exit 0，2142.224793127 秒**；`final-v2-clang-tests`，316 项输入一致；配置/构建 exit 0，构建 1290.495635002 秒 |
| ASan/LSan | clang++-18，Debug `-O2 -g`；`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`；39 个相关分组 | **29/39，10 Timeout，exit 8，2949.687652545 秒**；`final-v3-asan-ram-tests`，私有 RAM 临时库，316 项输入及 57 项二进制/库身份前后一致；新增八组全部通过。配置/构建 exit 0，构建 2288.411257002 秒；ext4 轮及探针失败/中止保留 |
| UBSan | clang++-18，Debug `-O2 -g`；`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`；39 个相关分组 | **39/39，exit 0，1353.006271965 秒**；`final-v3-ubsan-ram-tests`，私有 RAM 临时库，316 项输入及 30 项二进制/库身份前后一致。配置/构建 exit 0，构建 2466.815557369 秒，覆盖全部 28 个相关可执行目标及生产库依赖 |
| 最终二进制身份 / 输入复核 | 对最终各配置记录实际二进制，复核 316 项输入及后续提交树 | **本地身份核验完成**；Release / Clang / ASan 各 57 项，UBSan 30 项二进制/库逐项 SHA 与字节数匹配；实际运行路径集合及 316 项哈希相等。提交树及远端快照由发布回执另核 |

sanitizer 的实际选择表达式固定于 `evidence/sanitizer-selection.txt`，包括 Store、
event/workflow/acceptance、WorkerTools、cleanup、process evidence、恢复/结算、
计费、完整 billing baseline E2E、single-agent、相关离线/loopback 集成及八个新增分组。
39 个是既定相关选择，不称作完整 89 项 sanitizer 运行。Release/Clang 的完整运行
包含历史兼容和全部现有 CLI，完成记录已保留；单个重跑不能替代整轮结果。

最终 ASan 轮次于 `2026-10-30T10:40:16.627395+00:00` 结束，10 项超时为
`store`、`event_runtime`、`workflow_runtime`、`acceptance_runtime`、`worker_tools`、
`cleanup_runtime`、`r24_recovery`、`single_agent`、`single_agent_runtime`、`live_workflow`。
`billing_baseline_e2e` 在原 900 秒期限内通过（CTest 592.45 秒），`r3_evaluation` 和
四项已选 CLI 均通过；不能将这些局部通过合并成 ASan 门禁通过。
`live_workflow` 在 300 秒整组期限到达前还输出一个 exhaustion 子例的 `FAIL` 标记，
但未打印详细 traceback；针对该原始子例的独立诊断另列，不遗漏这一观察。
完整原日志、`LastTest.log`、实际命令、time-v 及前后二进制身份均已保存。
`asan-final-process-exit-check.json` 确认该轮已跟踪进程全部退出、无该 ASan 目录的程序
残留，父 namespace 中任务目录仍为数据盘 ext4；这只是测试收尾检查，不代替业务退出证据。

随后 `asan-workflow-exhaustion-diagnostic-v1` 直接加载未改动的 Python unittest，
仅选择 `test_model_exhaustion_escalates_original_rejected_submission`，仍使用同一个
ASan driver、默认 sanitizer 选项、60 秒业务批次、240 秒子进程界限及 300 秒外部
界限。结果 **1/1 通过，exit 0，28.188914685 秒**，输入和所核 driver/脚本身份不变。
它没有复现整组中的 FAIL，因而无法恢复当时被整组超时截断的 traceback；不能解释为
原 FAIL 不存在，不能代替完整矩阵，也未据此修改产品代码。该观察及本机整体超时根因
仍未关闭，需要后续独立审核结合确切提交的远端全套 CI 判断。

另行评估的 ASan 配置在 `halt_on_error=1:detect_leaks=1` 后追加
`malloc_context_size=10`。`asan-stack-depth-review.json` 根据 LLVM 18.1.3 源码确认它
改变分配/释放栈记录深度，不关闭原地址、释放后使用、双重释放或泄漏扫描；但深层
LSan suppression 可能不再匹配，且相同前十帧的泄漏可能归为一组，因此不称为默认
30 帧诊断的等价运行。项目源码没有自定义 sanitizer 默认选项/抑制 hook；独立自检
实际读取的 134 个旗标仅此一项不同，`LSAN_OPTIONS` 未设置。
`asan-depth10-detector-verification.json` 记录独立程序的越界、释放后使用、双重释放、
64-byte 泄漏四个故意错误均产生相应诊断并 exit 1；编译及两份旗标读取 exit 0。
它们只是检测器配置自检，不增加本项目的逻辑套件或子例数量，也不代替完整产品测试。
默认深度的原超时记录继续保留。10 帧条件的产品探针仍失败，故没有采用它替换最终门禁；
完整相关重验回到原 `halt_on_error=1:detect_leaks=1`，不添加该选项。

## 9. 独立复核与适用范围

[本批独立交叉审核](../reviews/r2-4-boundary-coverage-review-20261030.md) 保存作者、
非作者审阅者、输入 SHA 和 finding。容量作者 `/root/concurrency_fix` 与并发作者
`/root/independent_recovery_review` 互相全文审核；谓词作者 `/root/evidence_audit`
的文件由 `/root` 全文审阅。并发 F1 将 `operation_intents.arguments` 原始字节纳入
快照；F2 用当前有效 claim 单独检测旧 Root session。两项新增测试覆盖缺口均已整改并
经非作者读回关闭。容量和谓词审核未发现阻断项；这不是主协调 Agent 的正式采纳或
合并批准。

收尾证据工具另经 `/root/evidence_audit` 只读审阅，记录为归档内
`evidence/finalizer-failure-aware-review.json`（SHA-256
`05123e7d47844e33ca737d38ee36032389e2e7bc874baaa42d4dc5fcf240aee5`）。
完整失败门禁允许归档，但必须保留非零退出及 `local_validation_complete=false`。
最终核验同时检查实际运行文件集合和 316 项冻结哈希；归档拒绝符号链接、非普通文件、
NUL 和非 UTF-8 内容。审核时 85 份已完成命令的原日志哈希匹配，26 份非零退出及其原日志
均在选取范围；这些是审核时点数量，最终归档数量以第 10 节清单为准。

本批结果仍有以下明确边界：

- 容量等保护在旧实现本已有效。本批分别交付历史真反例、固定生产基线正向结果和八项
  单守卫敏感性对照。用户已明确接受该证据方式，不要求为正确守卫伪造历史失败；仍不能
  将人工对照称作 pristine 缺陷，也不据此推导正式 PR 审核完成。
- 容量两例沿用受支持的 Store 403 tools 夹具；完整事件、工作流、清理及当前迁移兼容性
  依赖本批其余测试和既有完整回归，不能把该夹具单独称为所有 Store 模式的证明。
- 故意 SIGKILL/`_exit` 的宿主崩溃子进程不会产生自身正常退出的 LSan 结论。
- 新清理场景是文件工作流，不单独证明所有真实 `run` 进程退出负例；相关既有套件须按
  最终矩阵运行，恢复成功始终不能充当进程退出证据。
- 快照比较建立在受控暂停或 join 后稳定点，不证明任意多写者下的通用一致快照。
- 分配注入依赖当前调用链及运行库，节点校准不匹配必须失败调查；文件检查覆盖原效果
  和拒绝目标，不声称任意工作区全树审计。
- 不覆盖真实供应商、付费接口、生产数据或原生 Windows 环境。

## 10. 归档、发布与交接状态

本报告引用的 `evidence/`、`controls/` 位于数据盘任务目录。已生成下列公开文本证据
归档：401 个成员、6,366,024 bytes，全部为 UTF-8 普通文件，已逐项读回比对 SHA-256
及字节数，保留历史失败和中断，不包含可执行二进制或临时数据库。

| 交付文件 | SHA-256 |
| --- | --- |
| [原始文本证据归档](r2-4-boundary-evidence-20261030.tar.gz) | `ac6a4d33d7f4f0080e5908b6738c97ae4b96be8fbdbaed2d24d170d926ee76f5` |
| [401 项成员清单](r2-4-boundary-evidence-20261030.members.json) | `33ae0bfe3cf924d3c9fdb4d81a360303f3f6919745385d9dfff46d7857a0929e` |
| [316 项运行输入清单](r2-4-boundary-inputs-20261030.sha256) | `12c03dd56861c0c08a0bf6176ca9f9088587fdda98ec95492f83ba228c3a5f2d` |

归档中的 `evidence/final-local-verification.json` SHA-256 为
`03e6c1d0e3c45aabe3664373dde5c422f5cad95254195daa005c8662be027c66`。
它确认记录完整、输入和二进制一致，同时明确 `local_validation_complete=false`。
`final-evidence-verification` 命令因此返回 **1**（19.164709492 秒），表示存在未通过
门禁；不是归档失败，也不将失败结果改记成功。归档命令独立执行，退出码 **0**。
`final-test-process-check.json` 记录诊断结束后无本任务测试程序残留。

`2026-10-30T10:42:24.076607+00:00` 的 fetch/readback 确认 main、R2.4 和蓝图分支仍
分别为本文记录的 a6、55、079，专用测试分支尚不存在于远端；原始命令保存于归档的
`evidence/prepublication-remotes.json`。提交前核对最终差异、允许文件范围和敏感文件；
正常提交/推送后，数据盘 `publication/` 回执与该提交的 PR 记录完整 SHA、远端分支、
六项 CI 矩阵及源码快照核验，避免本文自引用未来提交。
只有确切提交的远端运行结束且成功，才能在发布回执中标记远端完成；不会由本地结果
推断远端结果。PR #1、#2 不由本批合并。

| 状态 | 本报告提交前冻结时的真实值 |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | **COMPLETE（本批新增测试覆盖）**；3 套、15 子例已实现，新测试 8/8 通过，独立审核整改闭环 |
| `LOCAL_VALIDATION_COMPLETE` | **NOT_COMPLETE**；五项门禁中四项通过，本机完整相关 ASan 29/39、exit 8；原因与工作流 FAIL 观察未关闭 |
| `PUSH_COMPLETE` | **PENDING_AT_REPORT_FREEZE**；实际提交及远端 readback 由发布回执记录 |
| `REMOTE_CI_COMPLETE` | **PENDING_AT_REPORT_FREEZE**；由确切提交的 Actions 和源码快照更新发布回执 |
| `PR_REVIEW_COMPLETE` | **PENDING**；协作 Agent 交叉审阅不是主协调 Agent 的正式批准 |
| `MERGED` | **NO**；未合并 PR #1、#2 或本批测试分支 |

本批交主协调 Agent 对确切提交及未关闭的本地门禁独立审核，再决定是否采纳以及后续
定位任务。测试已通过的既有保护、人工删守卫对照和历史真反例须继续保持
上述各自含义，不得在最终发布描述中合并为“所有新测试在真实旧版先失败”。
