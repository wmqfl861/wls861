# R2.4 独立工程复核

日期：2026-10-29。基线：`a6b8559018a599d7a806b05942857a56d69a00ce`。
审查者为独立只读 Agent `independent_recovery_review`；执行者记录其结论。
这是同一模型体系的工程复核，不替代主协调 Agent 的独立审核或业务验收。
以下按实际审查阶段保留结论和输入身份；早期哈希、执行计数及建议不代表最终快照。
最新 v3 结算故障会话隔离的静态复核见文末，完整验证和发布状态另以实际运行记录为准。

## 审查范围与结论

实际补丁复核已完成，在本次限定范围内未发现需阻断后续验证的生产代码缺陷。
审查覆盖 `include/wls/store.hpp`、`src/store.cpp`、`src/worker_tools.cpp`，
相关 Store 授权/结算辅助实现，R24 三个测试套件与构建注册、既有 R2 两处测试调整，
ADR-091、新实现契约及 AGENTS、STATUS、ROADMAP、请求/epoch 恢复文档。
审查者没有编辑文件、执行构建或调用模型服务。

确认的边界：

- OneRequest 在原独占写者锁和同一 `BEGIN IMMEDIATE` 内读取、判断、恢复并提交；
  没有事务外判定、CLI 独有跳过或并行写者。
- `authorize` 无条件核验签发会话，覆盖 `current_epochs=false` 的精确重放前置路径。
  所检查的公开 Actor 方法直接授权或经原辅助函数汇入该边界。
- 判定包括未围栏操作、PENDING attempt、ACTIVE/CLAIMED 领取、需回收的经验/清理 job，
  以及缺失的非交付终态留存事实。普通待办、WAITING、AMEND、已围栏 UNKNOWN、
  无领取的 PENDING job、裸分配意图和保留清单继续使用原守卫。
- WorkerTools 在 actorless 工作区绑定前检查会话，控制路径在当前授权及限流前检查会话；
  同一会话中旧 epoch 的精确控制重放仍按原规则只读返回。
- 会话没有进入 `actor_json`、执行绑定、原命令或结算指纹。默认 AllRequests、显式恢复、
  不可变说明及恢复见证、原结果/反馈、UNKNOWN、清理 CAS、退出与所有权守卫保留。
- 清理测试分别拒绝旧 Root Actor 与旧 claim，核对原 inventory、真实临时文件及实际恢复清理。
  合成进程观察明确标记为夹具，没有冒充真实进程运行或退出证明。

## 建议及处理

审查者建议独立覆盖仅有 PENDING 模型尝试的请求，避免同时存在 ops/claim 掩盖判定遗漏。
执行者已增加 PENDING-only 子例，检查第一次恢复、原预约/信封不变，和 UNKNOWN-only
第二次打开全表稳定；审查者复核了该补充。
另增加外来控制会话超过限流次数仍全被拒绝、当前会话随后可 ack，以及同会话旧 epoch
精确重放不写数据库的子例。

核心套件现在为 17 子例，清理为 6 子例，CLI 为 3 子例，共三个套件。
原 R2 WAITING 测试加入真实 PREPARED 状态以保留必要恢复条件，继续核对取消和等待守卫；
CLI 空闲测试改为严格核对 epoch 与恢复事件稳定，兄弟和原记录断言保留。
没有降低安全断言、放宽权限或扩大原有测试期限。

审查者独立查看了首次稳定基线原始日志：核心 6 通过/9 失败，清理 2 通过/4 失败，
CLI 1 通过/2 失败。执行者随后对增加两个子例后的核心重新运行 pristine 基线，
实际为 6 通过/11 失败；该补充执行结果由执行者负责，不计为审查者执行的测试。

## 审查输入身份与限制

| 文件 | 审查时 SHA-256 |
| --- | --- |
| `include/wls/store.hpp` | `237eeb533e060d04666353fb0197ef64af8bd324944874f75898a0ce809f046d` |
| `src/store.cpp` | `f196be42cd740f9484bf2f8b39f8ea668dd35ce6779ade31d4c59145216b3d7c` |
| `src/worker_tools.cpp` | `d24e13ae0642b36b6d61348b88e6882d0cb33ffceebecf970a774f42de5f96cb` |
| `tests/r24_recovery_tests.cpp` | `f61a705c2bb9dcb613e5a1ef6324d089193746d0ea5ed9fe0a92706ee42ee7d6` |

审查者末次 `git diff --check` 为 exit 0。会话未提供图索引/coverage 工具，因此通过定向源码
与搜索复核，没有图覆盖或全项目安全性证明声明。Release、sanitizer、Clang、最终快照、
推送及 CI 必须以执行者的[本批验证报告](../reports/r2-4-demand-driven-recovery-validation.md)
及原始日志为准，本复核不推断这些门禁通过。

## 完整测试暴露的兼容假设及实际补丁复审

首次完整 Release 实际结束为 70/81、exit 8，含两个文件夹具超时及跨 Store Actor/
空闲必增 epoch 的旧假设。执行者调整后，同一独立审查者再次阅读实际兼容测试 diff，
最终落盘补丁未发现待修复缺陷；这次审查仍未执行构建或测试。

复审覆盖新增 `tests/store_actor_support.hpp`，及 store、workflow_store、acceptance_store、
r1_window、live_model_store、single_agent 原生测试和 s13、r1_structured、r3_evaluation
三个 Python 测试。确认：

- `replay_actor` 从当前 Store 签发并核对 role/principal/request/task/team 和不同 session，
  仅恢复历史 epoch 以重现原命令指纹。先验证旧 Actor 拒绝且持久状态不变，再验证原结果
  只读重放及新键历史 epoch 拒绝；没有把新 session 填入旧 Actor。
- reader 的 owner、policy 和旧 binding 负例使用 reader 自己的有效 Actor，避免被 session
  先行拒绝掩盖原 guard；历史尾读取仍保留 SQLite trace 的确定性并发 append 断言。
- CLI 跨 epoch 历史/见证用例通过显式 init 触发原强制恢复，原字节、反馈、CAS、见证篡改、
  原 v2 解释和隔离断言保留。single_agent 同时检查 idle 和 forced，逐条原历史不变、
  同一个 logical_agent_id、每次恰好追加两次调用，epoch 变化必须与 force 相同。

审查发现并修正了一处新增测试预期：已有 live send claim 时，更换命令键先触发原
唯一 claim 的 `Conflict`，并非 `Stale`。只修正新增断言及其说明；没有改生产顺序。
旧 Actor Stale、无持久副作用、相同 claim 重放不获得新发送权继续检查。

| 实际兼容复审输入 | SHA-256 |
| --- | --- |
| `tests/store_actor_support.hpp` | `de3423530d1db40a0b714593e466567799d8dbea7e7ce6181e2d2a43fb96b2fc` |
| `tests/store_tests.cpp` | `5a55370c49d7833e7e4ecb1827bb0f7b7f286b537a22a2db6ec907905fac9b12` |
| `tests/workflow_store_tests.cpp` | `ad54af2882f974a0a03c2f1c6105c6c074424b2fc548f5cbb5043c888fcc2a63` |
| `tests/acceptance_store_tests.cpp` | `8b9a8703ac53218c863f0282a37e3ed0f8dc5261cdacb1dd52ff1ed842e589ed` |
| `tests/r1_window_tests.cpp` | `5cfcd26230f2a72f92a13f40383ed106abf2b437068304e897485f3a78328801` |
| `tests/live_model_store_tests.cpp` | `5ebe7fae0798d0bbe0cca9ca7c9176950509a08fafbb30d2da7515e20838c2f3` |
| `tests/single_agent_tests.cpp` | `597cd5c41ecff2b86bd57c25477cd20088bb011d1b63e81b2ec9e015996a21c8` |
| `tests/s13_cli_smoke.py` | `009d3efbfd70a1828e90d18eefe5822043ccb2c9edf4bea5728d35779a8230f3` |
| `tests/r1_structured_cli_smoke.py` | `ef526e5a0d022ab42ecc25dc2433a9e8021b8906aa0f521831088c00387b2811` |
| `tests/r3_evaluation_tests.py` | `c6650a4fa672895c4bc8c3ea638d932d6af9cd705bce96dae975910514492d49` |

在该次兼容补丁复审时，三个生产文件仍与前表相同。审查者单独执行 `git diff --check` 为 exit 0；
没有宣称调整后运行、完整图覆盖或远端门禁通过。

最终文档进一步区分了新增 `writer_session` 字段与复用的既有 `writer_instance` 值：
原 live 预约/发送记录中的实例 ID 继续持久化；新增 Actor 字段不参与原事实或指纹，
也不把该 ID 变成网络认证秘密。独立审查者再次只读确认三处澄清与已审实现及可信宿主
契约一致，消除了“实例 ID 从不持久化”的潜在误解，没有发现问题。此次只审文档措辞，
没有重新审核生产补丁、修改文件或执行测试，也不推断任何发布门禁。

## 验证期间的 Clang 并发失败追加复核

完整 Clang 候选 `feature-clang-serial` 中，原 worker_tools 并发用例出现
`command_id reused with different intent`。独立审查者针对原日志再次只读检查，
并逐函数对照基线与 feature，以下片段的两侧字节完全相同：

| 片段 | 字节数 | 两侧相同的 SHA-256 |
| --- | --- | --- |
| `Store::Impl::prepare_operation` | 4082 | `a65924f2fc2d5e92c48236806c3718b15d3fd49f80c57f89112fe653cc139ad0` |
| `Store::start_tool_operation` | 2183 | `ed7eead292cfac3cfd59a29fa20491f0113832dd0e3a64a769524311a681ec0f` |
| `WorkerTools::execute` | 10924 | `98040b1e07d8dedc15878869c211ed6e647af2523d00f7be3dff6a4902d6a28b` |
| `concurrent_exact_commands_have_one_execution_owner` | 1714 | `88e8af9fd3e1df5d07ab25336b96d9fa312d34cade141e3b004f6ac7ba4a205f` |

可达时序是两个线程先查询同一 operation key 为空，T1 prepare 得到版本 1 并派发，
T2 延迟 prepare 的精确重放返回已经 RUNNING 或 SUCCEEDED 的当前行版本 2/3。
若 T1 已移除内存 active owner，T2 可以重新插入 owner，并使用变化后的版本派生
同一个 `command:start`；Store 正确拒绝与原版本 1 不同的命令指纹。
测试在线程开始前冻结了相同的 Actor、binding、task version、key、command 和 args。
该用例只有一个 Store 生命周期，没有重开、恢复、epoch 或 session 切换。

审查结论为基线已有的并发重放竞态，当前证据不显示 R2.4 引入双派发、旧身份授权或
CAS 绕过；不要求扩大本卡生产修改，但不能将失败的 Clang 运行记为通过。保留源码
冻结与所有原断言，单独重跑完整 Clang。该竞态仍交主协调 Agent 决定后续独立任务，
不得以重跑通过声称竞态已修复。审查仅包括源码、差异及既有日志，没有实际执行基线
竞态复现、修改文件或运行测试。

## 独立证据审计

独立只读 Agent `evidence_audit` 核验了原基线与最终反例编译/链接 argv、日志和二进制
身份，确认核心 6/11、清理 2/4、CLI 1/2 的通过/失败数及 exit 1 来自目标断言；
早期夹具错误另行保留，没有被算作缺陷反例。该次审计的 v1 共 312 项运行输入与冻结清单相同，
清单不是完整提交树；323 个原有受保护文件的字节、类型和模式保持基线。

该次审计直接核验 v1 完整 Release `0022` 为 81/81、exit 0、2102.195914 秒，输入和记录的
二进制前后相同；普通 UID/GID、umask、capability 和私有 tmpfs 与描述一致。实际
优化 sanitizer 缓存和编译/链接 flags 保留插桩、Werror、Debug `-O2 -g`，无 NDEBUG。
该配置检查不等于执行结果。三个新逻辑套件与 26 子例、完整 81 项和相关 31 项的
范围分别登记，未完成运行没有计为通过。

审计促使证据归档同时排除未结束命令的 JSON 与同名日志，并在最终摘要中明确断言
sanitizer 开关、插桩及 ASan/LSan、UBSan 运行选项。执行者修改任务目录辅助脚本后，
审查者只读确认修正。审计时已完成记录的日志哈希均相符，候选内容的常见凭据格式、
私钥和 ELF/SQLite/ar 魔数扫描没有命中。最终生成包仍须独立检查，不从候选检查推断。

runner 的运行前后二进制集合不包含安装生成脚本和 runtime manifest；最终补充资产
身份只能说明最终字节，不能追认运行前后哈希。审计者未修改文件、运行构建/测试、
访问网络或操作 Git 引用，也不推断后续本地门禁、推送或远端 CI 完成。

## 并发重放最小兼容补丁的独立复核

随后完整 Release `0002` 也出现相同的原 WorkerTools 竞态，实际为 80/81、exit 8。
该问题反复影响完整验收，执行者准备最小兼容补丁和确定性回归；前述“保持冻结源、
单独重跑”的建议只属于此前尚未合入此补丁的快照，不表示最终忽略该问题。

独立 Agent `concurrency_fix` 只在任务目录候选树准备代码，生产变动是在 prepare
返回后遇到非 PREPARED 行时只读返回 operation/result；不会重新插入 owner 或派生
新的 start 意图。新增 before-prepare 观察点仅在回归测试翻译单元定义宏时生效。
`independent_recovery_review` 再次独立只读复核了红绿候选与实际来源，未发现阻断合入的问题。

| 最终候选审查输入 | SHA-256 |
| --- | --- |
| 原基线实现加私有测试观察点 | `3a733e2821b227025fe901b6eb41fabf70b91b7593dd08a15111fa8a710adcac` |
| 修复后的 `src/worker_tools.cpp` | `f5733950ed473c87a8da4e623ead70bac1410320d87c32db112376fb99a4845f` |
| 两侧相同的 `tests/worker_tools_tests.cpp` | `17d4fe1ef00e43819361dca4ac2dfcbd60df39d199f74eb066cbea890a89469c` |
| 候选精确编译/链接 manifest | `7ffc3e616954a931f17fe85e15d40a69df4d23cf23dc74eaa286f39e7c376e32` |

审查确认新增只读返回之前仍执行 session/Actor/epoch、Workspace、binding、tool grant
和 prepare 的原指纹校验。早期 failed-settlement 检查保留；PREPARED 路径保留第二次
结算检查、admission lock 与 Store CAS。UNKNOWN、CANCELLED 及其他状态原样返回，
没有新 claim、事件写入、权限恢复或进程退出推断。改变内部 start 的 expected version
或 prepare 参数时，Store 原指纹仍拒绝。

新子例让迟到调用先经过空查询并在 prepare 前等待，owner 同步执行到完整返回、
移除 active owner 后才释放迟到调用。初始 key 和文件不存在；第一次真实执行增加
一个 operation/result、一次工具计数和三条原事件，task 版本只推进一次；迟到重放
不改变原 operation/result、request/task、事件前缀、预算/attempt 或文件 device/inode、
size、mtime、ctime 和内容。改变意图的三种调用继续被拒绝。异常路径释放门并 join
线程后才重置 observer。原并发与 prepared-call failed-settlement 用例的函数体和期限不改。

执行者随后分别使用原基线与 feature 的头文件、资产和库进行实际编译/链接，避免
StoreActor ABI 混用。原基线 Store 库未修改；红例只在测试 TU 内编译原 WorkerTools
并增加调度观察点，不冒称完全未插桩的二进制。红例 18/19、exit 1、4.862053 秒，
唯一失败是迟到 prepare 的原命令指纹冲突；绿色候选 19/19、exit 0、17.203872 秒。
这使该失败从源码时序分析进一步成为实际确定性反例。原始 argv/log、源和库/二进制
身份记录在本批 evidence 中。新增的是原 worker_tools 套件的一个子例，项目仍为
81 项 CTest；R2.4 新增三个套件的 26 子例另计。

候选开发者和审查者没有编译或运行测试；上述执行结果由主执行 Agent 负责。最终
合入与新冻结输入后的完整门禁仍须以验证报告及对应实际记录为准。

## v3 结算故障会话隔离与最终回归的静态复核

按需重开保留 epoch 后，原内存失败键 `(team_id, execution_epoch)` 会把旧 Store
会话的局部结算故障继续施加于新会话，即使原持久 UNKNOWN 已经合法 Root 核对。
执行者准备的最小补丁只将该键细化为 `(writer_session, team_id, execution_epoch)`。
`independent_recovery_review` 对 tuple 及新增测试作独立静态复核，未发现阻断该补丁
合入的问题；审查者没有编译或执行红绿测试，也没有核验最终完整门禁或发布。

| 此次复核与正式红绿使用的输入 | SHA-256 |
| --- | --- |
| v2 红例 `src/worker_tools.cpp` | `f5733950ed473c87a8da4e623ead70bac1410320d87c32db112376fb99a4845f` |
| 会话键修复后的 `src/worker_tools.cpp` | `aedf1cfc9c428ae74b68da7922a57558996733ebe08832f809fa8c15467b689a` |
| 红绿两侧相同的最终 `tests/worker_tools_tests.cpp` | `9abbac0b086d8e037ca3679fbe4e3c6f40e700afbcf331de8e790386c288e8ce` |
| 候选编译/链接 manifest | `5b88a4c32c4b8dec9f47faa2b014eb43e6f3bdcc35809cfaf92a678b033dbe8e` |

静态确认的边界包括：

- 业务早期准入、首次派发前复查及故障登记使用相同三元组；首次派发与结果保存/故障
  登记仍受同一互斥边界保护。此前通过检查但尚未获得 claim 的调用仍不能越过后来登记的故障。
- 同一 Store、团队及 epoch 内重签 `host_actor` 或重建 WorkerTools 不改变故障键。
  合法 Root 核对持久 UNKNOWN 不会删除内存标记；新 Store 会话只隔离旧局部标记，
  持久 UNKNOWN、当前 Actor/epoch、请求/任务、Workspace 和 CAS 守卫继续独立生效。
- 原故障表 4096 条上限、旧记录保留及分配失败时的进程级容量耗尽标记不变；全局
  标记不能由换 Actor、WorkerTools 或 Store 解除。进程重启后仍需处理持久未决事实，
  不据此推断进程退出、清理资格或业务执行授权。
- 新子例通过真实 SQLite trigger 使确定结果事件写入失败，并保留真实文件效果和
  UNKNOWN 兜底；没有 mock Store 身份、核对或权限条件。它覆盖先核对后重开及先重开
  再核对，同会话重签/重建仍拒绝，旧 Actor 的新业务及原 prepare 精确重放均被拒绝。
- Root 核对保持原 CAS/角色检查，原 result、输出、历史前缀、预算/attempt 和文件
  字节及身份不改写。新业务只产生一次合法效果，精确重放不再执行。原 19 子例、
  helper 和测试期限字节保持，两个核对顺序仍是同一新增子例的分支。

静态复核期间修正了新增夹具对核对后 `evidence_ref` 的投影：只还原原核对契约允许
改变的 status、state_version、ended_at、evidence_ref，再比较其余原操作事实。
后续编译发现 Boost.JSON value 与字符串直接比较的 C++ 类型问题，修正为使用既有
`text` 提取后比较。最终测试源为上表 `9abbac0b…`；没有改动生产核对路径或原断言。
初始源、manifest 和两次 exit 1 编译日志保留，不能把编译错误当作产品行为反例。

主执行者随后实际编译并运行同源红绿：v2 红例 **19/20、exit 1、20.590175 秒**，
唯一失败为新会话仍受旧局部围栏阻断；绿例 **20/20、exit 0、23.777536 秒**。
两侧都使用 feature v2 的 Store 库、头文件和资源，测试翻译单元分别提供 v2 或修复后
WorkerTools，**不是 pristine 原始基线反例**。实际 argv、源/库/候选二进制身份及日志
见 `settlement-counterexamples.json`。候选二进制身份是链接后的记录，不等于 CTest
运行前后的完整哈希证明；这些运行由主执行者负责，不是审查者执行的测试。

本批在原 worker_tools 套件增加了两个子例（late-prepare、结算故障会话隔离），共
**18→20 子例**；项目仍为 **81 个 CTest 注册项**，R2.4 三个新增逻辑套件、26 子例另计。
最终源码已冻结为 v3、312 项，清单 SHA-256 为
`59c8310d64a961d2ebac1159b93e8aa925499be61795b06831a96d79e2fa2050`。
v2 三项完整原生门禁通过和 ASan 独占四项超时均保留其快照范围；静态复核不推断
v3 完整验证、本地 sanitizer、推送或远端 CI 已完成。


## v3 实际合入及契约复核

`independent_recovery_review` 最后只读确认实际 feature 的 WorkerTools 实现和测试与
最终 Green 候选逐字节相同；`source-final-v3.sha256` 的实际摘要与元数据一致，
312/312 项构建运行输入全部匹配，无缺失或摘要漂移。四份契约准确区分局部 session
故障、4096 条记录和不可由换 Store/epoch 解除的进程级容量耗尽，保留 UNKNOWN、
Root/CAS、原进程退出与清理边界，没有将验收要求写成 sanitizer 或发布已通过。

| 实际审核文档 | SHA-256 |
| --- | --- |
| `docs/implementation/R2-demand-driven-recovery.md` | `a32fd24c590d9f95b41a67795af79f6dcf39b00dc1e8f06187cd5f5d24c5ec06` |
| `docs/adr/091-demand-driven-request-recovery.md` | `863065b89d3e984dc0d06b248276c4b20ace0d7045a4a1f07dcc0331833fa861` |
| `docs/implementation/S1-worker-tools.md` | `9d3c47af52d870248e8d61723f96884c61a6c2e34dfe2cf4f511ee28c2719de0` |
| `docs/implementation/S1-process-settlement.md` | `52fb812eb7dda885193e2961046aac4ce93129114c4e067f2046adc8d13ad031` |

审查者读取结算红绿原始日志并核验其摘要：v2 红例 19/20、exit 1，唯一失败为新增
会话故障子例；绿色候选 20/20、exit 0。明确红侧不是 pristine 原基线，两个核对顺序
不是独立套件。本轮未编辑文件或运行构建/测试，不从静态复核推断最终门禁或远端 CI。


## v3 五项最终门禁的独立证据审计

`evidence_audit` 只读核验机器摘要 SHA-256
`78c718189cf951ac6a61352ee8390aa46d17502b91196a6377910abb1f7fb2fc` 与五份完整原始记录：
Release 两种 umask 及 Clang 各 81/81、exit 0；UBSan 31/31、exit 0；ASan 19/31、
12 项 Timeout、exit 8。摘要正确保留 `local_validation_complete=false`，ASan 是正常
结束后的 FAILED，没有改写成 Interrupted 或通过。每份 argv/cwd/普通 UID/GID、umask、
affinity、capability、选项、312 项源输入和 57 项二进制/构建身份均与记录/计划一致。
五份原日志、五份详情、最终生成配置里的原 timeout 和当前文件摘要相符；四构建、
323 个旧文件、12 个最终补充资产也已核对。

审计明确保留 ASan 原日志三个 unittest FAIL，不能只用外层 Timeout 遮蔽内层失败。
当次审计尚未覆盖后来完成的方法诊断，不从配置正确或原生通过推断其根因。
两个 sanitizer observer 及最终进程检查与摘要相符，没有将当次空进程列表作为后续
诊断结束的证明。审计者未修改文件、运行测试、生成归档或操作 Git 引用；最终包另审。

## 冻结证据包与后续诊断的独立审计

`evidence_audit` 完成最终只读审计：归档 SHA-256 为
`45ac14ee3558dfb8f18054f4d543d0598297c7032a012e457943ed806bea4f6c`，339 个成员，
压缩大小 4567874 字节、展开内容 16784099 字节。INDEX 的成员集合、大小和全部摘要
与原始文件一致，没有重复成员、危险路径、链接、可执行/数据库文件头或常见凭据模式。
27 份早期 runner 因缺少 `binaries_before` 被标记为 observation，但均有真实整数
退出码、结束时间和匹配日志；按更宽的命令记录结构核查，103 份实际命令全部已结束。
包和机器摘要完整保留 ASan FAILED/exit 8 及 `local_validation_complete=false`。

四次后续方法诊断均 exit 1，原测试、driver 和评测脚本与基线字节相同；计费保留
multi_agent/single_agent 两种条件及所有原期限。详情观察器只在原失败登记后读取
白名单字段。工作流首阶段 deadline 为 60274/60000 ms，次阶段 quiescent 为
30276 ms，两阶段 provider_calls 均为 0；两项计费 traceback 均为原 deadline 断言。
这些诊断没有 CTest 式二进制集合，另存的脚本/driver 身份和序列前后摘要均匹配。
当前 312 项输入匹配 v3，323 个历史受保护文件无差异，WorkerTools 与 Green 候选相同。

审计未执行测试或归档代码，未编辑文件或移动 Git 引用。其结论是证据完整且状态如实，
不表示本地验收或发布完成；也没有将 deadline 观察解释为仅由环境导致。冻结包不因
后续诊断而覆盖；新增定位记录单独保留，结论须由各自实际记录支持。

## 性能定位及未采用候选的限定复核

后续原 v3 event_runtime 的 perf 诊断仍为原 90 秒 Timeout、exit 8。`evidence_audit`
另审 17 成员的文本补充包，SHA-256 为
`35c838fe112aa648b05f44f6f192fc0b3097fd39b7d356191dd1600898f710b8`；312 项源和
57 项构建身份未变，原总包和机器摘要未覆盖。relative 与 absolute 的分母区别、
inclusive 重叠和 frame-pointer 栈截断均已保留，未把采样作为完整性能或验收证明。

`independent_recovery_review` 只读检查了外部 serializer 复用候选：原源摘要为
`60282aee148eaa5f11a2b7b4ad9a5b4a59c0a4bf87efa5127ec942fc8dde83a9`，候选为
`6ceaa14059acf90127ec91d72c80bfb0064654cf189659a1101d540f0fd9462d`。
相同 Boost reset/read 路径、逐项 drain、原排序/数值拼写/finite/depth/最终 DOM 校验
保持，没有跨调用缓存或新授权路径；静态未发现阻断性语义问题，不据此认定兼容测试通过。
主执行者实际运行相同 v3 原测试对象、原 90 秒期限的顺序对照，两边均 0/1、exit 8：
原实现 90.506768 秒，候选 90.529404 秒。候选未解决门禁，`NOT_ADOPTED`，没有合入
feature。额外 oracle 仅保留未编译源码和清单，状态 `NOT_RUN`，不计入回归套件或通过数。
v3 实际交付输入仍是原 312 项，未扩大为一次未经验证的核心性能改造。

`evidence_audit` 又完成 49 成员候选证据包只读核验，包摘要为
`92bedd5a7547f3792f7b1d3eb976c93f8a3a9ceab9b5a905ea862e7e9dd2b0da`，验证记录摘要为
`9d48d318e481a72ea7f6ee252b83048b4f0074f7b0638e7374c89afd7879b796`。
INDEX 与原文件、312 项 v3 输入及 744 项探针引用相符，源/对象/库/原测试对象/链接产物/
CTest 身份关联一致；构建成功与双侧超时、oracle 未运行均分开记录。map、对象和可执行
文件只保留本地引用，不入包；原总包、profile 包、机器摘要和计划保持原哈希。
审计没有新增测试、构建或 Git 动作，完整性通过不表示 ASan 验收完成。

## 发布前 v3 源码及历史范围复核

2026-10-29 18:10 UTC，`independent_recovery_review` 再次只读核对最终发布范围，
结论为已审 R2.4 v3 范围无阻断性源码发现。新增记录
`final-publication-independent-review.json` 的 SHA-256 为
`5c6386f040727d3347ce22c126939f56bbf2ba8e404df7ec5f763f64aef931c5`。
312/312 构建输入仍匹配冻结清单；生产变更仍只有 Store 头文件、Store 实现和
WorkerTools，没有采用外部性能候选。直接对照基线 Git blob 和模式的 323 项原
SQL、third_party、handoff 及历史报告均保持一致，`git diff --check` 为 exit 0。

复核确认按需判定在原写者锁和事务内，旧 session 在精确重放前被拒绝；迟到 prepare
只返回原事实，结算故障三元组及互斥边界一致，4096 上限、全局耗尽、UNKNOWN、
清理 claim、进程退出、资源所有权和终态业务门禁保持。兼容测试保留原安全断言和
期限，旧跨 epoch 覆盖通过显式恢复触发。此次未编译、测试、访问网络或移动 Git
引用；当时完整 ASan 重验仍在运行，最终证据和远端结果须以随后实际记录为准。

## 同源完整重验后的独立证据审计

完整运行结束后，`evidence_audit` 独立只读核验本轮 31/31 ASan/LSan、exit 0、
1243.067519 秒的原日志与最终详情，以及同 v3 的其余四项通过门禁。
312 项当前源码、57 项当前二进制/构建身份与原失败、探针和完整重验前后记录一致；
原 31 个注册名、timeout、插桩、泄漏检查、普通用户和零 capability 条件均保持。
独立结论为 `audit_passed=true`、`blocking_findings=[]`、
`local_validation_complete=true`，没有由静态审查推断测试成功。

`resumption-independent-audit.json` SHA-256 为
`e339a3cba709e54c96159b465690509300492a22f972ce5ecb5afb26a9d7b7c9`。
补充包的 49 个成员均与 INDEX/原始文件一致，12 份命令记录全部结束且原日志摘要匹配；
旧三包、历史失败摘要和原计划保持原 SHA。外部候选仍为 `NOT_ADOPTED`、测试
`NOT_RUN`，没有将准备构建计为验收。没有发现危险成员、数据库/二进制载荷或
常见凭据模式。审计未执行测试、编译、生成器、包内代码或 Git 动作；此结论覆盖
本地验证，不认证随后提交、推送或远端 CI，也不推断历史失败的唯一环境根因。
