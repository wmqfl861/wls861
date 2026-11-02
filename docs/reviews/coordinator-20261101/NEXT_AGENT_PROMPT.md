# 本地 Agent 接手指令：复审后 N0–N4 整改与后续准备

## 0. 职责、结论与实际范围

仓库：https://github.com/wmqfl861/wls861

主协调者负责独立审核、架构和计划；你负责实际开发、服务器/平台验证、正常提交与推送。每次完成都交回精确 SHA、报告、证据和下一步，不自行代签主协调批准。主协调者的交接必须同时包含审核结论和后续计划；本次先完整阅读同目录 `REVIEW_AND_HANDOFF.md` 与 `AUDIT_EVIDENCE.json`。

两项原问题 R-A3-01 / R-A4-01 在后继实现中已由主协调者重跑原探针、现有六套测试，按限定范围认定修复有效。不要再无依据重写 AMEND 控制或资源调度。旧 99c9063f 的 CHANGES_REQUIRED 属于旧版本，不能删除历史。整体仍为 VALIDATION_BLOCKED：原机 sanitizer 失败、分支未整合、Windows 目标验收未完成。不授权任何 main/PR 合并或产品发布。

本提示词直接派发 N0–N4。N0 候选分支整合、N1 定向定位与最小修复、N2 回归/证据、N3 原生目标验证可以实际执行；N4 本轮只做可审阅设计与离线夹具，不展开全部 B0–B6。遇到阻塞只暂停受影响链路，继续其他独立任务，不反复询问已获准的普通操作。

不允许强推、丢弃他人工作、覆盖历史报告、放宽断言/期限、关 sanitizer、真实 WLS 供应商调用、付费产品运行、生产数据操作、公网服务或采购资源。端点、模型、认证、预算外部配置，未来模型也只走用户配置中转，不硬编码官方直连。已获用户授权的本地编码工具可按原范围使用，但其使用/退出/费用与 WLS 产品调用数分开登记，不宣称总工具费用为零。

## 1. 精确输入：不要只拉旧 main

| 输入 | SHA / 分支 |
|---|---|
| 主要审阅与报告源树 PR #14 | `4289589384d75f29d42233cbaad256ceb711e9bf` / `docs/zcode-final-validation-20261101` |
| 它的代码祖先 PR #12 | `08dad7e815b9c3c84d3bd2393818b2030a6e0cde` / `fix/zcode-local-validation-20261101` |
| 计费入口兄弟补丁 PR #13 | `ceaa9b3e18618c592345001dfc597bdaf579cf81` / `fix/billing-subtest-diagnostics-20261101` |
| 两项原 P1 修复 PR #11 | `7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a` |
| 原红例基线 PR #10 | `99c9063f3e29cd6d69c56fb15815715d499a374a` |

关系为 #10 → #11 → #12 → {#13, #14}。#14 不包含 #13。远端与本机结果分别绑定原输入，不能拼成一个“全部完成”的集成结论。

审核资料在独立分支 `docs/coordinator-handoff-4289589-20261101` 的 `docs/reviews/coordinator-20261101/`。发布精确 SHA 见交接评论/主协调者消息。先 fetch 该分支，从冻结提交读取资料；资料发布不改变产品基线。历史复现附件仍在 `docs/review-attachments-99c9063f@ce518883d890d4bef62457aeed2e1b4ff1ec6ec4` 的 `docs/reviews/attachments/99c9063f-20261031/`。

## 2. N0：数据盘安全接手和单一候选整合

先核验用户、挂载、空间、仓库 remote、工作区、worktree、运行进程。历史数据盘 /mnt/d 必须由 findmnt/lsblk 实际确认。源码、构建、tmp、日志均在已核验数据盘；普通用户测试。不要动全局 /tmp、生产目录或其他 Agent 进程。

读取 AGENTS、STATUS、ROADMAP、本交接、现有 A3/A4 契约/ADR-092/093、验证 runner 契约、PR11–14 实际差异与结论。相关文件以真实仓库为准，不编造接口。保留四角色、Store 单写者、Actor/session/epoch/CAS、唯一派发、UNKNOWN、原预算/证据及清理条件。

已有仓库先 `git fetch origin`，记录 PR head/base 与以下两个远端分支的完整 SHA。不要直接 `git pull` 合并到当前开发树。若 head 已前进，先审差异；未知冲突隔离，保留已知固定基线，不覆盖。

在确认两个固定提交存在且关系正确后，建立数据盘独立 worktree 和唯一命名的候选分支，例如：

```sh
git worktree add -b work/review-followup-<唯一后缀> <数据盘新目录> 4289589384d75f29d42233cbaad256ceb711e9bf
cd <数据盘新目录>
git status --short
git merge --no-ff ceaa9b3e18618c592345001dfc597bdaf579cf81 -m "Integrate reviewed billing diagnostics into validation candidate"
git diff --check
git log --graph --oneline -8
```

尖括号用实际值代入。本次明确允许的是**新候选分支上的普通本地整合**，不是合并 GitHub PR、写 main、移动原 PR head/base 或强推。合并后核两侧均为祖先；相对 #14 只应多出 #13 已审阅的补丁与相关资料，异常变化先处理。保留原日志入口和全部历史资料。

完成 N0 的合并提交后再做不同模块的小提交，确保未提交改动不混入正式 runner。新候选建立 Draft PR，base 选择实际直接祖先 #14 分支并明确包含 #13；不得新建一条与原功能无关的空壳分支。

N0 验收：单一、可追溯的候选包含两支成果，有冻结 SHA、干净输入和最小差异。其他旧 PR 保持原状态。

## 3. N1：定向诊断，不再盲跑全矩阵

已知 code08 正式本机结果是 ASan 48PASS/2FAILED/42Timeout，UBSan 64PASS/2FAILED/26Timeout，各92项；Release/Clang strict通过。GCC 配置对齐仍失败，不能再认定 Clang/-O2 是唯一原因。保持历史原因 OPEN，保留 `historical_clang_asan_failures_closed=false`。远端绿色不能改本机记录。

### N1a：明确的 HTTPS 部分上传失败

优先分析 `test_partial_request_upload_deadline_is_not_free_retry_authority`。两类 sanitizer 均在 `len(peer.prefixes)==1` 得到0，而不是在后面的上传字节或再次发送授权断言失败。保持 timeout_ms=3500、padding=900000、原 TLS/预算/唯一发送语义。

先保存原 ELF/脚本/库/配置身份和实际错误，再按可区分假设定位：准备阶段耗时、peer 服务就绪、握手/首次写入、客户端 deadline 起算、线程调度或底层传输。观测只记录数值阶段、单调时间和非敏感计数；不得公开请求正文、配置全对象、真实 header 或凭据。

原二进制单次诊断与派生插桩二进制分开：派生 ELF 通过不代表原 ELF 通过。不要通过增加 deadline、降低 payload、提前制造 prefixes、mock 发送或弱化“不免费重试”守卫修复测试。确认为夹具就绪竞态时可改为真实就绪屏障，须证明不改变业务 deadline 起点/断言，并给同条件反例。确认为生产热点/状态错误则给最小修复、原证据和回归。

### N1b：明确的内层期限失败

分别定位而不合并计数：
- ASan `live_model_store_tls` 的容量方法18秒子进程 TimeoutExpired。
- 同一个 CTest 中 large escaped valid output 方法17.29秒不小于原16秒断言。
- UBSan `r1_structured_cli_smoke` 的容量场景 `pump-fake` 原50秒子进程 TimeoutExpired。这是 CTest Failed，不另计外层 Timeout。

取得完整即时 traceback 与原动作阶段，区分原格式/账目容量、上下文重复编码、序列化/校验、SQLite 等待和进程启动开销。发现热点后才优化；不去掉校验、不跨事务缓存可变状态、不丢弃历史、不扩大原预算。重复编码减少一次只是机制变化，不是整体性能根因证明。

### N1c：代表性 native/runtime 超时

从完整 START/PASS/FAIL/RESULT 定位首个未完成方法，选择少量代表场景，而不是又跑42个超时。先写假设、要观测的数据、预期可区分结果、停止条件。正式运行前记录 CPU配额/节流、频率/steal、内存/交换/压力、挂载、编译器/链接和 sanitizer实际flags；信息仅用于因果对照，不能靠一张负载截图宣布环境原因。

同一时间只执行一组正式重编译/sanitizer；不杀其他工作负载。允许有限诊断剖析，但诊断参数与正式门禁分列。无新证据不要无限重复同一运行。不可复现则写明边界与未排除因素；不要编造缺失的旧 traceback。

N1退出：每个失败有准确分类、最小证据、已确认原因/未确认边界、真实修复及验证，或者明确未解决的阻塞范围。至少交回实际诊断结果，不只写建议；也不为了宣布“修复”制造无关生产改动。若仍未闭环，暂停其产品发布结论，继续N2/N3/N4独立任务。

## 4. N2：常驻回归、完整日志与隐私边界

### N2a：补充异常后资源队列回归

在实际 AsyncCoordinator/M1/Store 链路中加入 adapter 抛异常、同资源跨池后继已排队的确定性用例。明确期望：原异常事实/UNKNOWN按现有契约保存；错误不能变成成功；被保留/未结算的前驱不能让后继绕过，合法完成或允许的退出路径也不能永远占执行槽。检查唯一 claim、实际 callback 次数、FIFO、双审核预留、control/close和资源/结果容量。

使用真实屏障，不用 sleep 或扩大线程/预留/期限。既有实现可能通过，这是补覆盖；允许隔离单守卫对照证明敏感性，但不能冒称发现历史生产缺陷。不得因为补例失败就先改断言。

### N2b：Store event 常驻兼容回归

复用已有同源探针的语义断言，把 command原字节/hash/payload、原Actor/CAS、预算/epoch、合法重放、foreign-session拒绝以及代表性UTF-8/depth/byte限额回滚放入可持续执行的回归路径。一次编码计数可作为限定工具链探针，不以实现次数替代行为契约，不引入生产注入接口。

优先增加现有suite方法，保持名称和期限；确需新CTest则同步审查目录增量、CI与runner预期集合。不要机械把92改94，也不要自动接受任何当前数量而失去缺项检测。

### N2c：计费入口集成验证

在N0候选上执行真正的 `tests/test_immediate_unittest.py` 全部5个工具方法，包括billing __main__入口。保留原loader/无外部效果约束和子进程回收。该5方法不是5个产品CTest。随后运行受影响billing/diagnostics产品回归，不能继承兄弟分支绿色。

### N2d：完整 CTest 日志保留

共享 GCC runner 目前的成功JUnit片段被截断，artifact没有完整LastTest.log。在成功和失败/中断可收尾时保存原 `Testing/Temporary/LastTest.log`、LastTestsFailed（存在时）、JUnit、命令/终态/身份和小索引。保证没有完整退出记录时不能计通过；缺日志须明确告警/未完成，不编造。

增加自检覆盖成功文本超过1024字节仍完整保存、失败、启动失败与中断。保持原期限、asan选项、异常和退出码，不能通过关闭输出截断掩盖证据遗漏。

原日志只追加，不回写。公开产物需逐项安全检查；敏感host doctor对象、真实headers/配置/正文不得进入公开ZIP。保留私有原件及其hash，公开投影单列来源/范围/字节/hash；未脱敏范围逐字保留。隐私guard对已知拒绝样本应红，对准确交付包应绿。不要把整个服务器目录打包。

## 5. N3：Windows 证据与目标环境

本轮不是完整Windows runtime移植。先修证据来源问题：同run/job/attempt/checkout的Runner Image.Version `20261025.346.1`与artifact环境ImageVersion `20261026.347.1`不一致。核对采集时点、环境变量是否继承/改写、下载artifact所属运行、原job日志和收据字段语义。不要选有利值覆盖另一份原件，不据此直接断言篡改。

提供 Windows11 x64、NTFS、普通非提升用户的现有组件可复现验证入口；有已授权实机则真正执行，记录OS build、工具链/SDK、token权限、文件系统、完整LastTest及4注册/内部方法结果。没有该机器就保持NOT_RUN，同时完成入口/来源链修订；不购买资源、不把Server2022提升权限验证改称Win11普通用户。

如果WindowsCI仍使用有提升权限runner，可以增加受限普通测试账号的显式隔离验证，但必须准确记录实际OS/权限、只在获准临时runner上执行，不改用户生产主机账户策略。它仍不自动等价Win11。

不得将原生config/scheduler/resource切片说成完整Store/VFS/文件/进程/沙箱/cleanup实现。Job Objects单独不是强沙箱。缺能力保留拒绝，不用WSL/Wine结果代填原生。

## 6. N4：下一开发批的可执行设计，不做全量扩张

依照交接报告的后续路线，交付三张最小设计/验收卡，引用实际接口而非想象API：

1. 单协调写者异步驱动如何接原持久inbox/claim/ack/反馈：捕获目标、一次派发、部分完成只ack实际ID、等待/AMEND/取消/恢复、预算和未结算结果回收。明确原同步CLI兼容和opt-in方式，先FakeModel纵向用例。
2. S2最小DAG与独立审核：无环/版本/失败传播、固定候选hash、producer/reviewer epoch票据、拒绝后有限返工及审核双容量。Review调度类别不等于业务审核已经实现。
3. Windows最小只读对象身份/资源读取切片：根Handle、卷/文件ID、重解析与ACL/父目录替换风险、支持矩阵与拒绝出口。先只读允许/拒绝证据，再后继扩Workspace/Store/进程；HANDLE不伪装POSIX fd，旧SQL/资源字节不改。

每卡写目标、当前代码入口、接口/持久事实、兼容约束、最小正负例、依赖、退出条件、未支持范围。可以整理离线夹具，不实施未经审核的持久schema大改、多写者协议、任意run、真实供应商接入或整套记忆/MCP/A2A。

未来路线保留：稳定候选门禁 → 有限异步工作流 → DAG/独立审核 → 多层记忆 → MCP双向/第二适配器 → 鉴权服务/A2A/治理。Windows前置并行，不等S5；真实S1验收需端点/模型/计费/预算/调用另授权，不阻塞无模型工程准备。

## 7. 统一验证、提交与交接

修复确认缺陷时同业务条件红绿；补已有正确守卫时基线可通过，不伪造旧缺陷。记录实际源码/headers/libs/资源/ELF，不混不同版本。原92目录与deadline基线从 `tests/gcc_asan_expected_tests.json`读取，新增项人工审查。

定向定位形成合理候选后再冻结单一SHA，执行受影响测试及完整Release两种umask、Clang strict、UBSan、共享GCC13.3 ASan/LSan；完整远端六路和相关Windows/fairness/source收据属于新输入。使用原runner和原venv，清洁独立目录，不复用旧cache。不能把部分工具探针、其他机器、旧SHA或Make调用数写成同一完整CTest结果。

N1仍未解时可以以Draft正常推送有用模块，不勾选LOCAL_VALIDATION_COMPLETE或正式PR审核。保留首次失败与所有中断；不以无限retry获得唯一成功。对系统资源限制的潜在例外，需要单列证据和风险接受决策交主协调者，不得自行豁免。

建议独立worktree分工：N1诊断，N2回归/runner，N3Windows，N4设计；一个集成人维护共享Store/header/build/CI/STATUS文件。每项完成即小提交正常推送；候选集成冲突先审，不强推。不要新造重复证据包堆仓库，优先小的可读报告/索引与受控完整artifact。

最后交回：
- 数据盘和工作区、起始/集成/最终完整SHA、PR依赖及远端读回。
- N0–N4逐项实际代码/资料与演示、变更理由、测试和非作者复核。
- 具体失败逐项原因/未解边界；passed/failed/timeout/skipped/notrun按环境/输入分表。
- 原日志/ELF身份、完整方法输出、脱敏公共副本的对应范围及CI链接。
- Windows实际OS/权限与NOT_RUN项，真实WLS调用数和授权编码工具使用分开。
- 后续三张设计卡、建议先后顺序与主协调者需决定事项。

IMPLEMENTATION_COMPLETE、LOCAL_VALIDATION_COMPLETE、PUSH_COMPLETE、REMOTE_CI_COMPLETE、PR_REVIEW_COMPLETE、MERGED分别登记。工程自审不是主协调者批准，mergeable不等于merged。

现在先获取本交接、核数据盘及最新远端，完成N0，再开展N1/N2。N3/N4在独立范围继续，不让历史超时阻塞全部进度，也不绕过产品发布门禁。
