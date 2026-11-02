# wls861 项目复审、整改与后续开发交接

审阅基准：2026-11-01。主协调者负责审核、计划和本报告；本地 Agent 负责后续源码开发、环境执行、测试、提交与推送。本轮未修改任何产品源码、产品测试、CI 或既有报告。执行审核所需的原测试/既有复现探针，不等于代替执行团队开发。

**结论：两项原 P1 缺陷在本次审阅的后继实现中已通过限定复核；整体产品与本机验证仍未通过，不批准合并或发布。** 本报告是绑定精确输入的协调者意见，不是 GitHub APPROVE。旧 `99c9063f` 的 CHANGES_REQUIRED 仍是该旧版本的正确历史结论，不回写历史。

## 1. 精确输入与分支关系

| 对象 | 完整 SHA | 本轮判断的范围 |
|---|---|---|
| 旧审核基线 / PR #10 | `99c9063f3e29cd6d69c56fb15815715d499a374a` | 两项旧缺陷与历史反例来源 |
| 原 P1 修复 / PR #11 | `7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a` | AMEND/cancel 重放、资源头先入池、生命周期回归 |
| 后继实现 / PR #12 | `08dad7e815b9c3c84d3bd2393818b2030a6e0cde` | 继承 #11；即时日志与 Store event 字节复用 |
| 计费测试入口 / PR #13 | `ceaa9b3e18618c592345001dfc597bdaf579cf81` | 继承 #12；接入 ImmediateTextRunner，新增入口自检 |
| 报告和证据 / PR #14 | `4289589384d75f29d42233cbaad256ceb711e9bf` | 继承 #12；本轮主要源树，**不包含 #13** |
| main 读回 | `a6b8559018a599d7a806b05942857a56d69a00ce` | 仍不是这些未合并成果的集成入口 |

依赖为 `#10 → #11 → #12 → {#13, #14}`。#13 与 #14 是兄弟分支；“读取了 #13 的报告”不等于代码已经包含 #13。下一批必须建立明确包含两者的候选集成分支，不能直接 pull 旧 main，也不能把两个不同 SHA 的测试相加当作集成验证。

审核固定源树为 `75c8c9f703ca6560d5f9b0095095dacb80d0642b`。本轮独立下载 source artifact `13081626278`，ZIP SHA-256 `9d60a5359cb7884e6eacd53a18cb63c85419b27fdf39428b1ce75f4df3810bba`；核对 source.tar.gz 与 SHA256SUMS，按 801 个文件 / 38,607,330 字节和 Git 执行模式重建同一树。审核结束，原 801 个文件字节与模式未改变。

## 2. 审核范围与方法

本轮读取实际源码差异与契约，重点检查 AsyncCoordinator 的控制、入池、claim、结算、保留、关闭；交叉读取 Scheduler、Store 的相关授权/事件路径、公开接口与新增回归；核对诊断工具和 Windows 证据收集路径；静态读取 #13 的真实 patch。另审查 A2 离线包契约、当前阶段/依赖和本机失败原始公开投影。

这是交付范围、关键代码路径与证据的独立复核，**不是逐行穷尽审核整个仓库全部实现，不把 801 文件哈希核验称为 801 文件语义审核**。本机私有原始日志及其所有投影来源未直接访问；公开索引、归档字节与若干原日志投影已独立读取。未接入用户服务器、未运行真实供应商、未执行 Windows 实机。

审核执行环境是本会话隔离 Debian Linux，普通 UID 1000，GCC 14.2，Debug `-O0 -g0`、严格警告，ASan/UBSan OFF。编译使用系统共享 libcurl，测试采用审核专属临时目录。**不是用户 Ubuntu 环境，也不是 GCC 13.3 正式 sanitizer gate**。

### 本轮新执行

| 检查 | 实际结果 | 限定含义 |
|---|---|---|
| scheduler / runtime_types / store / event_store / r24_recovery / async_coordinator | 6/6 CTest，exit 0，约 34.12 秒 | 原六项期限不变；其余 86 项本轮未执行 |
| 上述内部方法 | 9 / 6 / 32 / 15 / 17 / 29，均 0 failed | 分别属于六项，不另加为新的 CTest 注册 |
| 原审核附件 AMEND 探针 | exit 0；`new_call_stop_requested=0` | 原复现源码未改，配后继版本自身头文件/库与夹具 |
| 原审核附件跨池探针 | exit 0；`cycle=0`，queued/awaiting/executing 均归零 | 原复现源码未改；合法业务完成另由现有 29 方法中的四调用回归验证 |
| Store event 同源探针 | exit 0；首次 encode=1，replay=1 | 持久字段/重放/拒绝断言同时通过；不是性能加速测量 |
| GCC runner / 即时日志 / 传输观测 / fairness 工具自检 | 24/24、4/4、11/11、9/9，均 exit 0 | 工具方法，不计入产品 sanitizer；#14 仍只有 4 个即时日志方法 |
| A2 离线验收契约 | 9/9，exit 0 | 未在本轮重跑十样本 CLI 或真实质量验收 |

原附件源码 SHA-256 为 `aec4981953945e2e714564623cb15006bec97d4703ca6d02a4baa3c6595f8f84`。本轮重跑的是后继版本绿例；旧 99 红例读取上一轮原件，没有冒称本轮再次运行旧库。命令、退出码、日志摘要及边界见同目录 `AUDIT_EVIDENCE.json`。

### 远端与本地证据复核

- 已直接查询 #14 Push Linux 六项 job，全部 completed/success。独立下载该次 ASan artifact `13082461326`，ZIP SHA-256 `efa93e89255f1f3da355901cd7300e486dcf07b02d0cd5b5cbd22ac4aed9450c`。
- 该 artifact 的 head 为 4289589，92 个 JUnit 与注册名对应、无 failure/error/skipped/notrun；9 份 step 退出码均 0，原日志 hash 相符。GCC 13.3.0、Debug -g、ASan/严格警告/泄漏检查、普通 UID 1001、构建 0022/测试 0002 均有记录。801 项源码与提交逐项相符，61 项构建清单前后相等。
- 远端 artifact 不含 ELF/PE 本体，因此“构建清单相等”不是本轮重新哈希远端可执行载荷。不能把 hosted 成功改称用户服务器通过。
- #14 其余 PR/Windows/fairness 全量收据来自提交者报告与现有证据；本轮没有重新下载其全部 17 份 job 日志。已核验的 Push/ASan 与作者报告范围分列，不冒称全部重新独立执行。
- 公开证据索引中的 8 份文件大小/hash 一致；三个 ZIP 分别 174、93、572 个成员，成员路径/唯一性/非链接与 UTF-8 已检查。没有执行归档中的取证程序。该检查不证明全部私有原件的脱敏投影，也不是无限范围秘密扫描。

## 3. 两项原 P1 的处置

### R-A3-01：在已验证范围关闭

`src/async_coordinator.cpp:278–297` 先读当前 request state_version，再走原 Store 控制授权/幂等/事务。只有返回版本确实推进，才按旧 requirements_revision 或 dispatcher_epoch 停止被本次转换废止的捕获工作。精确重放不再无条件停止全部同 request 工作。

原探针验证：返回历史结果相同、请求/预算不变，新合法调用未收到 stop。现有回归还覆盖新调用未入执行阶段/在途、重复控制、同键异参、外来会话及合法终态经验收尾。保持可信宿主、单 owner 线程访问 Store 的前提，不把可公开构造的 Actor 字段当密码认证。

### R-A4-01：在已验证范围关闭

`src/async_coordinator.cpp:178–219,377–470` 将同资源后继保留在有界宿主队列，只有资源头才进入任一 M1 池；资源资格持续到结算/反馈结束。M1 请求/团队公平轮转和两类审核预留未删除。未入池的停止任务现在显式形成未派发观察，不拿无效 future 当完成证明。

原循环探针通过，现有套件包含最小 2/2、默认 4/8、合法四调用完成、资源类顺序、结算/反馈失败、控制、关闭和外线程析构。没有通过增加线程、借用审核槽或延长期限换取通过。

**覆盖保留：adapter 抛异常且后面存在同资源跨池等待者的组合没有独立新增回归。** 这是 P2 覆盖任务，不是本轮已证明的新死锁。不得为了制造“先红后绿”主动损坏生产守卫。

## 4. 其他模块结论

| 模块 | 判断 |
|---|---|
| #12 Store event | 函数内复用一次完整 `json_text(command,payload_limit)`，未加跨调用缓存；hash/payload 的同字节语义及原权限/事务可保留。探针通过，但不支持“解决了全局 ASan 超时”或量化提速的结论。 |
| #12 即时观测 | 保留原异常、顺序、deadline 和 sanitizer，工具自检通过；只是可观测性修复。 |
| #13 计费入口 | 实际 patch 仅 import/选择既有 ImmediateTextRunner、新增入口自检及报告。静态未见阻断，可纳入下一候选；本轮未重跑该分支新增第 5 方法，须在集成输入上跑。 |
| A2 | 已有离线包及契约成果保留。合成计费、FakeModel、十样本结构通过不等于供应商能力/费用/质量通过。 |
| A5 | Server2022/elevated/NTFS 的原生组件与完整方法日志可保留；不是完整 Windows runtime。 |
| #14 报告 | 当前已如实区分原代码、兄弟补丁、本机失败、远端成功和批准；报告可作为证据入口，不能充当产品批准。 |

## 5. 当前开放问题及优先级

### V-01 / P1，发布与目标环境阻塞：本机正式 sanitizer 仍失败

本轮从公开 JUnit/LastTest 投影独立重算 code08 的结果：

| 本机配置 | Passed | Failed | Timeout |
|---|---:|---:|---:|
| GCC ASan/LSan | 48 | 2 | 42 |
| GCC UBSan | 64 | 2 | 26 |

每行均是同一 92 项目录，0 skipped/notrun。不要把内部多个错误另加为新的 CTest 失败项。Release 两种 umask、Clang strict Release 的 92/92 是既有本机报告结果，本轮未在原机重跑。

须分开诊断：
1. HTTPS `test_partial_request_upload_deadline_is_not_free_retry_authority`：ASan 与 UBSan 均命中 `len(peer.prefixes)==1` 的 `0 != 1`。尚未到达后续 uploaded bytes/重试权限断言，不能据此宣布“不重复发送”已经在该次失败中验证。原 timeout_ms=3500、padding=900000 的语义保留。
2. ASan `live_model_store_tls` 内：容量场景出现原 18 秒子进程 TimeoutExpired；大 escaped 输出场景出现 17.29 秒不小于原 16 秒的断言。这两种内部失败共同属于一个 CTest Failed。
3. UBSan `r1_structured_cli_smoke`：`pump-fake` 容量场景的原 50 秒子进程期限触发；CTest 结果是 Failed，不是第 27 个外层 Timeout。
4. 广泛 native/runtime Timeout 仍需热点和执行阶段证据，不能仅凭频率归因 CPU、ext4、编译器或调度器。

旧 GCC/Clang/live_workflow 原因仍 OPEN；新 event 序列化减少一次不是根因证明。全量矩阵已多轮提供相同信号，下一轮应先定向因果定位，不继续无假设反复跑整套。

### I-01 / P1，候选身份阻塞：#13 与 #14 尚未整合

新建候选分支，基于精确 #14 普通本地合入精确 #13，保存双方父关系并审 diff。此授权仅是候选分支集成，不是合并 GitHub PR 或 main。冻结新的代码输入和自己的 CI，原兄弟绿色不能代填。

### E-01 / P2：完整成功方法日志没有进入共享 ASan artifact

本轮下载的共享 ASan artifact 有 JUnit/console/摘要，但没有完整 LastTest.log；成功方法文本受 1024 字节截断。92 项通过结论有效，但不能声称完整逐方法原文已留存。下一批在 runner 的成功/失败收尾保留完整 CTest 日志与有限索引；不得改期限或把日志补齐当成运行时修复。公开副本按既有隐私规范审核，原件保持不变。

### W-01 / P2，平台发布阻塞：Windows 身份与目标矩阵未闭环

已发布 Windows 记录存在 job Runner Image version `20261025.346.1` 与 artifact ImageVersion `20261026.347.1` 的差异；本轮从既有审核记录核对，未独立重采 Windows 原始 job。它阻止宣称精确镜像身份一致，不抹去另有依据的 OS build、编译器和组件结果。先核同 run/job/attempt/checkout 与环境字段来源，不猜测篡改或平台错误。

Windows 11 x64、普通非提升用户、NTFS 实机仍 NOT_RUN。完整 Windows Store/VFS、Workspace、进程强隔离与 cleanup 未完成。缺少 Win11 机器不阻塞 Linux 离线开发，但必须阻塞对应 Windows 支持声明。

### C-01 / P2：持久回归与阶段边界

补 adapter exception + 跨池资源后继组合；将 event 字节兼容断言纳入常驻回归，而不只依靠手动 probe。仅在原 suite 添加方法时保持 CTest 注册数；新增注册必须准确更新目录与原期限对照。A3/A4 仍是显式 offline call API，不是自动 inbox daemon、live 并发或独立业务审核运行器。

## 6. 下一批直接派发 N0–N4

详细执行指令见同目录 `NEXT_AGENT_PROMPT.md`。本批允许本地执行团队正常修改、测试、提交并推送独立 Draft；不授权主分支合并、真实 WLS 调用或付费产品验收。

| 卡片 | 实际交付 | 依赖与退出 |
|---|---|---|
| N0 候选整合 | #14 + #13 的单一候选，精确父关系、功能差异和干净工作区 | 不改原 PR base/head，不强推；冻结完整 SHA |
| N1 定向诊断/最小修复 | 三类显式 TLS/CLI 失败和代表性 runtime 超时的原因、原参数反例、最小修复或明确不可复现边界 | 每个假设先定实验，不无限重跑；原门禁不降级 |
| N2 常驻回归/证据 | 异常资源后继、event 语义回归、计费入口第 5 自检、完整 CTest 日志与隐私收尾 | 新集成输入真实执行，子例/注册数分开 |
| N3 原生目标验证 | 镜像身份来源核查、Win11 普通用户组件验收入口及真实结果 | 无机器记 NOT_RUN；不提升到完整原生 runtime |
| N4 后续实现设计 | 单写者异步驱动与原 event claim/ack/反馈的接入契约，B1 DAG/审核票据的版本/失败状态机，B2 最小只读对象身份切片设计 | 本轮设计/离线夹具，不直接扩展持久 schema、任意 run 或大规模 B0–B6 |

N1 与 N2 可在独立 worktree 并行编码，正式 sanitizer 与重编译不要互相抢资源。N3/N4 不依赖尚未解决的 Linux 历史原因，可继续。若 N1 无法闭环，明确限制该链路与发布状态，照常交回其他独立成果，不以问询或空文档替代已授权执行。

## 7. N0–N4 后的开发路线，不是本轮全量实施授权

1. **工程闭环**：在单一候选上完成适用本机/远端门禁，逐项处置历史失败；如确需环境例外，需另列能力边界与有证据的风险接受决定，执行者不得自行豁免。
2. **异步工作流接入**：保留原 Store 单写者，接原持久 inbox/claim/ack、取消、AMEND、原反馈；先 FakeModel 的跨请求有限工作流，验证只确认实际完成事件、恢复无重复。显式启用，原同步 CLI 默认兼容。
3. **S2 最小 DAG 与独立审核**：持久版本、无环/失败传播、成果 hash 与 producer/reviewer epoch 票据、有限返工和双池审核容量，先小纵向闭环再扩展；Review 排程类别本身不算审核。
4. **Windows 原生主线并行**：普通用户目标证据 → 只读根 Handle/文件身份/安全资源读取 → Workspace/锁/Store/受控进程 → 强隔离/真实退出/清理 → 双平台一致性。Job Objects 不等于完整沙箱，不伪造 POSIX 身份，不以 WSL 代原生。
5. **S1 真实验收独立门禁**：先 Linux 强环境的合成 canary 与允许/拒绝证据；中转端点、精确模型、可信计费、预算和调用授权齐全后再跑 A2 同条件十样本。没有授权不发试调用，不自动直连官方端点。
6. **S3/S4/S5**：版本化记忆发布/加载/撤销与 CAS → MCP 双向/真正第二适配器 → 本地鉴权服务/A2A/备份及治理。不得把现有 Schema、经验表或工具声明说成完整实现。

阶段退出以可执行代码、业务/拒绝证据、旧格式兼容和最终输入矩阵为准，不按提交数、模块数或文档数计进度。

## 8. 权限、交付和来源

协调者本轮决定：`R-A3-01=VERIFIED_FIXED_SCOPED`，`R-A4-01=VERIFIED_FIXED_SCOPED`；`OVERALL_ACCEPTANCE=VALIDATION_BLOCKED`；`MERGE_AUTHORIZED=false`。接受两项修复不等于批准整条未合并 PR 栈。不得将远端成功改成本机成功，不回写 `historical_clang_asan_failures_closed=false`。

本地 Agent 每批返回新 SHA、PR 关系、变更范围、原反例/修复证据、每配置 passed/failed/timeout/skipped/notrun、完整日志位置、未解决原因与下一步。真实 WLS 模型/付费调用仍未授权；已有本地编码工具按用户原授权使用，使用/退出/费用不与 WLS 产品零调用声明混写。

主要核验入口：
- PR11：https://github.com/wmqfl861/wls861/pull/11
- PR12：https://github.com/wmqfl861/wls861/pull/12
- PR13：https://github.com/wmqfl861/wls861/pull/13
- PR14：https://github.com/wmqfl861/wls861/pull/14
- 精确 #14 Push Linux：https://github.com/wmqfl861/wls861/actions/runs/41301131765
- 精确源码：https://github.com/wmqfl861/wls861/actions/runs/41301131893
- 原审核附件：`docs/review-attachments-99c9063f@ce518883d890d4bef62457aeed2e1b4ff1ec6ec4` 的 `docs/reviews/attachments/99c9063f-20261031/`
- 已发布本机失败记录：本提交祖先中的 `docs/reports/zcode-independent-final-validation-20261101.md` 与 `zcode-final-validation-evidence-20261101-index.json`。

本报告发布提交不预填自身 SHA/CI；实际分支、发布 SHA 和读取入口由发布回执及 PR 评论登记。原审核输入的 CI 不自动成为这个文档提交的新 CI 结果。
