# S1.4 问答、提交与团队检查增量实际验证报告

日期：2026-10-19（UTC）。目标蓝图：**v0.4.1**；库/CLI：**0.5.0-s1.4**；原 M1 入口保持 **0.1.1-m1**。开发基线：`8a9328d90175b63161582fa62fa1a3843d149cc5`。

本批沿 S1-03/07/08/09/10/12 接入 Worker 提问、逐级回答和转发、真实文件提交快照、TeamLead 内部检查及有界返工。代码、迁移、模型投影 Schema、蓝图、路线图、任务卡、演示和本报告共同交付。通过团队检查只产生 `CANDIDATE_READY`；独立审核、业务验收、交付包、回执、经验和可恢复清理继续按计划实施。

**最终稳定源码 Make 严格门禁、UBSan 门禁均退出0：S1 C++ 202/202、实际S1 CLI 38/38、原M1内核/CLI 55/55、Python参考契约36/36全部通过；独立CMake Release / CTest 25/25组通过。** 新增Store 21项、运行器14项和实际CLI 12项，不将不同构建模式重复计为产品验收。

## 1. 本批实际接入

| 模块 | 当前行为 | 对应实现 |
| --- | --- | --- |
| SQLite 405 | 追加007事务迁移、资产指纹、问题/转换/提交/检查的原事实和独立CAS；旧接口与只读兼容 | [Store](../../src/store.cpp)、[007迁移](../../db/migrations/007_workflow_runtime.sql) |
| 原问题与回答 | Worker发起；TeamLead/Dispatcher回答或逐级附注转发；Root只答已上报user的问题；原文、选项和绑定不可变 | [Store回归](../../tests/workflow_store_tests.cpp)、[运行器回归](../../tests/workflow_runtime_tests.cpp) |
| 精确等待与控制 | 回答只结束绑定的原等待；新政策下必要控制可继续，旧捕获拒绝；普通wait/wake和旧run-fake不能旁路 | [EventRuntime](../../src/event_runtime.cpp)、[CLI回归](../../tests/s14_cli_smoke.py) |
| 提交原始字节 | 受控完整fs_read、路径/大小/SHA/base64校验、8文件/单64KiB/总256KiB；文件后来改变不覆盖原证据 | [OfflineRuntime](../../src/offline_runtime.cpp)、[Store](../../src/store.cpp) |
| 团队检查返工 | 单目标捕获、逐项自检/验证一致性、冻结返工计数、新任务/说明修订和Worker新ack；通过仅候选就绪 | [工作流契约](../implementation/S1-workflow-runtime.md) |
| CLI与有界事件推进 | questions/submissions只读，answer要求对象版本；问答与返工可跨进程恢复；错误、缺脚本和上限保留待办 | [CLI](../../src/cli.cpp)、[固定脚本](../../tests/fixtures/s14_fake_rework.json) |

运行器仍是同步、有界的离线宿主入口。一次 pump 默认最多12次Fake调用、每任务最多1次Worker调用；显式最大值为64与16，管理调用仍受冻结配置限制。405增加最多128轮宿主重访以处理本次成功动作新产生的待办，调用总上限仍有效，不在SQLite事务中等待模型或工具。

TeamLead 看到的 UTF-8 文件全文来自持久 base64 解码，二进制明确显示 `binary=true/content=null`；原始字节和哈希继续保留。完整调用上下文仍限1MiB，文件视图或累计原历史超限时在预约前拒绝，不能截短证据或静默丢弃约束。

## 2. 测试分层与完整门禁

### S1 C++ 与实际 CLI

| 检查组 | 用例数 | 关键范围 |
| --- | ---: | --- |
| Runtime types | 4 | 严格JSON、SHA、typed ID及完整binding |
| 原Store | 30 | 402/403、事务、身份/CAS、操作/预算、目录与不可变执行事实 |
| 原Event Store | 15 | 404、部分消费/新到项、领取恢复、独立计划、AMEND、等待、原反馈 |
| 新Workflow Store | 21 | 405迁移/回滚、原问题和路由、等待/政策恢复、快照、检查/返工、CAS/幂等/容量 |
| Config loader | 15 | 严格来源、冻结依赖、继承、恢复与未支持能力拒绝 |
| Model / Context | 15 | 原调用绑定/用量、完整输入帧、稳定前缀和依赖 |
| Schema | 12 | 完整记录与模型投影分开校验；当前注册16个Schema，原完整示例仍13个 |
| 原Offline runtime | 9 | 原单任务note、AMEND、迟到响应、预算、取消、恢复 |
| 原Event runtime | 14 | 真实双任务、遗漏/后补AMEND、wait、错误/额度、原反馈、跨运行历史 |
| 新Workflow runtime | 14 | ask权限、逐级问题、单目标捕获、防伪与旧响应、真实/二进制快照、完整返工、重启/AMEND/预算 |
| Workspace | 19 | 真实文件、SHA/CAS/patch、路径/链接/容量、并发和故障 |
| Sandbox | 17 | 强模式拒绝、doctor、私有进程监督；不含强隔离正向成绩 |
| WorkerTools | 17 | 真实文件/Store/Fake、权限与控制、唯一派发及结算失败围栏 |
| **S1 C++合计** | **202** | 既有167项加本批21+14项 |
| 原S1.1 CLI | 8 | 实际C++ CLI配置/Schema、note、AMEND、查询、取消 |
| 原S1.2 CLI | 8 | 实际文件链、强run拒绝、只读诊断、目录身份、失败停批 |
| 原S1.3 CLI | 10 | 双任务文件/历史、逐项AMEND、wait/wake、预检和上限 |
| 新S1.4 CLI | 12 | 跨进程问答/返工、原文件快照、policy控制、迟到/防伪/路径/只读预检 |
| **S1实际CLI合计** | **38** | Python启动真实C++ CLI，独立于下列Python参考实现 |

原S1.2/S1.3 CLI仅把新建数据库预期版本404更新为405，其余行为继续回归。旧402/403/404的Store和运行器接口分别保留原测试入口。

### 原内核、参考契约和来源

| 检查组 | 数量 | 证据范围 |
| --- | ---: | --- |
| M1 Journal / Config / Scheduler / TaskEngine | 10+5+9+28 = **52** | 原内核状态、并发、持久化、合成审核和回执 |
| M1实际CLI | **3** | 原合成生命周期与输入/错误行为 |
| Python Schema / SQLite参考 | **36** | 13项Schema/语义参考，23项SQLite事务参考；Python本身无UBSan插桩 |
| v0.3原始交接包 | **33文件** | 固定原件清单；原51项产品验收仍未执行 |
| v0.4原始审核包 | **49文件** | 固定SHA-256清单 |
| 固定上游原件 | **29文件** | 已逐项比对实际字节数和Git blob SHA-1 |
| 原schema / 004 / 005 / 006 | **字节未变** | 已与本批开发基线逐字节比较，新能力只追加007 |

### 稳定源码完整门禁

| 门禁 | 实际状态 |
| --- | --- |
| Make严格完整门禁 | 退出0；S1 C++ 202/202、实际S1 CLI 38/38、原M1 55/55、参考契约36/36及原件完整性通过 |
| Make UBSan完整门禁 | 退出0；同样的完整分层回归全部通过，无UBSan错误 |
| 独立CMake Release / CTest | 退出0；25/25组通过，实际测试总时长72.15秒 |

CTest的25组由17个C++组、5个CLI组、1个Python参考契约组和2个原件完整性组构成。组内用例、重复构建与不同入口不合并成产品验收通过数；整个S1和原51项产品验收不因此完成。

## 3. 关键路径与边界的实际观察

**原问题逐级上报。** `s14_fake_question_route.json` 在一次pump中产生7次Fake调用：一次Dispatcher计划、alpha/beta各一次TeamLead说明、两个Worker各一次动作、alpha TeamLead转发、Dispatcher转发。alpha等待答案时beta仍创建实际文件。CLI查询核对问题原文、上下文、原选项、原binding和两级独立附注；Root回答只处理 `holder=user` 的OPEN对象，并使用查询得到的state_version。

**跨进程恢复和输入拒绝。** 在独立CLI进程中问、转发、答和继续执行，读取实际SQLite事件、调用帧、任务及问题记录。跨请求、错误版本、未知选项、未上报用户、空白或超长答案在打开写者前拒绝，不能增加epoch、预算、事件或任务状态。普通wake和旧run-fake不能解除未答问题。问题在新说明后成为SUPERSEDED时，原answer、ANSWERED转换和binding仍完整保留。

**政策变化的必要控制。** 只撤销不相关工具后，原问题仍由持当前policy的新调用回答；旧policy捕获的回答拒绝。提交正面检查要求原生产政策仍匹配，当前policy的新负面检查允许明确退回；Worker新ack、新快照提交后才再次检查。已就绪候选的负面撤销也要求本次当前身份和捕获CAS，不把旧响应改盖为新授权。

**真实文件的不可变证据。** Worker写入临时工作目录后，宿主通过既有fs_read捕获完整字节。回归独立解码base64、重算SHA、核对字节数和路径，随后修改实际文件，再次读取提交及管理上下文仍得到原字节。二进制、路径缺失、符号链接、路径穿越、截断页、哈希/大小不匹配和容量超限均有对应检查；失败不生成半份提交或成功状态。

**完整8次调用返工链。** `s14_fake_rework.json` 包含Dispatcher计划、TeamLead说明、Worker写第一版、Worker提交、TeamLead失败检查、新ack后的Worker写第二版、Worker新提交和TeamLead通过检查。实际文件成为第二版，第一份提交保留第一版快照并标RETURNED_FOR_REWORK；第二份标CANDIDATE_READY。task_revision与instruction_revision各增加，requirements_revision/plan_revision不因内部返工增加，Worker必须有新ack记录。独立CLI用例另验证中途重开数据库的同样链条。

**一致性和冻结上限。** 未覆盖当前验收项、重复/外来ID、否定自检、缺证据、失败验证和已知问题不能正面放行。允许负面自检先提交进入返工；检查失败要求合法WorkOrder并保留原目标/标准/登记约束。默认冻结上限3轮按team_id持久计数，重启不清零，耗尽则BLOCKED并留下Dispatcher待办。

**原子状态和容量。** SQLite故障注入核对问题/等待、提交、检查/返工、事件一起回滚；并发同一目标仅一份CAS成功，原命令精确重放不重复增计数。普通SUBMITTED检查受4096普通事件上限约束；只有当前CANDIDATE_READY的宿主负面撤销可使用8192有界控制额度。该测试没有模拟物理磁盘耗尽或整机掉电。

**捕获事件和缺失步骤。** 管理调用输入固定单一问题/提交对象，模型伪造ID、CAS或自我批准被拒绝；迟到响应不能选择另一份当前目标。缺脚本、错误、调用/预算上限保留具体待办，Dispatcher候选后续处理项不被现有计划自动吞掉。恢复事件只能为它捕获的同task主题重新选择目标。

## 4. 交叉审查与修正

- 发现仅允许note的旧Worker预约前置检查会阻断ask-only配置；新投影按当前实际授权声明ask/submit，带Workspace与不带Workspace两条ask-only路径均有回归。
- 管理检查上下文增加从原base64导出的UTF-8视图，二进制明确标识；解码不重新读取活文件，也不改变原哈希。
- 模型预约完成后若宿主发现目标不属于原claim，在调用FakeModel之前保存原binding的ERROR响应、明确零Fake用量和 `model_invoked=false` 反馈，避免留下未实际调用的未结算预留；恢复授权收紧为同subject_task_id。该预约后窗口来自代码审查修复，本批未通过竞态注入专门触发；Store双线程同目标CAS回归是另一项证据。
- Store与CLI交叉审查定位政策变化造成的blocking问题死锁，补上当前政策的回答/转发及负面返工恢复；旧捕获响应仍严格拒绝。该问题用实际CLI复现后加入回归。
- 保存to_json的局部对象再遍历字段，避免C++20临时子对象生命周期风险；UTF-8视图使用显式Json构造，通过严格编译。
- CLI答案按最多16000字节读取，再由Schema校验4000个Unicode字符，避免把中文答案错误限制为4000字节；README使用真实node到目录字符串映射，单独演示问题上报和8调用返工。
- 新测试的路径拒绝错误类型按实际受控文件契约核对；恢复后问题状态断言为SUPERSEDED并保留原答案，不把它错误写成新说明的授权。

Store、运行器和CLI分别进行交叉审查；最终发布证据取稳定源码的完整门禁。没有降低警告、关闭断言、修改原迁移或启用弱生产执行入口。

## 5. 环境、复现与原始证据

本次环境为Linux x86_64、内核6.18.55、glibc2.39、GCC13.3.0和CMake3.28.3。实际系统依赖：Boost1.83.0、C++ SQLite3.45.1、OpenSSL3.0.13、LibYAML0.2.5；Python3.12.14、jsonschema4.10.3。Python sqlite3模块为3.53.1，与C++链接版本分开记录。

使用项目现有依赖和虚拟环境，各验证使用独立构建目录。实际命令在仓库根目录执行：

```sh
make -j2 test BUILD_DIR=/tmp/wls-s14-make \
  PYTHON=.local/test-venv/bin/python
make -j2 ubsan BUILD_DIR=/tmp/wls-s14-validation \
  PYTHON=.local/test-venv/bin/python

cmake -S . -B /tmp/wls-s14-cmake -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/absolute/repo/.local/test-venv/bin/python
cmake --build /tmp/wls-s14-cmake --parallel 2
ctest --test-dir /tmp/wls-s14-cmake --output-on-failure
```

Make强制 `-std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`。UBSan增加 `-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`，并设置 `halt_on_error=1:print_stacktrace=1`。CMake独立编译全部源文件并通过CTest运行相同分层组。

证据文件：

- [Make严格完整门禁](s1.4-make-test.txt)
- [UBSan完整门禁](s1.4-ubsan-test.txt)
- [CMake配置、构建及CTest](s1.4-ctest.txt)
- [活动源码、配置、契约与文档SHA-256](s1.4-source-SHA256SUMS.txt)

日志将当前仓库绝对路径归一化为 `<repo>`，保留独立 `/tmp/wls-s14-*` 构建目录，去除行尾空白。Make日志是最终稳定源码的增量重建及全套回归；UBSan和CMake使用新建独立目录完成构建与完整测试。活动文件清单覆盖165个文件，不含报告/日志及自身，也不混入历史交接档案或固定vendor原件；后两类按各自原清单单独核验。在仓库根目录运行 `sha256sum -c docs/reports/s1.4-source-SHA256SUMS.txt` 可核对本批完整活动文件。

本报告记录推送前本地结果；GitHub上同一提交的strict与UBSan Actions将在推送后独立核验。历史报告和历史源码清单继续保持其原提交含义。

## 6. 保留边界与下一增量

问答目前由Worker主动发起，上层处理该原问题；没有扩展为任意角色发起所有问题。TeamLead内部检查依赖固定Fake脚本，结构和证据一致性不能证明任意业务的语义质量。

本轮提交是SQLite内的原始文件证据，不是S1-11交付包或原子发布目录。文件逐个完整读取，不声称同时冻结整个活目录；文件代理的合作锁不防任意同UID宿主绕过。CANDIDATE_READY的负面撤销当前是Store宿主API能力，运行器尚未新增自动复查/负面证据唤醒或S2正式审核循环。

405只升级已知、完整可验证的404资产。已有旧问题行而缺原binding时拒绝编造历史；缺冻结快照的非空401、物理掉电和跨存储恢复继续需要专门方案。

没有真实供应商调用、费用、服务部署或用户数据清理。强bwrap/cgroup正向、后代逃逸、历史残留cgroup恢复、真实input挂载、后台run与完整SBX/SEC验收仍需各自环境和独立证据。

下一增量沿S1-10/11/12接当前版本业务验收、不可变交付包、回执、经验与可恢复收尾，再推进wlsd/API。跨功能独立审核、DAG及全局审核双容量按S2推进。原51项产品验收和整个S1保持各自未完成范围。
