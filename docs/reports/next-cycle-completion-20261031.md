# A0–A5 本轮收尾与验证结果（2026-10-31）

本报告补充[首次执行记录](next-cycle-validation-20261031.md)，保留它和所有历史失败。
本轮实现 A1 日志/目录守卫、A2 离线验收包、A3 单协调写者、A4 公平及双预留、A5 原生基础切片。
**本机历史 sanitizer 问题未闭合；Windows 11 普通用户、真实供应商验收、正式 PR 审核与合并未完成。**
完整原始日志留在数据盘及 Actions artifact；随本报告新增[原始文本证据包](next-cycle-evidence-20261031.tar.gz)、[机器索引](next-cycle-evidence-index-20261031.json)和[实际测试输入清单](next-cycle-executed-inputs-20261031.sha256)。
证据包包含74个原文本文件（另有内部manifest），压缩SHA-256为 `4df6d94135f85cc6cc29f20d861257dc47f89d7feb249b895680b87e8a4b1eca`；各成员原字节哈希均已回读核对。
最终文档发布提交及其 CI 由 PR #10 和发布后收据记录，不把旧提交的运行冒称为新提交的运行。

## 环境、基线与分支

普通用户 ubuntu，UID/GID 1000；Ubuntu 24.04.1，Linux 6.8.0-147-generic，x86_64，4 vCPU、约7.8GiB RAM、无swap。
`/mnt/d` 实际挂载 `/dev/vdb2`、ext4，与系统盘分离。所有 checkout、build、私有 TMPDIR、SQLite fixture、证据均在：

```text
/mnt/d/codex-workspace/wls861-next-20261031-065247/
```

GCC `/usr/bin/g++-13` 13.3.0，Clang 18.1.3，CMake/CTest 3.28.3，Python 3.12.3；
测试 venv 保留 `/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python`。
没有替换系统编译器、安装本机全局依赖、修改安全策略、终止其他会话进程或操作生产数据。
运行版本仍为 `0.11.1-r3.3c-audit`、Store410；v0.5.0仍是规划版本。

实际起点为 PR #4 `772feaee62086ddb0e7f605a7472721d3bc61406`。
本轮核验的 main 保持 `a6b8559018a599d7a806b05942857a56d69a00ce`，PR #1–#4 未改 base、未合并。

| 模块 | 最终模块完整 SHA | 分支 / PR / 直接依赖 |
| --- | --- | --- |
| A1 | `1c279f6770f5f4440adcf6b8ee2f51e2401d9a74` | `fix/sanitizer-followup-20261031` / [#6](https://github.com/wmqfl861/wls861/pull/6) / #4 |
| A2 | `e8a861f32e29e191645f788d20eee8e5ad0e8533` | `feat/offline-acceptance-20261031` / [#7](https://github.com/wmqfl861/wls861/pull/7) / #6 |
| A3 | `e78791ca1420204b6205301ec9f22a2e1595fa96` | `feat/coordinator-async-20261031` / [#8](https://github.com/wmqfl861/wls861/pull/8) / #6 |
| A4 | `86b82f8f63a3c60c7659c9cd8f9b8e7d7252870b` | `feat/fair-scheduling-20261031` / [#9](https://github.com/wmqfl861/wls861/pull/9) / #8 |
| A5 | `21f5308271505706594f1a03941ec6ab4a804d50` | `feat/windows-native-foundation-20261031` / [#5](https://github.com/wmqfl861/wls861/pull/5) / #4 |

集成分支 `work/next-cycle-integration-20261031` / [Draft PR #10](https://github.com/wmqfl861/wls861/pull/10)
直接依赖 #9，正常 merge #7、#5。实际完整产品测试输入为
`12d0f94db4645ca187d47e94a472d952b7489d04`，Git tree为
`79a8f3240791360c9699e6be35f01ceec9ec9941`：773个提交文件，27,582,257bytes。
[测试输入SHA-256清单](next-cycle-executed-inputs-20261031.sha256)固定这些原字节；后续报告新增文件不混入该次运行身份。
Release最终输入增量构建 exit0，与此前完整构建的61项构建/二进制身份逐项相同；61项包含CMake注册/配置文件，不能称为61个可执行程序。
`wls` SHA-256为 `226f19c9c2fe32f5c2eee50a09af1402a1ea45b1a381f9864c6f3a20eb51cd40`。

## 实际新增能力和边界

| 模块 | 实际实现 / 关键源码 | 测试及未实现范围 |
| --- | --- | --- |
| A1 | `tests/immediate_unittest.py`立即flush原traceback；共享`run_gcc_asan.py`核对预审名称/期限目录 | 中止后失败原因保留：旧工具3/4、候选4/4；runner20→23→集成24自检。都是工具自检，不计入产品92项；没有修复或关闭历史Clang原因 |
| A2 | `scripts/r3_offline_acceptance.py`及`offline_acceptance/v1/`复用原R3 v3五任务×multi/single；prepare/dry-run/run-fake/report/真实配置模板校验 | 两个产品CTest：合同9方法、CLI10方法。干跑通过确定性拦截证明无凭据解析/发送/持久准入；真实质量、实际费用及语义评审NOT_RUN |
| A3 | `async_coordinator.hpp/.cpp`、`OfflineRuntime::reserve_captured_call`、事务内`Store::claim_offline_call` | 原A3一个suite/12case；当前A4后同suite/18case。实际S1 plan/order/ack/note，两个屏障模型并行、第三请求提交；显式C++调用级API，无auto inbox、live异步或文件/进程执行器 |
| A4 | 原M1 `Scheduler::submit_fair`、请求/团队轮转、跨池同资源FIFO、两类常驻审核预留及有限诊断 | 管理4/Worker8、预留1/1、普通3/7；有限选择次数验证公平，满载控制/结果、异常容量、真实ask与UNKNOWN预算。审核类别容量不是独立业务审核运行器 |
| A5 | `platform/native-foundation/`直接编译真实config/Scheduler，版本化公共资源、Unicode路径、类型化Handle RAII、能力报告 | 四个原生CTest及资源缺失/篡改/链接拒绝、源码路径缺席后的重定位。Store/VFS、业务文件/进程、沙箱和cleanup明确拒绝；不是完整wls.exe移植 |

A3等待发生在Store事务与协调锁外；只有宿主线程写所拥有的Store，原数据库生命周期锁保留。
Actor/session/request/task/version/epoch、预算、原准入和唯一claim在派发与提交时仍受原Store守卫。
旧结果不能换当前Actor补权；精确重放不再次派发，证据重试不重复业务效果。
取消不证明效果撤销，逻辑等待释放槽位不清除UNKNOWN或账目；原同步行为和冻结配置不变。
设计契约见ADR-092/093及各实现文档，后续B0–B6只维护[任务卡及接口依赖](../spec/v0.5.0/10-authorized-cycle-and-followups.md)。

A2持久包实际四步均exit0，39条原生CLI命令全部exit0，十槽均COMPLETED且原评测10/10。
原118条尝试和118条反馈、合成input/output 11800/2360及fake cost0与账本一致，live send/unobserved均0。
原包325文件、34,006,031bytes、11个Store保持原字节；五个single槽均为一个逻辑执行者的完整共享历史。
这些是离线结构与工程证据；不证明供应商质量、实际费用、多Agent优势或真人语义验收。
材料、模型、预算、工具权限、返工上限和验收条件继续冻结；真实执行须外部配置用户中转、精确模型/协议、可信计费、凭据引用、预算及调用授权。
模板没有可发送默认官方端点或真实key，也不会自动取得发送权。真实供应商/付费调用均为0。

## 实际验证结果

下表按执行环境及精确输入登记，重复Push/PR和suite内case不计作额外独立套件。

| 输入 / 环境 | passed / failed / timeout / skipped / notrun | 退出状态和证据 |
| --- | --- | --- |
| 原PR4，本机GCC13.3 Debug-g ASan/LSan，完整89 | **48 / 2 / 39 / 0 / 0** | CTest8、runner1，7482.340725s；714源码/60构建身份不变 |
| 历史Clang18 Debug-O2-g，原39项 | **29 / 0 / 10 / 0 / 0** | CTest8，2949.68765s；原失败不改写 |
| 原Clang二进制完整workflow诊断 | 18方法：5ok、1ERROR、1已启动未结束、11未启动 | 原300s watchdog124、wrapper1；捕获cross-task原240s超时，model_exhaustion未到达 |
| 12d，本机完整Release，umask0022 | **92 / 0 / 0 / 0 / 0** | CTest/外层exit0，2366.396155s；完整日志/目录/身份及无遗留进程已核验 |
| 12d，本机完整Release，umask0002 | **92 / 0 / 0 / 0 / 0** | CTest/外层exit0，2330.494831s；与0022同源码及构建身份，无遗留进程 |
| 12d，本机严格Clang/UBSan/ASan最终矩阵 | **NOT_RUN** | 相应精确输入配置实际在下列CI执行；不冒称本机通过，不替代原本机GCC失败 |
| 12d，Linux Push/PR 两种Release、Clang18、UBSan | 每个CTest job **92 / 0 / 0 / 0 / 0** | 两组各四配置已核验；runner24及即时诊断4自检另计 |
| 12d，Linux Push/PR共享GCC ASan/LSan | 每轮 **92 / 0 / 0 / 0 / 0** | CTest0，2387.279/2434.375s，UID1001、umask0002、detect_leaks=1，773源码/61构建身份稳定 |
| 12d，Linux Push/PR Make-ASan | 两轮完整目标success | 每轮57条原生+33条Python测试命令、另2条源码核验；不是92个CTest |
| b8集成，本机Linux原生组件 | **4 / 0 / 0 / 0 / 0** | 18条记录命令全部0；不是12d本机原生重跑或Windows证据 |
| 12d，Windows原生Push/PR | 每轮 **4 / 0 / 0 / 0 / 0** | 每轮10条记录命令全部0；Windows Server2022/elevated范围见下文 |

第一轮Release的完整LastTest.log独立确认合同9、CLI10、async18、live_workflow18原始结果；
这四项JUnit成功输出被CTest的1024bytes阈值截断，所以不能单凭JUnit文本声称逐方法完整。
源文件、全部92实际命令/原期限、JUnit、原stdout与LastTest逐一核验；旧测试PID/session/group均不存在。
第二轮也完整保存LastTest、JUnit、实际目录及进程收尾；其结果独立记录，不由第一轮或同名CI推断。

A1原两项非超时失败为https_transport与live_model_store_tls；超时套件里还包含原FAIL/ERROR，不能简化为“只有超时”。
历史model_exhaustion的缺失traceback不能补造，新诊断也没有运行到该方法；状态仍为 **UNRESOLVED / NOT_REPRODUCED**。
本机/远端比较确认原89名称/期限/规范化命令、90编译命令和175项生产输入字节一致，但工具链环境、文件模式、内核/存储和跨机二进制不同。
CPU steal、资源压力和磁盘观察仅为条件记录，没有证据把Clang、-O2或磁盘单独定为根因。
`historical_clang_asan_failures_closed=false`保持；没有关闭LSan、放宽期限/断言、重复重跑求绿或缩短历史。

A4同源反例不是编译失败：同一CPP `48ae8f4db3d29835403bc41daba2554d51a2c05d5a7c3901ee901193fd25aad6`
分别连接旧A3 `a2dfb46322d217f32eb5cc2516de6107c10db4b8`和候选自己的头/库/Store资产。
模块86及集成12d的Push/PR均真实得到旧版原第193行顺序断言exit1、新版唯一PASS/exit0，原5s屏障/180s watchdog不变。
这是新增公平能力的行为缺口，不称旧安全漏洞；产品90/92目录只是诊断附带信息，探针不替代完整产品执行。
原A3本机对照工作区仅CONFIGURED/NOT_RUN，实际红绿来自CI。

保留的其他失败包括A2 unittest字段覆盖run方法、交付元数据/虚拟路径夹具断言错误，以及A4初始CMake PATH预检失败。
修复只针对已复现的夹具/工具问题；原日志保留，没有削弱Store守卫或历史断言。
证据解析器也保留空WAL/成功JUnit截断等错误假设的首版失败记录；这些不计作产品测试失败，也不作为成功产品反例。

## 远端身份、Windows与工程复核

集成Linux：[Push41193660570](https://github.com/wmqfl861/wls861/actions/runs/41193660570) /
[PR41193665523](https://github.com/wmqfl861/wls861/actions/runs/41193665523)。
同源公平：[Push41193660582](https://github.com/wmqfl861/wls861/actions/runs/41193660582) /
[PR41193665647](https://github.com/wmqfl861/wls861/actions/runs/41193665647)。
原生：[Push41193660590](https://github.com/wmqfl861/wls861/actions/runs/41193660590) /
[PR41193665487](https://github.com/wmqfl861/wls861/actions/runs/41193665487)。
PR实际checkout为 `489143c029f1a423d3cace9b65853fc7dfb032ab`，与12d同tree，不能混称branch head。
[源码快照41193660568](https://github.com/wmqfl861/wls861/actions/runs/41193660568) artifact13039858318逐项匹配773文件，
tar SHA-256为 `c95de97c3d628f81071cc969e9f7a0852619a4494d296678f8dc142a9566ee30`。
A1–A5各自最终模块的两组六路CI已成功并核验，完整run、checkout和artifact身份见机器索引；不使用旧head结果代填新head。

Windows实际为 **Server2022 build20348 / AMD64 / NTFS / elevated token**；
VS17.14.37824.6、MSVC19.44.35229.0、toolset14.44.35207、SDK10.0.26100.0、CMake3.31.6。
35项公共资源160549bytes、Unicode、篡改/缺失拒绝、原源码路径缺席后定位均有原生运行证据。
**Windows11 x64普通用户NTFS仍NOT_RUN**。完整Store/VFS、文件/ACL/SID/Reparse安全、进程树/退出证据、
AppContainer/受限令牌/网络隔离、owned cleanup仍未实现或未验收；Job Objects不是完整沙箱。
Windows sanitizer未执行；Linux ASan/LSan与UBSan门禁保留。
Linux临时文件任务与强执行环境的正向bwrap/cgroup验收也分开；本批不将已有拒绝测试或doctor诊断变成B0实机安全通过。

A3/A4原始最低纵向要求的非作者收据绑定12d六个源码文件和18项核查：真实Store→M1→锁外模型→原captured结果提交，
双池满载审核、有限轮数公平、资源FIFO、同池模型回调子调用拒绝、真实ask释放槽位但保留UNKNOWN及兄弟推进。
审核者为`/root/independent_recovery_review`，属于同模型工程复核，不是第二模型或主协调Agent正式批准。
原初业务Agent级作者在可用记录中未确认，使用原commit/Git作者记录；root的Store协作、evidence_audit两追加case、
concurrency_fix诊断工具修改等已确认角色分别披露。知识图只覆盖旧PR4，新增源码的判断依据为精确提交直接读取及哈希。

## 复核入口与交付状态

已有入口不执行真实模型：

```sh
python3 -B tests/test_run_gcc_asan.py
python3 -B tests/test_immediate_unittest.py
python3 -B scripts/r3_offline_acceptance.py --help
python3 -B scripts/run_a4_fairness_red_green.py --help
```

完整Release实际命令为`ctest --test-dir combined/build-integrated-release --output-on-failure --parallel 1 --no-tests=error --output-junit <本轮独立证据路径>`，
在普通用户、分别0022/0002及各自数据盘私有TMPDIR下运行。准确argv、起止时间、出口和SHA见两个completion receipt。
原基线共享GCC命令在固定`integration/`执行：`python3 scripts/run_gcc_asan.py --data-root /mnt/d --compiler /usr/bin/g++-13 --python /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python --expected-tests 89`。
候选集成采用经过审阅的92名称/期限目录；不能在候选上沿用89或仅读取当前数量自动接受缺项。
离线四步和原生MSVC复现命令分别见[离线契约](../implementation/R3-offline-acceptance-package.md)与[原生契约](../implementation/windows-native-foundation.md)。

| 模块 | IMPLEMENTATION_COMPLETE | LOCAL_VALIDATION_COMPLETE | PUSH_COMPLETE | REMOTE_CI_COMPLETE | PR_REVIEW_COMPLETE | MERGED |
| --- | --- | --- | --- | --- | --- | --- |
| A1 | TRUE，仅诊断/目录工具；根因未关闭 | FALSE | TRUE | TRUE，精确模块head | FALSE | FALSE |
| A2 | TRUE，离线限定包 | FALSE，最终本机完整矩阵未关闭；十槽已通过 | TRUE | TRUE，精确模块head | FALSE | FALSE |
| A3 | TRUE，显式调用级基础 | FALSE，历史运行时及最终本机门禁限制 | TRUE | TRUE，精确模块head | FALSE | FALSE |
| A4 | TRUE，限定调度容量 | FALSE，同A3 | TRUE | TRUE，精确模块head | FALSE | FALSE |
| A5 | TRUE，原生组件 | FALSE，Windows11普通用户未运行 | TRUE | TRUE，精确模块head/Server范围 | FALSE | FALSE |
| 集成12d | TRUE，以上限定代码 | FALSE，见分别结果 | TRUE | TRUE，精确12d四类workflow已核验 | FALSE | FALSE |

两种本机Release及12d远端矩阵已经完整结束；上述FALSE保留各自历史、本机配置、平台或批准边界。
本报告的新增文档提交不授予正式接纳或合并权。
发布前需再次核验原SQL、third_party、handoff、v0.4.1及基线已存在212份reports没有修改，原始日志不重写，数据库/构建产物/凭据不入Git。
主协调者下一步需要独立裁决A1根因定位和A3/A4实验实现的正式采纳，安排Windows11普通用户实机条件，
并按B0–B6卡片派发下一批。真实模型/付费授权、生产部署、主分支合并均不在本轮执行范围。
