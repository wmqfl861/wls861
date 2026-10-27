# 单 Agent 逻辑历史与 v3 评测专项验证（2026-10-27）

## 结论与范围

本模块的 Release 原生测试 **11/11 通过**，最终真实 CLI / SQLite 评测回归
**40/40 通过**。模型输出均为固定 Fake，真实供应商调用为 **0**。测试证明原角色
权限代理可以接续同一请求的完整模型历史，并能在实际多进程恢复后保留来源；不建立
真实模型质量、独立模型复核或普遍成本优势。

合同见 [ADR-089](../adr/089-single-agent-role-broker.md) 和
[运行器说明](../implementation/R3-single-agent-baseline.md)。本报告只覆盖下列独立
staging 中的专项运行。最终全树 Release、ASan、资源 manifest、CI 与发布提交由本轮
集成验证报告记录；这里不将较早的专项构建宣称为全部最终源码的重新验证。

## 实际环境和输入

独立目录为 `/tmp/wls861-single-agent-validation`，由 UID/GID **65534/65534**
运行，supplementary groups 为空。外层 umask 为 `0002`；原 C++ 测试夹具和
Python `setUp` 各自使用 `0077` 建立私有临时目录，两者分别记录，不能混称全部
测试操作均使用 `0002`。

原生构建使用 `/usr/bin/c++`、Release `-O3 -DNDEBUG`，保留项目严格告警规则；
CTest 的 `single_agent` 及后来分出的 `single_agent_runtime` 各为 **90 秒**。Python 为
`/tmp/wls861-r33a-venv/bin/python`，CLI helper 每次调用原限额仍为 **35 秒**。
未给生产限额、历史容量或判据放宽。

最终评测使用复制后固定的 CLI，SHA256 为
`4ae6766372f54651260cb346fb607d13a8674d3c66a04bef486c04e48c2e7eb2`。
最终 evaluator SHA256 为
`6bc4263e50f062504e22bfc40ee9ec582be308fdced9aeda3bc6c69836251cbf`；
测试脚本 SHA256 为
`8eed09d58280e1811977c2d5393daf80732209c1017e0f2321bfba6342285a3a`。
实际原生 staged 输入、各日志 hashes 与运行身份保存在
[专项证据元数据](single-agent-baseline-specialty-evidence.json)。这些是本地一致性
记录，不是签名或供应商身份凭据。

发布归档时仅对下面两份日志逐行删除行末空格/tab，保留原行数、每行换行和末尾换行，
不改变任何原成功或失败记录。元数据的 `evidence_files` 对应实际归档字节，
`archive_normalization` 同时记录原始/归档 SHA256、字节数及改动行：

- `single-agent-baseline-configure.txt` 保留 **17 行**及末尾 LF，1122 字节规范化为 1113 字节；原始 SHA256 为 `8acfed9ec33307146180fa0b74d7dc8883d00fbffb8d6a93f61c1f0ef286a309`，归档 SHA256 为 `58b64629b7ccc8bb6db6d0708b770e3daccdb801dd5ba3b701efaa1cf0ecdd6a`。
- `single-agent-baseline-evaluator-continuation-fixture.txt` 保留 **92 行**及末尾 LF，9356 字节规范化为 9355 字节；原始 SHA256 为 `22b8340e44cb78758066ed6f6ef523ed69bd23b949c509a2caaa50bd9f144f75`，归档 SHA256 为 `2ee8a0d3163a70e15e5d1ce966c0b1dc20c55d2240a6657192c884b26f6f971e`。

## 原生执行与负例

原生测试使用实际 Store 410、原事务、原工作区工具和固定模型响应，未修改 SQL。

| 范围 | 实际断言 |
| --- | --- |
| 跨角色、两个独立任务 | 原 D/TL/Worker 共 7 次调用，两个文件实际落盘；下一次 Worker 输入包含完整 14 条原生 assistant/tool 消息，含另一个任务和管理角色的历史 |
| 实际输入和链 | 删掉或调换原消息对，即使重算 context hash，也在新增预约前失败；更改/删除逻辑身份同样失败且预算不变 |
| 顺序与并发 | 同一前链的两个捕获不能各自继续；前者未结算时后者 NotReady，完成后旧捕获 Conflict，原预约幂等重放不追加调用 |
| 完整反馈 | 原调用已结算但缺不可变反馈时禁止下一轮，不能由宿主制造成功反馈补齐 |
| 当前工具权限 | 原 Worker 的 `fs_write` 历史不授权 Dispatcher 写入；原不可用工具错误及反馈保存为 user observation，未创建越权文件 |
| 恢复 | 重新打开选定请求后接续同一逻辑 ID，原 7 条历史逐项保持，新调用使用新 epoch；总记录达到 9 条 |
| 请求隔离 | 兄弟请求的模型调用不进入当前共享历史，逻辑身份不同 |
| 容量与冻结配置 | 原 attempt / 字节限额在第二次预约前拦截；缺策略、不同 reasoning、无效容量拒绝 |

首次运行是 **10/11**，失败例为历史 Worker 工具不能授权管理者。生产门禁已正确拒绝，
测试误期待 `Unauthorized`，原模型响应校验的错误类型实际为 `InvalidInput`。
仅修正测试预期，没有改变生产权限路径。首轮日志和最终日志均保留：

- [原生首轮 10/11](single-agent-baseline-native-initial.txt)
- [原生最终 11/11，7.88 秒](single-agent-baseline-native-final.txt)
- [Release 配置](single-agent-baseline-configure.txt)、[首次构建](single-agent-baseline-build.txt)、[修正后重编](single-agent-baseline-rebuild.txt)

此前严格编译还发现测试中 Boost.JSON value 与 `std::string` 直接比较及 range-loop
复制触发 `-Werror`；修正为明确字符串读取与 `const auto&`，没有放宽编译选项。
普通 UID 不能遍历原工作区的私有父目录，因此使用上述独立 `/tmp` 源副本进行验证，
没有更改原工作区权限。

### 后续 ASan 门禁观察与有界分组

根集成首轮 ASan 专项为 **6/8 通过，整体 521.81 秒**；其中原单个 `single_agent`
套件在 **90.07 秒超时**，是失败记录。该轮 stdout 由原测试入口缓冲，终止前没有
阶段输出，因此不能凭空认定具体哪个方法挂起。源码中包含 9 次完整七调用场景的
独立重建；Release 通过不能替代这次 ASan 门禁。

随后仅修改本测试 binary 的 `main`：`--group history` 选择原 6 个方法，
`--group runtime` 选择其余 5 个方法；无参数仍按原顺序运行全部 11 个方法。
未知组、缺值、额外参数或其他旗标立即返回 2。全部原函数和断言保持，只有该入口
启用 `std::unitbuf` 留下阶段证据，共用测试 harness 没有修改。
CMake 分别登记 `single_agent` 与 `single_agent_runtime`，**各保持 90 秒**，
Make 明确运行两组。分组后测试源码 SHA256 为
`67f26be8cbf620bafda531ed77dadfe3e8e2cf3ff8f4242101992743abd7936c`。
此前专项证据 JSON 保留其当时真实输入，不回写成新入口的验证结果。

最终输入的实际 ASan 分组结果为：

| CTest 套件 | 参数 | 原方法数 | 实际结果 | 实耗 / 原限额 |
| --- | --- | ---: | --- | --- |
| `single_agent` | `--group history` | 6 | **6/6 通过** | **60.73 / 90 秒** |
| `single_agent_runtime` | `--group runtime` | 5 | **5/5 通过** | **60.30 / 90 秒** |

两组实际运行的 binary 均为 `single_agent_tests`，表中的组参数由各 CTest 注册项
传入。原 11 方法没有被重复相加。最终全树 Release 两种外层 umask 也分别为
**78/78**：`0022` 用时 **165.92 秒**，`0002` 用时 **170.44 秒**。
这些后续结果的最终输入清单、机器记录及完整输出统一见
[两模块最终集成报告](billing-baseline-validation.md)，不借用较早专项输入替代。

同一 ASan 轮次的联合 E2E 还观察到两个 single 条件触及原 **60 秒 batch deadline**；
这与原生套件的 90 秒总限额是不同门禁。独立验证调整固定脚本的每批最多 6 次调用、
最多 8 批，保留原 60 秒 batch、150 秒 CLI helper 和 900 秒套件限额。调整后的
独立 Release 为 14/14，通过更多真实 CLI 恢复验证原来源；其最终 ASan 联合结论见
上述集成报告。本节确认本 native 两组的实际结果，原两项候选失败仍保留，不能由
重编成功或已完成的局部测试推定其他门禁通过。

## v3 评测与两次真实恢复

最终 40 个方法包含原有 31 项 v1/v2 回归和 9 项 v3 方法。v3 的固定计划、原逻辑
输入及每次原反馈独立重建均通过实际 CLI 数据验证；不能只添加共同 ID 就把旧
多角色数据库叫作单逻辑 Agent。公平条件只移除 `context.logical_agent`，修改其余
context 配置仍不可比。已重算逻辑 hashes 的删消息与重置全局步号仍然拒绝。

新增恢复正例对 multi/single 两个条件各实际启动三个 CLI 进程：第一进程产生
plan/order，第二进程恢复并写文件，第三进程再次恢复并提交、检查、接受。精确交付
与收据随后完成。原工单到提交间的两个原 recover 作为 v3 来源见证保留。

Store 的 `rebound_instructions.previous_*` 指向不可变原工单 epoch，始终不是上次
投影。v3 按实际记录核完整恢复区间：每段原 body/hash 与原权限及语义 binding 相符，
记录中的当前 epoch 连续递增，最终精确等于原提交；不杜撰数据库没有的中间 binding。
错中间 body hash、错中间 execution epoch、删除中间恢复事件均不能通过。

同一数据按 v2 重评仍按原完整 binding 相等判据失败，不把 v3 恢复解释静默应用到
旧版本。v1/v2 的 `single_agent` 仍表示单一 Worker principal/team。

CLI 负例还在 Takeover 之前拒绝 single fake 的非空宿主 AMEND dispositions，比较
所有原数据库表，确认没有新增恢复事件、epoch、预算或业务写入。

### 保留的失败和修正范围

| 轮次 | 原结果 | 根因与修正 |
| --- | --- | --- |
| 37 项首轮 | 31 过、6 失败 | 新 fixture 把 system 配置移到临时目录，却未指定原 functions 目录；仅补明确资源参数，失败发生在 submit 前期 |
| 38 项复跑 | 34 过、3 error、1 failure | 三个对抗 fixture 未增加原 `state_version`，SQL CAS 先拦截，现按原规则递增后继续检验实际消息关系；另一个失败使用了保留旧 mtime 的旧 CLI 对象，快照断言没有放宽，集成方实际重编并提供新 SHA 后通过 |
| 40 项首次扩展 | 38 方法过，2 方法中有 3 个失败子例 | Fake role_call_index 每个 CLI 重置，初版续跑脚本又返回初始 plan/order，未到验收；改成明确分进程的连续固定脚本，不改变生产计数或来源判据 |
| 恢复专项 | 2/2，2.309 秒 | 两条件两次恢复，以及损坏/缺失中间恢复原证据 |
| 最终完整评测 | **40/40，12.331 秒** | wrapper 实耗 12.410 秒；固定新 CLI、实际源码 hashes 和原运行身份均已记录 |

联合测试另暴露 evaluator 误从 `requests` 直接读取 `config_snapshot_sha256`；原
Store 410 将该列存于 `request_runtime`。v3 现从原 runtime 读取并核对冻结 snapshot
字节，未改数据库结构或原冻结配置。跨重启来源另由上述显式 v3 契约补齐，未放松
v1/v2 的原来源门禁。

原始输出分别为：[37 项首轮](single-agent-baseline-evaluator-initial.txt)、
[38 项旧 CLI 观察](single-agent-baseline-evaluator-stale-cli.txt)、
[恢复续跑 fixture 失败](single-agent-baseline-evaluator-continuation-fixture.txt)、
[恢复专项](single-agent-baseline-recovery-focused.txt)、
[最终 40 项](single-agent-baseline-evaluator-final.txt)。

## 独立本地 TLS 联合结果

[独立集成报告](billing-baseline-independent-validation.md) 记录最终 **14/14 方法通过**
（最终第3轮43.683秒，进程43.892秒），固定五任务乘两个条件的 **10 个样本位全部 passed**。每个样本实际
跨 CLI 恢复；独立测试逐项比较真实 wire 中的历史 function call 名称/arguments、
function call output 与原 Store 响应/反馈，未为评测构造共同 ID 或补造历史。

| 固定任务 | multi 原 attempts | single 原 attempts | 工程结论 |
| --- | ---: | ---: | --- |
| 工具往返 | 9 | 9 | 原读取、写入、回读及模型提交来源相符 |
| 两任务部分 AMEND | 19 | 19 | 一个任务 update、另一个 keep，原模型决定与当前接受相符 |
| 否定验收后返工 | 13 | 13 | 原拒绝、模型纠正及新接受相符 |
| 返工耗尽 | 25 | 25 | 原额度耗尽后模型提出当前 owner question |
| 交付与精确收据 | 7 | 7 | 原接受字节、交付和收据版本相符 |

上述十样本共有 **146 次原调用**，均为本地 TLS 固定 peer。独立报告保留首轮
100000-token 合成 profile 窗口不足的失败；第二轮在新计划中为两条件共同显式设为
262144，并保留原业务预算、512KiB 历史容量和所有原历史。生产容量门禁没有改动，
不能把首轮容量拒绝解释成全局历史已满，亦不能将合成额度解释为供应商能力事实。

## 验证边界

本报告的模型响应为固定 Fake，费用为合成输入；其来源链可以说明工程关系，不能
声称提供了真实供应商判断或独立账单真实性。五任务乘两个条件的真实 CLI / 本地 TLS
联合实验与本模块专项是两个独立范围，详细原始 trace 与首次失败见上列独立报告。
未知费用、未完成轮次、缺反馈或超容量均保持原失败/未知含义，不能裁掉历史后计为成功。
