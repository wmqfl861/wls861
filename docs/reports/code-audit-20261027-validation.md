# 0.11.1-r3.3c-audit 全代码审核修复验证

审核编号：20261027；原报告按 America/Los_Angeles 记日，执行日志使用 UTC。
问题与修复见[审核报告](../reviews/code-audit-20261027.md)，设计取舍见
[ADR-090](../adr/090-comprehensive-code-audit.md)。蓝图仍为 v0.4.1，Store 仍为 410。

本轮已复现并修复十一类边界问题，并处理正式 ASan 验证暴露的完整历史重复处理开销。
优化后的原 case04 在不改变期限、断言和权限的条件下通过，runner 墙钟为 **164.486 秒**。
最终两种 umask 的完整 Release 和 18 组 focused ASan 结果只以第 2 节正式记录为准；
旧候选的通过、失败和插桩诊断分别保留，不合并为最终版本的通过声明。
本轮没有执行真实供应商或付费调用，也没有建立真实供应商质量验收结论。

## 1. 原基线、覆盖范围与最终输入

审核基线为 `d3da700c9c47db86abe941374bcbdac5e31462d4`。
原实现由该提交的 `git archive` 源码独立构建；原失败没有借用修复后的生产对象或库。
早期专项绿灯中存在“修复对象加原基线库”的组合链接，相关 metadata 如实登记，
不能把这些专项链接称为最终全仓构建。

最终[输入清单](code-audit-20261027-inputs.sha256)共有 **307 项**，SHA-256 为：

`ce3d518aa1ecf50d2d7e84c7c28e949e39e5bd7df559147c8b10219d69a0dbad`

它覆盖源码、公开与内部头、构建/CI、脚本、测试和运行资产；相较原 306 项增加一个
C++ 原始 JSON 数字夹具。最终冻结时间为 `2026-10-28T03:11:36.469731+00:00`。
初始审核候选同为 307 项，但清单 SHA-256 为
`025d3290451a2c4776c854a99c2a9c6aedd128c164eaf14aec331780707af9de`，两者不可混用。
最终相较初始候选变更 `CMakeLists.txt`、四个生产实现及两个测试文件，具体路径见
[机器记录](code-audit-20261027-results.json)。

源码审核覆盖 49 个实现/内嵌实现/内部头（37 个 `.cpp`、11 个 `.inc`、1 个内部头）、
34 个公开头、5 个工程脚本、11 个数据库参考/SQL 文件，并检查构建、安装及 CI。
84 个 C++/Python 测试与支持源文件进入验证输入；该计数不含 JSON 等数据夹具。
执行全部套件不等于对全部源码、测试或第三方依赖完成形式化证明。

受保护旧文件逐项核对原 Git blob、SHA-256、文件类型和可执行模式：

| 受保护范围 | 基线文件数 | 总字节 | 已记录核对结果 |
| --- | ---: | ---: | --- |
| `db`，含 README、原 SQL 与参考实现 | 12 | 131063 | 与基线字节和模式一致 |
| `third_party` | 31 | 399041 | 与基线字节和模式一致 |
| 原有 `docs/handoff` | 83 | 773396 | 与基线字节和模式一致 |
| 原有 `docs/reports` | 192 | 4857947 | 与基线字节和模式一致 |

本次新增报告不计入“原有 reports”数量；没有改写已发布 SQL、第三方源码或历史证据。
核对清单及时间保留于证据归档，不能用新增报告数量改变历史范围。

## 2. 最终普通用户正式门禁

构建和测试使用 UID/GID **65534**、空附加组，保留 C++20、严格警告及 `-Werror`。
最终 Release 在源码内 `build-release` 布局执行，完整 CTest 使用 `--parallel 2`，
分别记录调用 umask `0022` 和 `0002`；focused ASan 按原注册顺序执行。
下表登记最终输入清单对应的实际结果；初始候选与诊断运行的结果另列于第 4 节。

| 最终门禁 | 实际结果 | runner墙钟 | 原日志SHA-256 |
| --- | --- | ---: | --- |
| Release，调用umask0022 | 78/78，exit0 | 166.699秒 | `23efc0e9d36c6fc54fa5047be087485355e239e7092913c10ec2f960ce9aad46` |
| Release，调用umask0002 | 78/78，exit0 | 161.086秒 | `e3e785f79b581de4c4a6549636b2ea6c23f4520aed504c844cfb4d14a491a1b6` |
| ASan原case04，独占执行 | 原multi/single分支及断言通过，exit0 | 164.486秒 | `63575d7b3e82191100177379cd108be203cc19fa300bae3d810bf9fdf7f2c3cd` |
| ASan/LSan受影响完整套件 | 18/18，exit0 | 864.914秒 | `e942df03fa6fcca146c6c2774ee0bda02f84d0e35b1751822c1301ae1e77fe36` |

已完成的最终 ASan 增量重建耗时 **32.119 秒**，Release 增量重建 **28.062 秒**，均 exit 0。
两次均复用此前构建目录，**不是最终源码的清洁构建耗时**。
原 case04 的正式复验使用无诊断插桩的最终 ASan CLI，exit 0、164.486 秒；
它覆盖原多 Agent 和单 Agent 两分支，仅为完整计费 E2E 中的一个注册方法。
该次日志 SHA-256 为 `63575d7b3e82191100177379cd108be203cc19fa300bae3d810bf9fdf7f2c3cd`。

最终 runner 在每次构建/测试前后核对全部 307 项，测试另核对 CLI 二进制身份；
新 runner 即使测试非零退出也先做末尾核验，再登记失败。
归档前另检查最终完整日志，未发现 AddressSanitizer 或 LeakSanitizer 诊断。

调用 umask 不等于夹具内部的有效 umask：共享原生测试和计费私有夹具按既有规则使用
`0077`，doctor CLI 保留调用 mask，部分 sandbox 夹具另测子进程 `0002`。
一般套件 90 秒、`cleanup_runtime` 180 秒、`billing_baseline_e2e` 900 秒等原期限保持；
计费场景原 60000 ms batch deadline、150 秒 CLI 子进程期限及断言均未放宽。

## 3. 最终 focused ASan 的逐项范围

本地最终执行范围为以下 **18 个不同的已注册 CTest 套件**；逐项结果单独记载。
实际逐套件状态和时间见第 2 节及[机器记录](code-audit-20261027-results.json)。
本地 focused ASan 不代表本地全部 78 组 ASan；重复运行也不增加独立套件数。

| 序号 | CTest 套件 | 覆盖重点 |
| ---: | --- | --- |
| 1 | `store` | Store 生命周期、锁、事务及状态守卫 |
| 2 | `live_model_store` | 账本快照、核账容量、live 配置准入 |
| 3 | `process_evidence` | 文件类型、真实读入预算和原进程证据 |
| 4 | `cleanup` | owner marker、清单与完整 EOF |
| 5 | `cleanup_runtime` | 取消证据、真实交付后的 owned 清理 |
| 6 | `worker_tools` | 工具权限、取消状态与幂等结果 |
| 7 | `schema` | 父路径、无符号链接及有界读取 |
| 8 | `config_loader` | 冻结配置、历史配置恢复及约束 |
| 9 | `offline_runtime` | 捕获请求、上下文与 token 预留 |
| 10 | `https_transport` | loopback HTTPS、HTTP 原字节与传输限制 |
| 11 | `live_model_store_tls` | 原发送、结算与 TLS Store 集成 |
| 12 | `r3_evaluation` | 42 项评测方法、原事实及历史兼容 |
| 13 | `billing_baseline_e2e` | 完整 16 方法计费/成对基线集成 |
| 14 | `single_agent` | 完整历史、prefix 原字节、深度和字节限额 |
| 15 | `single_agent_runtime` | 单 Agent 的实际运行、权限与历史连接 |
| 16 | `model` | 请求/响应信封与公开入口校验 |
| 17 | `r1_structured` | 深恢复、原生消息配对及重新哈希篡改 |
| 18 | `protocol_codec` | 请求编码、响应解码与协议边界 |

新增注册测试方法累计为 **16 个 C++、4 个 Python**，均加入既有套件；CTest 总数仍为 **78**。
性能阶段新增的 4 个 C++ 方法为一个结构化请求深校验回归和三个历史 prefix 回归。
子例、两个 mask 重跑及重复 sanitizer 执行不再计作新增独立方法。

## 4. 保留的初始验证结果与失败

下表属于初始候选或诊断阶段，完整原日志保留；它们不替代最终输入的门禁。
时间统一为各 runner 的墙钟秒数，避免与 Python 方法内部或单批次计时混淆。

| 阶段 | 实际结果 | runner 秒数 | 解释 |
| --- | --- | ---: | --- |
| 初始 `release-0022` | 76/78，exit 8 | 380.060 | 两个 CLI 路径夹具因验证目录布局失败 |
| 布局修正后的旧 `release-0022-final` | 78/78，exit 0 | 187.600 | 初始候选输入，尚无性能修复 |
| 布局修正后的旧 `release-0002-final` | 78/78，exit 0 | 188.015 | 初始候选输入，尚无性能修复 |
| 旧 `focused-asan-core` | 9/9，exit 0 | 146.688 | 当时 303 项原生输入核验 |
| 旧 `focused-asan-integration` | **3/4，exit 8** | **895.485** | 计费 E2E 的 case04 与派生 case99 断言失败 |
| 旧 `asan-billing-isolated` | **0/1，exit 8** | **698.622** | 无其他编译/测试时，仍为相同 case04 与 case99 失败 |
| 顺序对照 `baseline-case04` | 原 pristine 通过，exit 0 | **250.678** | 原提交 ASan CLI，两分支均执行 |
| 顺序对照 `audit-case04` | 初始候选失败，exit 1 | **244.089** | 原 60 秒单 Agent batch deadline 触发 |
| 临时插桩 `profile-case04` | 诊断运行通过，exit 0 | **223.401** | 插桩/重编译/重链接结果，仅用于定位 |
| 最终 `optimized-case04` | 正式专项通过，exit 0 | **164.486** | 最终清单、无诊断插桩、原期限与断言 |

首次 Release 将 build 和 source 放在同一级，触发已有的可执行文件相邻源码发现及
默认资源错误文本夹具，失败套件为 `doctor_diagnostics_cli` 和 `review_cli_smoke`。
修正为源码内构建目录后，重新完整构建耗时 275.053 秒；未为消除这两项失败修改产品行为、
测试断言、期限或权限。该次布局修正的完整构建也不能冒称为后来性能补丁的清洁构建。

初轮 ASan 集成与编译/Release 运行重叠，但之后安静环境下完整 E2E 仍失败，
因此不能将问题归因于并发负载并宣称隔离复验已通过。
case04 单 Agent 未完成预期耗尽重试终态，case99 因而正确拒绝不完整的成对证据；
后者是前者造成的格缺失，不能删掉该断言或把失败格排除后宣称 10/10。

同机顺序对照使用相同原 case04 测试源，分别运行 pristine 和初始候选 ASan CLI。
初始候选失败导致提前结束，不能用 244.089 与 250.678 秒计算“候选更快”。
这些运行没有构成多次随机顺序、跨机器的性能基准，不据此给出普遍加速比例。

## 5. 诊断依据与保留验证边界的优化

临时诊断按 `pump_workflow` 调用栈区分 batch 内外，原记录同时保存 wall/CPU 和 inclusive/self。
一个 6-call 单 Agent batch 为 32.882 秒，其中 canonical JSON 6443 次、累计处理 166612960 字节；
相应 canonical 累计 21.737 秒，完整 structured context 恢复 198 次、15.793 秒。
最慢单 Agent batch 墙钟 54.312 秒、CPU 54.115 秒；structured context 恢复 25.472 秒、
transcript 8.213 秒，其中 marker 6.375 秒；该批包含两个 LEFT JOIN 的整条容量查询累计约 0.007 秒。
这些是**嵌套 inclusive 时间，不可相加**；batch 时间也不等于整个 CLI 或 case04 的时间。

诊断说明本次主要成本在反复处理完整 JSON；插桩本身、重新编译和链接会改变代码布局，
诊断通过不能推翻前面的正式失败，更不能算作正式门禁通过。
最终只去掉同一次局部计算中的重复工作：

- 同一 `model_request_from_json` 的 structured context 完整恢复由三次降为一次；保留原字段、完整深恢复与完整回序列化相等性检查。
- 同次 context 编译复用 prompt 原字节用于长度和哈希，同次捕获复用 prompt 长度；前缀超限仍先短路。
- 单次 transcript 逐条以外层数组完成 canonical 检查，复用原记录字节构建 prefix；完整浮点、转义、深度和字节限额保持。

公开可变请求的 `to_json`/`validate_response` 仍完整校验；Store 当前权限、历史、预约、
send claim 与实际发送仍各自检查。没有跨调用或跨事务缓存，也没有删除完整历史或原鉴权。
prefix 每步的 SHA-256 仍扫描完整前缀，不将该局部优化描述为整个算法完全线性。
新增回归覆盖重新计算哈希的非法信封、恢复后再次篡改、独立完整数组 oracle、
Unicode/转义/小数、126/127 层边界、非法 UTF-8/非有限数和 4096/+1 字节边界。

## 6. 基线反例、专项绿灯与兼容性

以下专项证据在最终整仓门禁之前获得，明确保留其原链接方式和输入阶段。
各条原日志、argv、身份和已保留产物指纹见[证据索引](code-audit-20261027-evidence.json)。

| 问题/套件 | pristine 原实现实际结果 | 修复后专项结果 |
| --- | --- | --- |
| A01 Store 继承写锁 | 31 过、1 败 | 32/32 |
| A02/A03/A08 live Store 快照、容量和准入 | 首轮 15 过、3 败；只修 Store 时 18 过、1 败 | 联合 19/19；另有 capacity 8/8 |
| A04 进程 FIFO 打开/真实读取预算 | 8 过、2 败 | 10/10 |
| A05 cleanup 完整 EOF | 23 过、2 败 | 25/25 |
| A06 启动前取消/真实交付后清理 | worker 17 过、1 败；cleanup_runtime 20 过、1 败 | 18/18、21/21 |
| A09 Schema 父目录符号链接 | 12 过、1 败 | 13/13 |
| A07 TLS 来源重标为 provider | 一个方法内 6 个子例被错误接受 | v1/v2/v3 均拒绝；纳入 16 方法 E2E |
| A10 修改原输入并重算局部哈希 | 一个方法内 3 个子例被错误接受 | v1/v2/v3 均拒绝；纳入 16 方法 E2E |
| A11 合法完整历史含失败小数响应 | pristine 实际完成交付，评测仍误报反馈指纹损坏 | 完整历史有效；纳入 42 方法评测 |

live Store 首轮后另加入 408/410 Fake 兼容守卫，最终 19 例并非原 18 例原样重跑。
IO 相邻路径 allocation_recovery 30/30、owned_capacity 11/11、sandbox 19/19 通过，
与该域四个直接修改套件合计 134 例；最终整仓门禁另行覆盖这些路径。

评测专项为 **42/42，16.168 秒**，loopback/Fake 联合 E2E 为 **16/16，53.083 秒**。
这是性能修改前的专项结果；最终版本对应结果须从第 2 节及正式日志读取。
专项成对报告 10/10 格通过，但 `provider_quality_acceptance_established=false`。
本地签名/传输/来源一致性证据不等于真实供应商质量或费用对照验收。

A11 使用真实单 Agent/Fake 保存含额外 `0.1` 参数、被 Schema 拒绝的原响应，
之后继续正常流程并完成 publish/ack；pristine 仍报告
`single-Agent immutable feedback fingerprint mismatch`，不是人为伪造的历史哈希反例。
修复保留原数字 token 以重建 C++ 事实，贯通输入、反馈、marker/link、消息及相关原事实哈希；
原 Python plan/report 身份规则保持，嵌套同名字段、字面字符串和数值篡改仍被检出。

真实 Store 408/Fake/framed-v1 无 live 表、无新 logical 字段时，v1/v2 仍为有效证据，
nullable cache 保留；未完成任务正确报告 failed，而非 invalid_evidence。
同一真实未使用 seed 由 pristine/修复评测器生成的 v1/v2/v3 预提交计划，
**三个版本的完整字节和 SHA-256 均一致，并可相互 validate**。

## 7. 输入身份、缺失历史与记录规则

初始 9 组原生 ASan 阶段核对 303 项；其后冻结核对三个 Python 文件，实际同步其中发生变化的
评测器、评测测试及新增数字夹具，计费 E2E 测试字节未变化。早期原生测试源未含新增四个
性能方法，该阶段只支撑当时原生路径的局部结果。
旧完整门禁的 307 项清单与最终清单不同，所有历史结果保留原指纹，不改标成最终输入。

归档的 `initial-audit/source/` 只含相较最终版本变化的 7 个旧文件；其余 300 项由旧清单与
最终仓库相同字节对应，不将该目录描述为完整的 307 文件旧源码包。旧记录中的执行路径随后
复用于优化构建，不能用路径当前内容判定旧身份；历史身份以捕获的旧清单、旧二进制指纹
和这 7 个旧文件为准。最终 environment 则重新计算当前二进制指纹。

旧 runner 的失败阶段记录入场核验，但因非零退出未执行末尾核验；
后续源码或二进制检查是明确的后续时点证据，不能补写为失败当时的末尾核验。
最终 runner 已改为失败也先做末尾核验，前后时点单独存储。

未保留的精确编译 argv、执行时源 hash、umask 或起止时间按原专项 metadata 保持 `null`，
不用重建命令或当前文件指纹补作历史事实。已知包装命令和独立身份探针与逐次实测值分开说明。
早期 live 红灯测试源后来被 19 例版本替换，旧源 hash 未知；protocol 两个早期红测的
执行时测试源 hash 也未记录，不能用最终源码冒充。
IO 初始 metadata 早于增长读取回归，原实现最终 8 过/2 败以保留的最后红灯日志、
`fixed/results.json` 和 `audit-summary.json` 为准，历史记录差异保留。

## 8. 环境、命令与证据归档

实际环境为 GCC 13.3.0、ld 2.42、CMake 3.28.3、Ninja 1.11.1、Boost 1.83、
OpenSSL 3.0.13、libcurl 8.5.0、native SQLite 3.45.1、libyaml 0.2.5；
Python 3.12.14/jsonschema 4.10.3，Python sqlite 模块为 3.53.1。
各阶段实际 argv、cwd、环境、身份、调用 mask、时间、输入和二进制指纹见机器记录。
代表性 CTest 命令如下，使用普通用户身份；构建目录对应各自阶段，不能据此补造历史 argv：

```sh
ctest --test-dir build-release --output-on-failure --parallel 2
ctest --test-dir build-asan --output-on-failure -R '^(store|live_model_store|process_evidence|cleanup|cleanup_runtime|worker_tools|schema|config_loader|offline_runtime|https_transport|live_model_store_tls|r3_evaluation|billing_baseline_e2e|single_agent|single_agent_runtime|model|r1_structured|protocol_codec)$'
```

ASan 使用 Debug 和 `WLS_ENABLE_ASAN=ON`，`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`。
环境中设置 `UBSAN_OPTIONS` 不等于此次 ASan 构建也执行了独立 UBSan 门禁。

| 文件 | 内容 |
| --- | --- |
| [code-audit-20261027-inputs.sha256](code-audit-20261027-inputs.sha256) | 307 项最终运行输入 |
| [code-audit-20261027-results.json](code-audit-20261027-results.json) | 最终与历史门禁、环境、输入及二进制身份 |
| [code-audit-20261027-evidence.json](code-audit-20261027-evidence.json) | 原始证据成员、大小及 SHA-256 |
| [code-audit-20261027-evidence.tar.gz](code-audit-20261027-evidence.tar.gz) | 红/绿日志、metadata、明确保留的源码及采集脚本 |

归档含 **217个成员、816852字节**，SHA-256为 **`738ac2fc0f62f1599eabfa8a61b7c149ec581b3a040868bc9eb6e04ecf5fd25c`**。逐成员读回与原始字节比对一致；原日志没有去空白或重排。

归档范围限明确保留的文本，不纳入构建二进制、对象/静态库、临时数据库、核心转储或私钥；
诊断源码和插桩补丁标明仅用于定位。原日志不作去空白、重排或内容归一化。

## 9. 发布与验收边界

最终报告和证据文件不改变上述 307 项运行输入；提交后仍需确认**精确最终 SHA**的
strict、strict-umask-0002、Clang-18、UBSan、ASan、Make-ASan 六路 CI 及 source snapshot。
旧基线和中间提交绿灯均不能替代最终 SHA。远端结果待实际发布后确认，
本发布前记录不预填未来 Actions 成功；交付回复应提供最终 SHA 及实际运行链接。

本轮为同一模型体系分工、交叉复核与回归的工程审核，不替代第二模型业务审核或形式化证明。
没有使用生产数据，没有执行真实供应商/真实费用质量对照、完整部署强隔离正向、掉电试验
或第三方 CVE 普查。完整本地回归和静态审核也不能证明不存在剩余缺陷。
已确认修复、兼容边界及未扩大的能力范围以[审核报告](../reviews/code-audit-20261027.md)为准。
