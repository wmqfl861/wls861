# G2 首次真实运行

2026-11-17 更新：**U1、U2 均通过**，真实交付、唯一确认回执和成果 12/12 核对完成。U1 为 19 次调用，U2 为 32 次调用及 3 任务/3 团队，均无自动重试。原三交付物请求的导出 UNKNOWN 已在 R0 关闭；旧失败和原 null 用量保持。R11 优化与 R12 `v0.13.1` 尚未完成，当前实现仍为 0.13.0；最新结果见[末节](#u1u2真实闭环完成)，以下按日期保留旧记录。

日期：2026-11-15 至 11-16。**U1/U2 未通过**：R2/R3 末次 Dispatcher 收到中转上游失败流；一次新 R5 则在末次 Worker 的无序号扩展事件处被协议拒绝，均无完整交付。原三交付物导出已按 D-31 恢复、成果 12/12 且无新调用；严格 metadata 保留检查仅 SHM mtime 变化而 FAIL，原结果保留。E1 离线字节减少已验证，真实改善仍未知，按 D-37 保守回退（[#134](https://github.com/wmqfl861/wls861/pull/134)，当前门禁/合入状态见末节），不宣称优化无效或导致失败。R6 按“明确记录未达原因”的计划路径整理 v0.5 与六格式；版本不代表 U1/U2 成功。R5 固定 main `3fa7019`（[#133](https://github.com/wmqfl861/wls861/pull/133)），原输入、CI 与回退身份见末节；Store 410。

11-15/11-16 运行配置：owner 已授权指定 Responses 中转、`gpt-6.1-sol` 与测试凭据，并取消金额门禁；D-35 增加其他思考等级授权，R2 仍全部用 `max`，R3 在探针确认后分层。新请求冻结 `observe-only-v1`；D-34 的 G2 余下额度为 250 次调用、500 万输入 token，11-15 的 41 次另列历史。单次期限 180 秒，自动重试为 0；R2 上限 60 次/150 万输入/25 分钟，R3 上限 80 次/200 万输入/40 分钟，两者输出上限均为 8 万。费用未知不等于免费，无需补价格表或重新提供密钥。

| 实际请求 | 结果与核验范围 | 调用（D/TL/W）；输入/输出 token；时长 |
| --- | --- | --- |
| 独立协议探针 | HTTP 200 / completed；模型 `gpt-6.1-sol`；返回 reasoning 与 function_call，工具执行 0，未经过 Store，不计闭环 | 1；255/172（reasoning 133）；8.45 秒 |
| WLS 冒烟 01 | Dispatcher 首次请求 HTTP 400：`Unsupported parameter: background`；`needs_accounting`；工具 0、成果 0 | 1（1/0/0）；未知/未知；5.52 秒 |
| WLS 冒烟 02 | `DELIVERED/quiescent`；固定 `write-hello/hello.md` 三行内容检查 PASS；1 个操作 SUCCEEDED、1 个任务 ACCEPTED | 9（2/3/4）；73,001/3,299；167.12 秒 |
| G2-4 三任务 01 | Dispatcher 首次请求达 60 秒期限，无 HTTP 响应；`needs_accounting`；任务、操作、成果均为 0 | 1（1/0/0）；未知/未知；61.12 秒 |
| G2-4 三任务 02 | Store 为 `DELIVERED`，但 `output_blocked`、`output_path=null`；Root `unknown_outcome`，成果发布未确认 | 29（2/3/24）；575,141/18,431；790.51 秒 |
| 11-16 R0：同一三任务 02 导出重放 | `DELIVERED/completed_replay`；原 `output/` 已导出，成果 12/12 PASS；严格保留检查 FAIL 仅 SHM mtime 变化，详情见下 | 新调用 0；原 29（2/3/24）与原用量不变；2.525 秒 |
| 11-16 R1：流式、等级、缓存探针 | 8/8 HTTP 200；流式和 low/medium/high 可用；两组第二次缓存均 0，按 D-36 不实施 E-4 | 8；26,204/54；单次 3.22–5.52 秒 |
| 11-16 R2 首次启动 | `RECEIVED/error`；`live pricing differs from the request's frozen role provider`，尚未派发或发送模型请求 | 0（0/0/0）；无新用量；0.865 秒 |
| 11-16 R2-fixed | `RECEIVED/needs_accounting`；完整 HTTP 200 SSE 被协议拒绝，0 任务/工具，无输出 | 1（1/0/0）；用量未知；96.326 秒；输入预留 34,266 保守占用 |
| 11-16 R2-stream | `RECEIVED/needs_accounting`；完整 HTTP 200 SSE 的遥测事件省略 model 被拒，0 任务/工具，无输出 | 1（1/0/0）；用量未知；36.097 秒；输入预留 34,268 保守占用 |
| 11-16 R2-stream-check | `RUNNING/needs_accounting`；第 17 次调用的 Dispatcher 响应不可用，未取得当前业务验收，无交付路径 | 17（2/2/13）；已知 187,388/8,832；1 Dispatcher 用量未知，输入预留 127,277；400.015 秒 |
| 11-16 R3-stream-check | `RUNNING/needs_accounting`；3 任务/3 团队已创建，末次 Dispatcher 遇上游失败，无当前验收或交付 | 21（2/4/15）；已知 136,479/4,619；1 Dispatcher 用量未知，输入预留 87,747；325.431 秒 |
| 11-16 R5-write-summary | `RUNNING/needs_accounting`；3 任务/3 团队；末次 Worker 无序号扩展事件被拒，`output_path=null`，成果核对未执行 | 10（1/3/6）；已知 57,642/7,894；1 Worker 用量未知，输入预留 51,440；230.839661 秒 |

按 Dispatcher / TeamLead / Worker 顺序，冒烟 02 输入/输出 token 为 14,622/1,006、30,323/1,356、28,056/937，平均请求体为 32,679.5、40,304、33,635.75 字节；三任务 02 为 31,194/4,887、76,857/7,098、467,090/6,446，平均请求体为 59,026、88,734、96,639.33 字节。缓存读分别为 0、7,680 token，缓存写均为 0；全请求按时间排序的 Worker 第 3..24 次缓存率为 7,680/446,197＝**1.7212%**（22 次调用，最小单次 0）。两次账目均 ready，总费用均 `null`，未知费用尝试分别为 9、29。冒烟保留 1 次 Worker `missing_leaf` 纠正，PASS 仅覆盖固定三行文件。

三任务 02 平均请求段字节（序列化 JSON value；other 含封装与分隔符，四舍五入）：

| 角色 | tools | system | contract | history | current_state | other |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Dispatcher | 13,174 | 6,319 | 2,179 | 0 | 37,006.50 | 347.50 |
| TeamLead | 11,141 | 5,912 | 19,026.67 | 0 | 52,303.33 | 351 |
| Worker | 10,775 | 6,111 | 16,066 | 57,995.25 | 4,977 | 715.08 |

三任务 01 的 TLS 连接及验证用时 0.401 秒，传输层报告上传 26,909 字节；HTTP 状态、响应头及响应体字节均为 0，用量和费用未知、账目未就绪，网络连接不能证明模型处理成功，超时根因未定。三任务 02 新建请求，只将实跑单次期限改为 120 秒，总墙钟 900 秒、40 次调用和原 token 上限不变，测试期限未改。所有失败原件保留，旧 attempt 未重发。

三任务 02 将三个需求合为 1 个 team、1 个 ACCEPTED task，5 次 Worker `fs_write` 均 SUCCEEDED。29/29 模型响应为 HTTP 200 / completed / usable；provider incomplete、输出到限、body/header 及工具 stdout/stderr 截断均为 0。当次 Root 报 `output intent publication incomplete`，首次 export 记录为 UNKNOWN。最终独立审计确认包内 13 文件的哈希/长度及 14 项数据库绑定全部通过；从不可变包复制出的私有投影运行固定检查 12/12 PASS，此次副本审计不证明原发布或用户收取成功。FAQ 两次写入 1,159→1,103 字节，第 3 答 78→22 汉字，其余 4 答不变，修订链绑定最终包；当次审计的原 DB/WAL/SHM 与包文件均未变。

Root 输出发布复用业务 Workspace，扫描兄弟数据库时受到 16 MiB 文件上限限制；旧 ELF 加 17 MiB 合成 fixture 的单方法回归已实测 RED。[修复 PR #111](https://github.com/wmqfl861/wls861/pull/111) 将发布检查与业务扫描分离，补齐收据与子目录重放的 fsync，保留副作用结果未知时的 UNKNOWN。独立最终静态审核 PASS；GCC 13.3 Release、普通用户、umask 0002 下 `workspace` / `delivery` / `run_cli_e2e` / `g2_outputs` **4/4 通过**，105.24 秒，0 失败/超时/跳过，run CLI 11 个方法通过，注册总数仍为 90；本机 LastTest/JUnit 完整保留。#111 已合并到 main `9b7f700`，[PR 三核心检查](https://github.com/wmqfl861/wls861/actions/runs/43737964662)及源码检查成功。#111 与 [#112](https://github.com/wmqfl861/wls861/pull/112) 合并后的 main `a83bdd8` [完整 CI](https://github.com/wmqfl861/wls861/actions/runs/43739309720)已通过：**450/450 CTest**，Make **60+25** 条，失败/跳过/未运行均为 0；JUnit/LastTest artifacts 不可用，保留 CI 原日志。本机合成通过不构成原真实请求复验。

端点适配已合并 [元数据兼容 #108](https://github.com/wmqfl861/wls861/pull/108)、[金额观察策略 #109](https://github.com/wmqfl861/wls861/pull/109) 和显式省略 `background` 的 [#110](https://github.com/wmqfl861/wls861/pull/110)；#110 本机专项 3/3 通过。实跑基线 main `cc65d9e` 的[完整 CI](https://github.com/wmqfl861/wls861/actions/runs/43724936562)九个构建/测试作业及[源码验证](https://github.com/wmqfl861/wls861/actions/runs/43724936571)通过：三核心各 90/90、ASan 与 UBSan 各 45+45、Make 60+25，无测试跳过；最大 ASan 套件占比 65.21/180 秒＝36.23%。JUnit/LastTest artifacts 为 UNAVAILABLE，CI 原日志留存。

[E-2 PR #113](https://github.com/wmqfl861/wls861/pull/113) 已合并到 main `900a879`。用途投影在本机对同一组 29 份捕获请求作离线编码测量（Dispatcher / TeamLead / Worker 各 2 / 3 / 24 份），按完整序列化 `current_state` 项计字节、包含外层封装：Dispatcher 均值 **37,006.50→18,336.50**（减少 **50.45%**），TeamLead **52,303.33→21,038.67**（减少 **59.78%**），Worker **4,977→2,436.375**（减少 **51.05%**），三个角色均达到状态项减半门禁。无 marker 的 a83 路径及仅有 `compact_model_state:true` 的 e81 路径各 **29/29** 旧 wire hash 全等，原样本未改。此项为本机离线测量；当时累计真实发送 41 次，未执行原发布重放或角色对照。

本机定向首轮 13 项为 **12 PASS / 1 夹具失败**（253.28 秒，0 超时/跳过/未运行），失败原件保留；原因为新增纯 codec 合成请求进入旧费用策略 Store 夹具时漏填正数费用预留。仅在该 Store 夹具补齐预留后，`live_model_store` 与 `g2_rehearsal` **2/2 PASS**（57.89 秒，0 失败/超时/跳过/未运行），产品 ELF 及四个产品源文件与首轮相同，本机 LastTest/JUnit 完整。D28 合成缓存固定序列在 Responses、Chat 均为 35 次调用、DELIVERED、3 个 Worker、11 个合格样本；冻结门槛分别为 `0.5633509583608723`、`0.5506825938566553`，确认运行均通过，原 G1 `>0.70` 门槛及固定序列/期限未改。CTest 注册总数仍为 90；首轮失败记录未改报为同轮 13/13 全绿。

最终 head `f773812` 的 [PR 三核心检查](https://github.com/wmqfl861/wls861/actions/runs/43752880351) **270/270 通过**，失败/跳过/未运行均为 0；[源码归档](https://github.com/wmqfl861/wls861/actions/runs/43752880296) **744 文件**与提交树逐字节一致。合并后的 main `900a879` 验证见[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43753899472)及[源码归档](https://github.com/wmqfl861/wls861/actions/runs/43753899551)。

随后 main `900a879` 的 [Make-ASan（1/2）](https://github.com/wmqfl861/wls861/actions/runs/43753899472/job/132933710502) 检出新增 Dispatcher 测试夹具在同一 JSON 对象插入时引用失效（`heap-use-after-free`）；修复为任何插入前分别复制 task/instruction 两个值，全部断言及原失败日志保留。 修复后本机 GCC 13.3 Debug 的 CMake ASan/LSan `r1_structured_lean` 完整注册项 **1/1 PASS**（13 个方法全部通过，84.23 秒，低于原 180 秒期限；`halt_on_error=1:detect_leaks=1`），LastTest/JUnit 已保留；此结果不代表 Make-ASan 全门禁通过。

本机旧轮次保留 89/90（专项 4/4，加其余 85/86），失败为 `doctor_diagnostics_cli` 布局检查；同一 ELF 在受支持布局下的 13 个方法通过，不将原轮次改报为全绿。已记录的凭据模式扫描为原 132 文件和新增审计 36 文件，均 0 匹配。长期状态统一见 [STATUS](../STATUS.md)。

本机私有证据根：`/mnt/d/codex-workspace/wls861-g2-20261115-a78u4b6i/`。原实跑在 `evidence/protocol-probe-01`、`evidence/real-*` 及对应运行目录；最终审计在 `evidence/real-three-02/package-audit-oid7cqxk`，修复验证在 `evidence/export-final-release`，基线 CI 在 `evidence/ci/maincc65d9e`。11-16 恢复线证据在 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/evidence/real-line/`。公开仓库只保留本页摘要和 CI/PR 链接，不提交凭据、数据库、长日志或原始模型响应。

11-16 R0 只读现场：`output-intent.json`、`output-root.json`、`output/` 及 `.wls-run-receipt-*` 均不存在；原数据库 30,060,544 字节，WAL 为空。完整备份 `r0-backup-v2ypjf_q` 含 23 个文件、6 个目录、0 个符号链接，共 30,561,124 字节；每个文件的 SHA-256、类型、mode 和 mtime 核对通过，备份阶段原请求与原配置前后不变。只读备份数据库确认 DELIVERED、唯一 CONFIRMED 交付和 29 次模型尝试。

R0 使用 main `8d44256` 的 GCC 13.3 Release ELF（SHA-256 `0a6a538349487f93e599fcd26a8cc40e983badda731525fa1c3ca17ec345eca2`），只替换原命令的程序路径，原配置及其余参数不变，未提供密钥。一次重放退出 0、耗时 2.525 秒；`stop_reason=completed_replay`、原请求 `output_path` 非空、调用仍 29，`check_g2_outputs.py … three` 12/12 PASS，D-31 四项导出标准满足，原导出 UNKNOWN 已恢复。准入、路径身份、回执、包和原字节守卫继续生效。

严格保留检查仍为 **FAIL**，不改写 `r0-replay-qiel64xz/result.json` 的 `pass=false`：原 `control.sqlite-shm` 只有 `mtime_ns` 从 `1794757036440118593` 变为 `1794830980908696960`，32,768 字节逐字节相同，SHA-256 `fd4c9fda9cd3f9ae7c962b0ddf37232294d55580e1aa165aa06129b8549389eb`、mode、dev/ino 均不变。原数据库、WAL、writer-lock、交付包与其他原文件无变化，原配置无变化，新增路径仅限两份导出回执及 `output/`。只读差异核对见 `r0-post-run-facts.json`；原请求、备份及该检查结果保留到 2026-12-16，不还原 mtime，也不再次重放。产品导出成功不等于所有保留检查通过。

11-16 R1 使用原中转和模型，自动重试 0。流式 max 探针与 low、medium、high 各一次；两组缓存请求每次报告 6,528 输入 token，第二次缓存读取均为 0，发送间隔分别 4.828、5.225 秒，满足 D-36 的实验条件。判定“不稳定或不支持”，不再为本端点开发缓存路由。R1 8 次用量全部已知，费用仍未知；不计作 Store 闭环。

11-16 R2 输入为 main `9aa9254`，新请求 `req_23F55W0D36B9NMPKRQZRR4WY3N`；`config check` 和三个角色 explain 均通过，实际全 max、流式、180 秒、60 次/150 万输入/8 万输出/25 分钟。产品冻结侧保留 Responses profile 原数组，运行侧的类型化序列化按集合排序；合法 `reasoning_efforts` 的不同顺序触发整份 provider 身份不等。错误发生在计费处理之前，与没有价格表无关。原请求、配置和失败日志保留；修复不得取消真实 profile 变化的拒绝守卫。副本只读核对 attempts、execution、send claims、transport observations 均为 0，G2 本轮累计保持 **8 次、26,204 输入、54 输出**。计入 11-15 历史后共 49 次；已知输入 674,601，加两次历史未知输入保守占用 60,948，合计额度占用 735,549；原实际总输入仍未知。

A-1：11-15 原三交付物请求的 24 次 Worker 调用。耗时来自原传输 `elapsed_us`；输出 token 包含推理 token。

| 序号 | 工具 | 输入 token | 输出 token | 其中推理 | 耗时（秒） |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | fs_list | 11,782 | 86 | 62 | 9.262 |
| 2 | fs_write | 9,111 | 262 | 49 | 10.907 |
| 3 | fs_write | 10,428 | 170 | 117 | 9.812 |
| 4 | fs_write | 11,524 | 41 | 0 | 9.417 |
| 5 | fs_write | 12,478 | 547 | 236 | 20.379 |
| 6 | fs_read | 14,191 | 29 | 0 | 8.600 |
| 7 | fs_write | 14,600 | 445 | 97 | 15.690 |
| 8 | fs_read | 16,339 | 67 | 36 | 15.727 |
| 9 | fs_read | 16,679 | 29 | 0 | 9.956 |
| 10 | fs_read | 16,828 | 29 | 0 | 9.446 |
| 11 | fs_read | 16,957 | 47 | 16 | 9.004 |
| 12 | fs_list | 17,376 | 34 | 10 | 6.792 |
| 13 | history_page | 17,509 | 178 | 148 | 12.189 |
| 14 | history_page | 19,464 | 38 | 8 | 16.524 |
| 15 | history_page | 21,265 | 28 | 0 | 10.340 |
| 16 | history_page | 23,079 | 546 | 516 | 21.097 |
| 17 | history_read | 24,187 | 551 | 516 | 29.512 |
| 18 | history_read | 25,200 | 551 | 516 | 22.639 |
| 19 | history_read | 26,062 | 47 | 12 | 17.479 |
| 20 | history_read | 26,398 | 33 | 0 | 14.098 |
| 21 | history_read | 26,989 | 147 | 113 | 11.630 |
| 22 | history_read | 28,613 | 33 | 0 | 21.793 |
| 23 | history_read | 29,201 | 49 | 15 | 9.730 |
| 24 | submit | 30,830 | 2,459 | 516 | 62.137 |

工具汇总：`fs_list` 2 次；`fs_write` 5 次；`fs_read` 5 次；`history_page` 4 次；`history_read` 7 次；`submit` 1 次。

A-2：最后一次 Worker 调用的历史段。复用产品离线编码器，重建 wire SHA-256 与原捕获值完全相同；按完整序列化 JSON 条目计字节。

| 类型 | 条目数 | 字节 | 历史占比 |
| --- | ---: | ---: | ---: |
| 推理项 | 16 | 59,324 | 48.36% |
| 工具调用参数及封装 | 23 | 8,917 | 7.27% |
| 工具结果及封装 | 23 | 54,432 | 44.37% |
| 其他 | 0 | 0 | 0.00% |
| 合计 | 62 | 122,673 | 100% |

A-1/A-2 的输入为原捕获请求和 R0 完整备份，离线分析新增模型调用 0。当前编码器重建最后一次 Worker wire 的 SHA-256 为 `d15bfbea1ce128d7b0c87373063cea9ea5262cf298e12a96218bc689d00adeed`，与原值完全相同，分析前后备份数据库哈希不变。数据支持评估 E-14、E-11、E-1；尚未实施或宣称真实改善，先完成 R2/R3。私有逐次探针在 `r1-probes/`，两表及身份核对在 `a1-a2-captured/`，R2 失败在 `r2-execution/`。

11-16 R2-fixed 使用 [#126](https://github.com/wmqfl861/wls861/pull/126) 合并后的冻结 main `bacf68bd`，所有上限、max、stream 和零重试保持。原 profile 规范化拒绝已通过原 ELF RED、候选两个 CLI 用例和 29 个 Store 方法验证修复，PR 核心及源码检查通过；原 R2 零发送失败保留。新请求 `req_1DT0DZRMJHJVZV67SSM1XM424Q` 首次 Dispatcher 传输为 HTTP 200、95.174 秒、响应体观察/保留均 439,149 字节、传输完成且无截断。模型或文件效果均未获准执行，原 attempt 为 ERROR，产品用量仍 null。

同一原始 SSE 的离线 `replay-model`（原 wire hash 匹配、0 网络/工具）在 `response.created` 拒绝：`SSE identity is empty, contains NUL or exceeds its bound`。原 response ID 为 477 字节，超出当前固定 256 的实现限制；后续事件兼容性仍在定向诊断。原响应完成事件中的用量可作为原始响应观察，不能覆写已冻结产品账目。证据为 `r2-fixed-execution/` 与 `r2-fixed-diagnosis/`；原请求不重发。

本轮真实累计 **9 次**，已知输入 **26,204**、已知输出 **54**，另 1 次用量未知，输入预留 **34,266** 保守计入阶段额度；阶段输入占用 **60,470** 不是真实总 token。计入 41 次历史后调用共 **50 次**，已知输入仍 **674,601**，未知预留合计 **95,214**，总输入保守占用 **769,815**。费用继续未知。

[#130](https://github.com/wmqfl861/wls861/pull/130) 增加显式冻结的响应 ID 上限，并按原策略处理已知 SSE 元数据。原 439,149 字节响应离线重放通过，1169 事件完整解析、1 个工具调用仅解析不执行；原真实账目不回填。新旧默认和拒绝守卫回归通过：本机 Release codec/SSE/CLI 3 项、GCC ASan/LSan codec/SSE 2 项（100 方法）；[PR 核心](https://github.com/wmqfl861/wls861/actions/runs/43909414171)、源码及 [D-40 六项](https://github.com/wmqfl861/wls861/actions/runs/43909409408) 全通过。

R2-stream 冻结在 main `322c173`，使用产品与资源等价的已验证 ELF。完整新 SSE 为 381,313 字节、1053 个连续事件，最后一个遥测事件没有 model，当前解码器报 `missing SSE field: model`。仅在该行补入 created.model 的派生诊断能完整解码，证明当前拒绝点，但不计作原输入通过；第二轮修复只允许该字段缺省，提供时继续强等值校验。原请求与 null 用量保留，不重发该 attempt。证据在 `r2-stream-execution/`、`r2-stream-diagnosis/`。

#131 合并后的 main `55bf304` 上，以新目录运行 R2-stream-check。复用通过两份原始 SSE Release/ASan 检查的 ELF（哈希及完整 source tree 与 #131 候选相同，合并 main 也为同一 tree）。请求 400.015 秒，17 次调用（Dispatcher/TeamLead/Worker 为 2/2/13），已知输入/输出 187,388/8,832；最后一个 Dispatcher attempt 为 `ERROR` 且输入、输出、缓存与费用均未知，输入保守预留 127,277。摘要 `RUNNING/needs_accounting`，一个 team/task 状态为 ACTIVE/SUBMITTED，缺少当前 Dispatcher acceptance，`output_path=null`，无 delivery。请求目录完整保留；不取消、不重发。当前新失败仍属同一受限 SSE 兼容工作线，在 #130/#131 两次修复后仍未完成，按 D-32 封存 U1，不做第三轮；下一计划项 R3 继续单独准备。证据 `r2-stream-check-execution/` 与只读诊断 `r2-stream-check-diagnosis/`。最后一份原流 57,385 字节（SHA-256 `a0faf4dd37b5697503f434b359ca05f7a1bda8b58cbf693974a4c4486f41a736`），事件序号 0..6 后以 seq=0 的 `response.failed` 报 `upstream_error`，其 response ID 也改变。原流有失败终态，解码器在序列守卫处拒绝，不能将 `terminal_received=false` 写成原流没有终态。

截至 R3-stream-check，G2 本轮累计 **48 次调用**；已知输入 **350,071**、已知输出 **13,505**，未知输入预留 **283,558**、未知输出预留 **16,384**；阶段输入保守占用 **633,629 / 5,000,000**。加上 11-15 历史共 **89 次调用**，through-v1 已知输入 **998,468**、未知输入预留 **344,506**，保守占用 **1,342,974 / 60,000,000**。历史与本轮 nullable usage 均未回填；实际总输入和费用仍未知。

[第二轮 SSE 修复 #131](https://github.com/wmqfl861/wls861/pull/131) 已合入 main `55bf304`。PR strict、strict-umask-0002、Clang-18、源码及 [D-40 ASan-affected 六项](https://github.com/wmqfl861/wls861/actions/runs/43916415645) 全部通过；main `322c173` [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43913004787) 也通过。main `55bf304` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43919634047)也已 10/10 作业成功：CTest 三核心各 90、ASan/UBSan 各 45+45，共 450/450；Make-ASan 两项作业通过。

R3-stream-check 与 R2 使用同一 main/ELF、仅按计划改用 `three-split.txt`、high/medium/low 和 R3 原上限。21 次调用（2/4/15）中角色实际型号均为 `gpt-6.1-sol`、等级分别为 high/medium/low；3 个任务、3 个团队，Worker 参数纠正 1 次。任务没有完整交付，成果检查未执行。末次 Dispatcher 原流 79,635 字节（SHA-256 `3a0ba9c370a63f71c5090b963e3c8b54949df07ca2147c32c692c20cbe7c10ae`），149 事件的 seq 从 0..147 跳回 0，`response.failed` 报同一 `upstream_error` 并更换 response ID；该调用的 usage 全部 null。原件和新目录不改，U2 写未通过、多团队写已观察到；不将请求配置或分团队创建当交付成功。证据 `r3-stream-check-execution/`、`r3-stream-check-diagnosis/`。

R4 后选择的 R5 最小切片是 E-1：仅对新配置显式 opt-in 的成功 fs_write 回执做发送副本投影；理由是 A-2 直接支持体积收益、无需修改历史记录，先避免组合三项无法归因。E-11/E-14 私有计划保留，本轮不为凑名额实施。合并前以同一 29 份捕获请求运行产品离线编码，合并后按原 R5 计划仅做一次新目录的同条件 R2；上游失败和不完整用量均单列，不以部分 token 差值宣称优化收益。

`upstream_error` 是中转原始流明确报告的错误，更深层的上游失败原因未观察到；报告不以此归因于某一供应商、主机或编译器。

## R5：离线验证、一次真实比较与处置

E1 原提交 `2bc26482b54865417635f4e9f294936c70c27f6c` 经 [#133](https://github.com/wmqfl861/wls861/pull/133) 合入 main `3fa70199af39600cf2c75e333e9ca97c4c4a9649`，两树相同。它只给独立 Worker profile 提供 `write-summary-v1`；原 profile/hash/wire、逻辑历史、Store 反馈及 `history_read` 原件保持。原 E1 本机 Release 与 ASan 协议各 60/60；[PR 三核心](https://github.com/wmqfl861/wls861/actions/runs/43933418378)270/270、[D-40 六项](https://github.com/wmqfl861/wls861/actions/runs/43933414439)和源码检查通过。

同一 29 份历史捕获按产品编码器离线比较，缺省 29/29 wire SHA 全等，管理角色 5 份不变；24 份 Worker 的 5 个不同成功写回执（4 创建、1 覆盖）累计投影 99 次。首次测试夹具 58/60、私有验证启动器和测量脚本的初始失败均保留，修正后结果不覆盖原失败。

| 离线样本 | 默认字节 | opt-in 字节 | 减少 |
| --- | ---: | ---: | ---: |
| 全部 29 请求 | 2,703,598 | 2,421,849 | 281,749（10.4213%） |
| Worker 24 请求 | 2,319,344 | 2,037,595 | 281,749（12.1478%） |
| 最后一次 Worker | 160,937 | 146,551 | 14,386（8.9389%） |

真实 R5 使用 `three.txt` 和原 R2 constraints，Root/D/TL/W 全 max、流式、单次 180 秒、自动重试 0；60 次/150 万输入/8 万输出/25 分钟。实际模型均 `gpt-6.1-sol`；D/TL 沿用 `g2-r2-stream-check-model`，Worker 使用独立 `g2-r5-write-summary-worker-model` / `g2-r5-write-summary-worker-20261116`，10 次 profile/effort 逐项核对。fresh Release ELF SHA-256 `de7bee490fc15ef365460d1e4d685055433f5880c12933575fadae5c456c1c46`，35 资源 manifest SHA-256 `29f43a7b40134751e8b7cdbf97eb236a0a475d69531e6295b92d4d9fc4254e4d`；运行源 main 与构建源完整输入匹配。

请求 230.839661 秒、10 调用后停于 `RUNNING/needs_accounting`。3 任务/3 团队仅证明建立路径；没有交付，`delivery_path/output_path=null`，成果核对未执行。最后 Worker HTTP 200，但 response 不可用，attempt ERROR，input/output/cache/cost 均保持 null，预留输入/输出 51,440/4,096。原数据库/WAL/SHM 哈希检查保持，分析仅在副本只读进行。

| 实跑与角色 | 调用 | 已知输入/输出 | 未知 input/output 调用 |
| --- | ---: | ---: | ---: |
| R2 Dispatcher / TeamLead / Worker | 2 / 2 / 13 | 4,801/1,942；21,104/2,278；161,483/4,612 | 1 / 0 / 0 |
| R3 Dispatcher / TeamLead / Worker | 2 / 4 / 15 | 4,820/1,121；29,716/1,791；101,943/1,707 | 1 / 0 / 0 |
| R5 Dispatcher / TeamLead / Worker | 1 / 3 / 6 | 4,796/1,940；22,204/4,284；30,642/1,670 | 0 / 0 / 1 |

R2/R3 实际 source 均为 main `55bf30459b8774e205023c20769c1b872894500b`，ELF SHA-256 `7634b85a5dcd4fbe50a7f5571602816b28738aa15dcb9a233c751afa3c8d04bd`；实际 model 均 `gpt-6.1-sol`，ref 分别为 `g2-r2-stream-check-model`、`g2-r3-stream-check-model`，effort 分别 max/max/max、high/medium/low。R2/R3 config 全文件 hash 分别绑定 preparation `db87ee301ac334eb6a2ac356f3dd1845f6c8766d08e34393a401b77efb3832ca`、`49cde095e26934c00e28aed378074f899aa0ad16b873c23690370bf266ad94fb`；R5 的同类绑定见下表。

R5 原末流为 92,130 字节（SHA-256 `13d82e94e77eb830d46ff51bee143d687a8eedca939b24cd091f0859394d74b3`）。同一 ELF 对原流离线重放在首个 `codex.response.metadata` 报 `invalid_sse: missing SSE field: sequence_number`；49 个事件中 47 个序号 0..46 连续，另两个扩展事件为无序号的 `codex.response.metadata` 与 `responsesapi.websocket_timing`，后部存在 `response.completed`。本次原流没有 `response.failed/upstream_error`，不能沿用 R2/R3 的失败归因；终态中观察到的内容不回填原产品账目。

六份实际 Worker wire 的编码哈希与原发送身份全等，发送历史中成功 `fs_write` 回执和 E1 summary output 均为 0。虽有两次写入成功，后续携带写回执的轮次尚未发送；本轮没有走到 E1 的实际压缩路径。既不能由部分 token、耗时或离线字节推断完整收益，也不能把失败归因于 E1。`token_improvement=null`，完整质量/用量比较 UNKNOWN。

阶段累计经各原收据独立求和：G2 本轮 **58/250 次**，已知输入 **407,713** + 未知输入预留 **334,998** = 保守占用 **742,711/5,000,000**；已知输出 **21,399**，未知输出预留 **20,480**。包含 11-15 历史的 through-v1 为 **99/3,000 次**，已知输入 **1,056,110** + 未知输入预留 **395,946** = 保守占用 **1,452,056/60,000,000**；已知输出 **43,301**，未知输出预留 **26,624**。实际完整用量和费用仍未知，预留不是实际 token。

决定：按 D-37 保守回退 E1，原因是一次获准比较没有取得有效完整改善证据；这不是“优化无效”或“优化有害”的结论。回退 [#134](https://github.com/wmqfl861/wls861/pull/134) 的 `ef8598a76cacbefc4673f5111b0c3b3b0b2a91b6` 仅反转 E1 五个文件，整个树精确等于 `c01df9310bbffaf43596a3487cf610d7aa68713a`，独立差异审核 PASS；[三核心](https://github.com/wmqfl861/wls861/actions/runs/43938846786)270/270、[D-40](https://github.com/wmqfl861/wls861/actions/runs/43938843600)六项和源码检查通过，已合入 main `6f13998366ecbd319eb844a413abe765b54d9c6a`。该 main 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43942161390)仍待结果；此前 `c01df93` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43929349216)450/450 CTest、Make 60+25 单独登记，不冒充本次回退/R6 的新门禁。

原 E1 提交、离线验证、ELF/资源/配置与失败请求完整保留，不追加真实 R5，不做第三轮 SSE 修补。回退 parser 不接受已冻结 R5 opt-in profile；该请求继续封存，不用回退后的二进制恢复、重放或改写其账目。

| 私有证据（相对本轮 `evidence/real-line/`） | SHA-256 |
| --- | --- |
| `r5-e1-validation-handoff.json` | `5d76f76699f2fbef4a1dad69a90d82946979394c706572d0ad5778bcb98d1a61` |
| `r5-write-summary-measurement-final/summary.json` | `fa88d949da32d518eb4c54e16f881a32b07f4f7c1eedb11164fc5fed37b31f93` |
| `r5-write-summary-prepared/preparation.json` | `0d7a9cd18d82fbe8c786ba040afc025b86d16e0cd91fa875508760bb99b1b704` |
| `r5-write-summary-execution/result.json` | `633d1ac397564178d74c43f919a66c6c3214be2a4c8b2205b99e9351bc6ad947` |
| `r5-write-summary-execution/metrics.json` | `0a8783bad1c5c3875566d553fe8354326a8b3441e78ec6a4ec7a305ee7d399d6` |
| `r5-last-worker-diagnosis/diagnosis-summary.json` | `0eace82ff146a44ac6ed8e1c0cb9984b28cd41a948fe3029e28cd94bf728d684` |
| `r5-actual-worker-wire-audit/summary.json` | `94f7260d30e35a01b54f45fa309420cb4c79d8ea0d9a15e8c9a661a95b4c85a7` |
| `r5-cumulative-accounting-check.json` | `5a5bbf1df0e68605476c4e86d7c80132d22a27e764d3ffecb49531dcc272eeee` |
| `r5-e1-exact-revert/receipt.json` | `1bc03e70c052055923267989ddc7c896f1a9eb37742ef647cd746e8d5f9ed49e` |

## R6：规格、格式与版本出口记录

R6 采用计划中“报告明确写出没达成原因”的出口路径：U1/U2 仍未通过，R5 完整比较 UNKNOWN，原请求处置及全部失败保持上述结论。迁入 [v0.5 规格](../spec/v0.5/01_实施规格_v0.5.md)及 A1–A5；冻结范围仅为 `structured-v3`、`lean-v1`、`fixed-role-v1`、`append-fold-v1`、`guided-v1`、`bounded-v1`。后续语义变化须新取值，旧 `framed-v1` / `structured-v2` 的字节、身份和恢复解释保持，E1 不作为保留能力。

冻结回归沿用 `r1_structured`/`r1_structured_capture`/`r1_structured_lean`、`append_fold`、`model`、`acceptance_runtime_guidance`、`g1_exit_e2e`、`g2_rehearsal_e2e` 及完整矩阵中的现有旧请求、角色目录、原历史和有界运行检查；不缩减用例、断言或期限。R6 基于回退 main `6f13998`，适用 CI 待核对；历史 `c01df93` 全矩阵仅作为相同产品基线证据，不替代 R6 版本入口检查；标签尚未创建。

旧 `docs/ROADMAP.md` 的 228 行原字节归档到 [roadmap-before-v0.5.md](../handoff/roadmap-before-v0.5.md)，SHA-256 `e3a990bb8b67dda88fd988a8b6d4818bbbf56d1663dc97e80383934266802505`；新路线不超过 60 行。AGENTS、README、STATUS、最新交接、CHANGELOG 与两处 0.13.0 常量同步；计划只维护 §1 状态列与 §15。旧规格、SQL、历史 handoff/审核与本文既有逐次事实保持。

## R7–R10：账目恢复、重试与真实复验

R7 已完成：D-46 [#140](https://github.com/wmqfl861/wls861/pull/140) 合入 `8c3cb975`，D-48 [#141](https://github.com/wmqfl861/wls861/pull/141) 合入 `93b8968`。D-46 让符合条件的已结算 ERROR 按原 token 预留参与观察模式的就绪判断，原始 null 用量不改；D-48 将完整且无可执行内容的合格上游失败/解码拒绝纳入有界重试，每次另行预约。超时、取消、未结算和争议等守卫继续有效。新配置为非流式、180 秒、`system.model_calls.max_paid_retries: 2`；provider profile 的零重试保持 codec 单次发送契约。

| 产品输入 | 本机合成验证 | 托管验证 |
| --- | --- | --- |
| D-46 `3eafe36` | Store 30/30；完整 live workflow CLI 23/23；修改的 run CLI 2/2 | [核心](https://github.com/wmqfl861/wls861/actions/runs/43995465626)270/270、[源码](https://github.com/wmqfl861/wls861/actions/runs/43995465610)及 [D-40](https://github.com/wmqfl861/wls861/actions/runs/43995485977)15/15 PASS；合入 main [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43998851346)450/450 CTest、Make 两 shard PASS |
| D-48 `2854c86` | 完整 live workflow CLI 24/24；HTTPS transport 46/46；专项 14 子场景 | [核心](https://github.com/wmqfl861/wls861/actions/runs/43999386304)270/270、[源码](https://github.com/wmqfl861/wls861/actions/runs/43999386291)及 [D-40](https://github.com/wmqfl861/wls861/actions/runs/43999408575)16/16 PASS；合入 main [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44002919509)450/450 CTest 与 Make 两 shard 通过 |

D-48 main 完整矩阵经独立核对，三种核心构建各 90 套，ASan/UBSan 各 45+45 套，合计 450/450 CTest；Make 两 shard 通过。D-20 检查 90 套 ASan，最大耗时为 live_workflow_cli 的 141.37/360 秒（39.27%），没有超过期限 50% 的项。原件 `run-intake/hosted-d48-main141-44002919509/verification.json`，SHA-256 `65110e96bfaad69b87020ded8f6e378b2c736b7b2e92b08cbc77ebb4018f1d1d`。

两项独立审核均 PASS；D-48 本机测试输入为 `7db60b2`，到最终 `2854c86` 只有产品注释和文档变化，执行代码/测试及 ELF 哈希一致。两项开发验证真实模型调用均为 0，不能当作 U1/U2 验收。新配置最初误把 provider codec 重试也设为 2，离线 `config check` 拒绝；只修新配置为 provider 0/runtime 2 后，配置、六个角色 explain 和资源检查均通过，没有更改 codec 守卫或旧请求。

R8：完整目录备份与清单核对后，在 main `93b8968` 的 Release ELF（SHA-256 `6851fb27406e65fd633de78d0cc642f9c93a46eb69868c0c340a80bde5080454`）上只替换原 argv 的程序路径，原配置和 `--source-root` 路径保持。原 R3 的 `created_at=2026-11-16T18:03:48Z`、40 分钟期限于 `18:43:48Z` 到期；一次运行返回 `wall_clock_exceeded`，随后封存，没有重设时钟或绕过守卫。

R8 显示 `accounting.ready=true`，原末次 ERROR 的有效 basis 为 reservation（输入 87,747、输出 4,096），原用量仍 null。新预约、发送、输入占用增量均 0；原 21 次调用与 send claim 事实、配置、根目录身份保持。数据库因获准续跑发生状态写入，不宣称整库字节不变。U2 没有达成，原 R2/R3/R5 均封存。

R9/R10 都使用 main `93b8968` 与上述同一 Release ELF、新建请求和数据目录，配置与角色 explain 核对通过。R9 使用 `three.txt`、D/TL/W 全 max，限 60 次/150 万输入/8 万输出/25 分钟；R10 使用 `three-split.txt`、配置 high/medium/low，限 80 次/200 万输入/8 万输出/40 分钟。两者均为非流式、180 秒、runtime 重试上限 2/provider codec 0；实际已调用的模型均为 `gpt-6.1-sol`，R10 只观察到 Dispatcher high，TeamLead/Worker 尚未调用。

| 本次运行 | 实际结果 | 调用与保守用量 |
| --- | --- | --- |
| R9 首次，194.753 秒 | `RUNNING/error`；首个 Worker HTTP 200 但响应不可用，`invalid_http_header`；无交付 | 3 次（D/TL/W 1/1/1）；已知输入/输出 12,900/5,441，未知输入/输出预留 75,776/4,096，输入占用 88,676 |
| R9 原命令续跑一次，11.226 秒 | 同一请求再次在 Worker 报 `invalid_http_header`；`RUNNING/error`，无交付 | 新增 1 次及输入预留 86,297、输出预留 4,096；整个 R9 累计 4 次（1/1/2）、输入占用 174,973 |
| R10 首次，46.783 秒 | `RECEIVED/error`；首个 Dispatcher HTTP 200 但响应不可用，`invalid_http_header`；0 团队/任务，无交付 | 1 次（1/0/0）；原输入/输出均 null，输入/输出预留 34,567/4,096 |

R9 续跑前完成整个请求与配置目录的备份、哈希和元数据核对；同一 main/ELF、全部原 argv、配置路径、根身份及原 `created_at=2026-11-17T02:29:32Z` 保持，25 分钟期限仍于 `02:54:32Z` 到期，没有重置。续跑后原 attempt/send claim 事实保持，新 attempt/send claim 各 1。三次运行均 `accounting.ready=true`、自动重试记录 0、已确认交付 0，成果检查未执行。R9 两次 Worker 的原 usage 均 null；预留回退参与就绪不等于补出实际 token。

独立只读核对三个失败 attempt 的分析副本、冻结设置与编码记录后，确认同一响应媒体类型不匹配：持久化及实际发送的请求均为 `stream=false`，发送 `Accept: application/json`；收到普通 HTTP 200 响应头，语法、CRLF 分隔和 chunked 声明核对通过，但 `Content-Type` 均为 `text/event-stream`。头部回调在结束空行处发现不符合预期的 JSON 媒体类型，提前终止接收并记为通用 `invalid_http_header`。三者 `transfer_complete=false`，已观察及保留的响应体字节均为 0；这不证明模型没有执行或响应没有内容，也无法推断 SSE 终态、模型是否完成、实际 usage 或哪个上游网络层选择了该媒体类型。针对该分类和处理的窄范围修正已由 #142 合入；下文保留原候选本机记录，随后给出托管门禁与真实复验。

11-17 新增合计 **5 次调用**，已知输入 **12,900** + 未知输入预留 **196,640** = 输入保守占用 **209,540**；已知输出 **5,441**，未知输出预留 **12,288**。R10 的阶段预算只加 R9 续跑后的累计回执，不再另加 R9 首次的 3 次；R8 增量为 0，也不重计原 R3 的 21 次。

截至 R10 首次结束，G2 本轮为 **63/250 次**，已知输入 **420,613** + 未知输入预留 **531,638** = 保守占用 **952,251/5,000,000**；已知输出 **26,840**，未知输出预留 **32,768**。包含 11-15 历史的 through-v1 为 **104/3,000 次**，已知输入 **1,069,010** + 未知输入预留 **592,586** = 保守占用 **1,661,596/60,000,000**；已知输出 **48,742**，未知输出预留 **38,912**。实际完整 token 和费用仍未知，预留是保守占用。该时点 U1/U2 为 NOT_PASSED；后续 #142 与诊断请求见下文。未发布 0.13.1、未进入 G3。

本轮私有根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：本机验证 `d46-evidence/development-vs3ytoyg/validation.json`、`d48-evidence/validation-final-2854c86.json`；托管日志/审核 `run-intake/hosted-d46-*`、`run-intake/hosted-d48-*`；R8 备份 `r8-backup-f3fbfoe5/manifest.json`、结果 `r8-execution/result.json`；新请求准备与候选离线预检 `real-prep-3gueo4lx/`。下表相对该新请求目录；公开摘要不含凭据、数据库或原始模型响应。

| 本次私有证据 | SHA-256 |
| --- | --- |
| `r9/execution/result.json` | `6fb4a7fb41c7b7fde69e4355f25a0318cc66aac802f3e62e59a0ad7f88cb02d1` |
| `r9-resume1-backup-30mixf8f/manifest.json` | `a2543a3fef7beec4c57b223525fa2228c9149e31815d5b0d53d6654c5b622d68` |
| `r9-resume1/result.json` | `93630cbb44fffbc746569f419b18d7b9940507a92b811e775ac5732ca9114428` |
| `r10/execution/result.json` | `d13eaa717c165922450d72ed29d246b34bd38c8e22bc2c4024a87a72a2a5ae89` |

媒体类型核对原件位于本轮私有根的 `run-intake/r9-r10-content-cause-o9xoamb7/cause.json`，SHA-256 `51af18469714b3dcc47b835bf818a2e8343e19b433a5afcbfb3e769cb1f44981`。该核对没有打开原数据库、读取凭据或调用模型；没有捕获响应体的事实保持。

### 非流式响应媒体不匹配修正（#142）

候选产品提交 `3968e53`、测试提交 `256aa2c`，集成测试输入 `2b131af`（后续仅同步文档）。非流式 Responses 收到 HTTP 200 / `text/event-stream` 时，在原非流式 body、累计字节、SSE 硬限和同一 deadline 内保存诊断正文，返回明确的 `unexpected_response_media_type`；原 `stream:false`、Accept 与实际发送身份保持。仅原 profile 声明流式能力才用既有严格 SSE 解码器作诊断，成功 SSE 也不进入业务。D-48 资格还要求完整 HTTP 与完整严格终态、没有任何输出观察、当前允许的失败代码；成功、拒绝、缺终态、未知事件、取消、超限和超时均不因该路径取得资格。原使用量/null 保留，SQL、冻结配置及现有预算不改。

本机 uid 1000、GCC 13 的一次干净 CMake Release 构建，四套 CTest 全通过：protocol_codec 58/58（0.30 秒）、sse_replay 42/42（4.44 秒）、HTTPS transport 49/49 方法（15.16 秒）、live workflow CLI 24/24 方法（89.62 秒），并行墙钟 94.07 秒。新增传输 16 个子场景、D-48 矩阵新增 11 个媒体子场景（矩阵共 25），核对有界原文、实际与诊断哈希、完整失败后的独立重试预约/send claim/结算、冻结零重试及成功/缺终态/拒绝/无能力等负例。首次编译因新比较 helper 的 Boost Json 显式构造要求失败，修正后正常依赖重编通过；原失败日志保留，未放宽断言或期限。产品非移动改动 60 行，测试 120 行；没有修改 C++ 测试。以上为候选合成端点验证，真实模型调用 0；托管检查及其后真实诊断见下文。

私有验证收据 `media-evidence/final-validation.json`，SHA-256 `38ffc0aaf43e5cce9b19bb1906b5a7765a3196c43e2f248dfa52b05983a487e0`；永久构建/测试日志 `media-build.log`、`media-build-retry.log`、`media-tests-initial.log` 及精确源码和 ELF 哈希均绑定在收据。随后文档 CTest 2/2 通过，但覆盖了构建目录临时 `LastTest.log`；逐方法计数为覆盖前实读，四套通过有永久汇总日志，原逐方法详细日志未保留，原收据保持并注明此限制。该验证证明本地拒绝与重试边界，不保证上游将返回正确的非流式媒体，也不补回 R9/R10 缺失正文。U1/U2 仍为 NOT_PASSED。

#142 最终输入 `99c16e6` 经独立审核 PASS，产品 60 行、测试 120 行；[核心](https://github.com/wmqfl861/wls861/actions/runs/44012736757)270/270、[源码](https://github.com/wmqfl861/wls861/actions/runs/44012736755)和[受影响 ASan](https://github.com/wmqfl861/wls861/actions/runs/44012777575)16/16 通过。合入 main `4b10ee1` 的树与受审输入相同；核心实际测试合成 merge 的父关系及树均核对。受影响 ASan 最大占比为 live_workflow_cli 173.50/360 秒（48.19%），未超 50%。核验原件 `run-intake/pr142-ci-root-ianinol0/`；[main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44015952843)已核对通过：九个构建作业、450/450 CTest、Make 两 shard；实际 checkout 及所有原日志已核对。

### 完整 JSON 被标为 SSE：真实捕获与离线判定

main `4b10ee1` Release ELF（SHA-256 `c29c9881e258bcf03b6da4367d83df84730abb9298261045ee3ff8b50c50aa1d`）用一份新 R9 请求运行 56.721 秒，配置逐字沿用 R9 的非流式、max/max/max、180 秒和 60 次/150 万输入/8 万输出/25 分钟限额。1 次 Dispatcher，0 重试，`RECEIVED/error`、`unexpected_response_media_type`，没有工具效果或交付，账目 ready。

HTTP 完整、头部 477 字节、正文 46,852 字节且未截断；头声明 `text/event-stream`，正文实际为一个 JSON response，status completed，含两个 reasoning 项及一个 submit_plan 调用。PR142 的 SSE 诊断没有识别到 SSE 终态，所以拒绝业务采用；这次原 usage 保持 null，以输入 34,280、输出 4,096 预留参与保守占用。正文 SHA-256 `159910d071f4c02259d9122fa161fc3b3caaa355ff45128829f6a3fbcc501c6f`，响应头 SHA-256 `1c2f47440603d38fcd99fdcc23589f626cfae971304ac86a8d68cc6ea7be3a31`。

从只读分析副本导出原请求、原 profile 与原 body，以同一产品 ELF 执行 `wls replay-model --profile PROFILE --request REQUEST --http-response BODY --http-status 200`：退出 0、completed，现有严格解码器验证输入 16,847、输出 2,638、缓存读 0，缓存写和费用未知。网络请求、付费尝试、Store 打开、凭据解析和工具派发均为 0。该离线结论不改原结算/null 用量，也不恢复原调用的执行权限。私有回放 `media-json-offline-ig_ga46p/verified-conclusion.json`，SHA-256 `e1bfc96d9db4dcf5e85ddbdacce22d6d7f388f61461e923200f4897873aa40fb`；真实原件 `media-real-prep-lcylnhl4/r9-media-check/execution/`。

累计更新：11-17 新增 6 次，输入保守占用 243,820；G2 本轮 **64/250 次、986,531/5,000,000 输入**（已知 420,613 + 未知预留 565,918），through-v1 **105/3,000 次、1,695,876/60,000,000 输入**（已知 1,069,010 + 未知预留 626,866）。费用及未捕获的实际用量保持未知。

决定：新增明确冻结的 `responses_media_type_policy: json-under-sse-v1`，只作用于新请求的非流式 Responses HTTP 200 + SSE 媒体声明，完整严格 JSON 仍由原解码器处理；理由：原文已证明内容可解码，显式策略能修中转错误并保留旧请求解释。缺键仍拒绝，实际 SSE 仍仅诊断，原 framing、字节限额、期限、取消、模型/字段/工具检查均保留。此前准备目录 `json-media-real-prep-4gybr21u/` 的新请求，在 #143 合入后启动；该次随后由预留守卫停止，后续新请求结果见末节。

显式兼容候选本机验证：干净 GCC 13 Release、普通 uid 1000，`cmake --build BUILD --parallel 2 --target wls protocol_codec_tests sse_replay_tests live_model_store_tests https_transport_driver live_model_store_driver` 通过；`ctest --test-dir BUILD -R '^(protocol_codec|sse_replay|live_model_store|https_transport|live_model_store_tls|live_workflow_cli)$' --output-on-failure` 6/6 通过，135.98 秒。六套报告用例分别 59、42、30、50、15、24，共 220；HTTP matrix 包含 12 个新非流式子场景及 2 个流式不变子场景，容量/中断 matrix 包含 7 个新 JSON 子场景。Store 入口验证冻结/恢复、原发送和证据 hash、Completed/usable 的持久化。产品 38 行、测试 76 行；无 SQL/Store 版本变化。独立静态审核无发现；两份新配置各 config check 和三个角色 explain 共 8/8 通过，凭据未解析。

构建输入 `4157894`，测试输入 `90f2e76` 的唯一后续差异为 Python 流式边界用例；C++/构建输入逐字相同。证据 `json-media-evidence/ctest-focused-20261117T044949260904Z.json`，SHA-256 `1ecbcc55eb57dbdf782fed391d89e60dc626cbaac13b4383e2bfbb34df475d05`；永久同前缀 `-LastTest.log` SHA-256 `02333a6b6a2bcf2a2cd4bc7d535aca0a4987c278f8175c97f04de302ce3818b2`，详细原日志已在后续检查前复制。原命令未请求 JUnit，故没有该文件，不补造或重跑替代。随后托管门禁、合入和真实复验结果见下。

[#143](https://github.com/wmqfl861/wls861/pull/143) 最终提交 `36b1d1f` 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44020460645)三配置各 90/90、[源码](https://github.com/wmqfl861/wls861/actions/runs/44020460626)与[D-40 ASan](https://github.com/wmqfl861/wls861/actions/runs/44020456515)16/16 通过，精确 checkout/合成 merge 树均独立核验。合入 main `5a96f40` 与受审树一致，该 main [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44026549272)已独立核验通过：450/450 CTest、Make 两 shard；本机重建 ELF SHA-256 `a32a6969b2dd2556b7c7cdb52f21e5939ee0317e5ee01c901430845d7c237a14` 与六套验证输入一致。该输入的首个新 U1 后因输出超过冻结预留而停止；后续新请求完成结果见末节。独立 CI 收据在 `run-intake/hosted-json-media-pr143-36b1d1f/`，D-40 收据 SHA-256 `6653f52fc15233eccb0ccc25deb60a8e9bbddd665154c2ebb5f55511d29d9c9d`。

### D-20：live_workflow_cli 分片

前一 main 完整 ASan 中 live_workflow_cli 为 193.10/360 秒（53.64%），触发 D-20。该结果在 #143 已开启后才取得，紧接的分片 PR 直接基于 main，将原套件注册为 core/media/retries 三组。24 方法的 AST、原用例/断言/业务期限保持；分组 18+1+5 完整互斥，直接 Python/Make 不指定 group 仍执行全套。每组 Release 180 秒、sanitizer 360 秒不变；CTest 注册 90→92，受影响 ASan 16→18，不修改历史运行计数。

分片本机验证用未修改产品 ELF `c29c988…`：core 18 方法、media 1 方法（原完整子场景矩阵）、retries 5 方法全部通过，分别 10.25/44.41/35.13 秒，普通用户、临时夹具在数据盘。第一次启动在导入共享 TLS 夹具时因 group argv 拒绝，尚未运行方法；仅修正导入前 argv 后运行上述三组，原失败日志保留。CMake 配置及实际注册/选择器核验通过。源码产品 0 行；测试修改仅 runner 分组，没有增加或删除测试断言，不以增加产品行凑比例。代理派发正文多次被网关省略，实际编辑与本机核验由 root 完成，不宣称未发生的独立代理审核；后续 CI 与静态核验分别登记。原件 `d20-shards-evidence/`，托管三组 ASan 为 25.63/70.17/56.02 秒，每组期限 360 秒；核心三配置各 92/92、受影响 ASan 18/18 和源码检查均通过。独立源码审核确认 24 方法、242 处断言语法位置不变；[#144](https://github.com/wmqfl861/wls861/pull/144) 已正常合入，清洁分支/工作树已备份并清理。

### U1/U2真实闭环完成

两个成功请求均固定 main `5a96f40` 的同一 Release 程序，使用非流式、180 秒、`json-under-sse-v1`、`observe-only-v1` 和 runtime 重试上限 2。先前 `r9-json-media` 完成 16 次调用后，某 Worker 报告输出 6,322 超过冻结预留 4,096，正确停于 `needs_accounting`。完整备份后原命令续跑一次，新增调用 0、原 attempt/send claim 不变，仍被守卫拒绝，遂封存。未修改旧事实或放宽守卫；以下两份新配置只把模型及 provider 的单次输出容量一同设为 8,192，请求总额度、期限和业务条件保持。

| 请求 | 真实结果 | 调用（D/TL/W） | 输入 / 输出 token | 墙钟；重试 |
| --- | --- | --- | --- | --- |
| U1：`three.txt`，三角色 max | DELIVERED，1 任务/1 团队，唯一 CONFIRMED，成果 12/12 | 19（3/3/13） | 375,089 / 20,278 | 563.510 秒；0 |
| U2：`three-split.txt`，high/medium/low | DELIVERED，3 个 ACCEPTED 任务/3 团队，唯一 CONFIRMED，成果 12/12 | 32（5/9/18） | 586,088 / 17,278 | 769.480 秒；0 |

实际型号均为 `gpt-6.1-sol`，角色等级从捕获设置核对；51 个调用均 COMPLETED，原发送 claim 和 feedback 各 51，账目 ready，无超额预留。只读分析副本的独立核验与根代理核对一致；费用保持 null，缺失的缓存写用量不补成 0。U1/U2 的 result 哈希分别为 `f01d6b7cc1c25fd775683738bd71c62a1eb3dc4cde38af76101c16274e9fefae`、`597ec4d37a968feee8eaf4f4508da31892ba3ad583eaac2ccfc09cff54a7cf44`。

与 11-15 的同一 `three.txt` 比较：调用 29→19（−34.48%），输入 575,141→375,089（−34.78%），输出 18,431→20,278（+10.02%），墙钟 790.514→563.510 秒（−28.72%）。分角色输入为 D 31,194→63,217、TL 76,857→78,335、W 467,090→233,537；Worker 调用 24→13。该表比较首次实跑耗时；旧请求首次导出被阻止，后来才以 0 新模型调用恢复导出。它是两次真实运行的描述性比较，期间存在多项产品和冻结配置变化，不能分别归因，也不能当作尚未实施的 R11 收益。

截至 U2 结束，11-17 新增 73 次；G2 本阶段累计 **131/250 次、2,355,289/5,000,000 输入保守占用**，through-v1 **172/3,000 次、3,064,634/60,000,000 输入保守占用**。历史未知预留没有改写；剩余 G2 为 119 次、2,644,711 输入，足够按原 60 次/150 万输入上限准备一次 R11 对比，后续仍按实际用量检查。

私有根同前，成功原件 `output-headroom-real-xjvkt5yx/r9-output-headroom/execution/`、`r10/execution/`；独立复核 `u1-review-new/`、`u2-review-new/`，累计账目 `budget-after-u1-u2.json`。旧预留超限请求及备份位于 `json-media-real-prep-4gybr21u/`。R11 按 E-14→E-11→E-1 顺序实施并完成真实比较后，再做 R12 版本/标签；当前不宣布整个项目完成。

### R11 E-14：可选 Worker 提示

本项只新增显式选择的 function 模板及说明，默认模板不变，不改产品 C++、SQL、工具权限或旧快照。已结算写回执只证明保存字节，两次历史调用是效率目标，必要取证与验收继续保留。现有 config check/explain 通过且不解析凭据，独立源码与测量审核通过；[#146](https://github.com/wmqfl861/wls861/pull/146) 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44040840012)276/276 和[源码检查](https://github.com/wmqfl861/wls861/actions/runs/44040840067)通过并合入。

成功 U1 的 19 份原捕获全部重现实际 wire SHA；仅 13 份 Worker 的 function_prompt、依赖身份及派生哈希变化，管理角色和全部 wire 历史逐字不变。Worker 总 body 1,036,252→1,054,036 B（+17,784，+1.7162%）；全集 1,349,285→1,367,069 B（+1.3180%）。基线 history_page/history_read 为 0/0，未宣称历史调用或真实 token 已改善。原 JSON 数字拼写与 Python 默认序列化不一致的首次断言失败已保留；改用保留原数字词法的同输入派生后通过，未改变原件。私有证据 `r11-e14-evidence/lexical-matched/summary.json`，SHA-256 `d09bc2071f9daa95d2f37dadd2eb7aa22ac146dd0de28348f979e27d2ea9575c`。38 次 encode-only、网络/付费/工具派发均为 0；后续组合真实比较仍待 E-11/E-1 合入。

#144 main [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44033514578)已独立核验通过：核心 276/276、ASan 92/92、UBSan 92/92，共 460 次 CTest；Make 50 个 C++ 套件/883 子例及 8+25 Python 套件/120+331 子例。原 24 个 CLI 方法保持，三个分组 ASan 为 24.46/91.85/55.05 秒；全部 92 套最大期限占比 48.65%，无 D-20 超半项。原始日志及收据位于 `run-intake/hosted-live-cli-main144-44033514578/`。

#146 合入后的 [main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44042486727)和[源码检查](https://github.com/wmqfl861/wls861/actions/runs/44042486527)也已独立核验通过：460/460 CTest、Make 子例数同上；全部 92 个 ASan 套件最大期限占比 38.60%，无 D-20 超半项。收据 `run-intake/hosted-e14-main146-44042486727/verification.json`，SHA-256 `eeeadead5344bd5c8f0f8ce742c6f1079772ca89e501e96bc15dddf7a7174061`。

### R11 E-11：最近两轮原生 reasoning

新增显式冻结的 `responses_reasoning_history_policy: last-two-rounds-v1`，只用于独立 Worker 的 structured-v3/lean-v1 Responses 输入。发送副本省略两轮之前的 reasoning，完整 payload、projection、密文、item/call ID 检查仍先执行；工具调用、结果和原历史不变。缺键保持原 profile 和 wire。独立审核发现的历史标记计数边界已修正，并覆盖直接构造的窗口标记和空 native payload。产品 40 行、测试 79 行；本机 Release 协议 61/61 和 config_loader、三组 r1_structured、sse_replay 5/5 通过。首次用例缺少必需字段的失败保留，修正夹具后通过。

同一 U1 的 19 份捕获，当前候选缺键 wire 19/19 全等；启用 E-11 后只省略 57 个旧 reasoning item，保留项逐字相同。相对 E-14，全集 body 1,367,069→1,101,502 B（−265,567，−19.4260%），Worker 1,054,036→788,469 B（−25.1952%），管理角色不变。累计相对原 U1 少 247,783 B；尚未宣称真实 token 改善。最终测量 `r11-e11-evidence/same19-reviewed/summary.json`，SHA-256 `0ab4d201b0f5e902fae6655f2261448673b1d44ae114934a1a2218248dcc2c91`；源码审核 `e11-source-review-final-20261117.json`，全部原日志留在同一私有目录。

手动 asan-affected 选择器新增 protocol_codec，由 18→19 个完整套件，CTest 注册数保持 92。[#147](https://github.com/wmqfl861/wls861/pull/147) 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44048414650)三配置各 92/92、[源码检查](https://github.com/wmqfl861/wls861/actions/runs/44048414672)和[受影响 ASan](https://github.com/wmqfl861/wls861/actions/runs/44048538759)19/19 已独立核验通过并合入。PR 合成 merge 与受审头同树，手动作业为精确分支头；全部 295 次套件执行未超过时限半数，ASan 最大占比为 worker_tools 60.74/180 秒（33.7444%）。私有收据 `run-intake/hosted-e11-pr147-26af6bf/verification.json`，SHA-256 `20dad6cb82d7195461be07b17748ea1a5a97d0a201b593bfa7167266131c8082`；合入后的 main 完整矩阵另行核验。

### R11 E-1：恢复可选写回执摘要

按 D-51 恢复 #133 原实现，仅适配已合入的媒体及 E-11 上下文。五个原文件的所有增删行与原补丁一致：产品 107 行、测试 148 行、契约 14 行。`write-summary-v1` 仅投影已接受、完成、持久且身份、内容和哈希匹配的 native fs_write 成功回执；不满足条件则保留原结果，原 Store、历史和回读不变。独立源码审核通过；当前 Release 协议 63/63、配置 24/24（2/2 CTest）通过，托管门禁待执行。历史 29/29 默认恒等证据按 D-51 引用，未重复运行。

同一成功 U1 的 19 份捕获，以 E-14+E-11 为基线：缺少 E-1 选项时 19/19 wire 全等；启用后全集 1,101,502→977,306 B（−124,196，−11.2751%），Worker 788,469→664,273 B（−15.7515%）。只有 5 份已验证写回执在后续历史中的 44 次出现被摘要；管理角色及其他 wire item 逐字不变。三项累计相对原 U1 的 1,349,285 B 减少 371,979 B（27.5686%），尚未验证真实 token、调用数或交付质量收益。

私有证据 `r11-e1-evidence/same19/summary.json`，SHA-256 `835d9b776610767e970f13f231fb382408996cb064b3d26c7311cdbcd8eda6fb`；源码复核 `e1-source-independent-review.json`，原恢复比对和构建、逐方法日志留在同目录。测量仅编码导出的输入，392 个输入及产品文件哈希保持；无数据库打开、凭据解析、工具派发或真实调用。组合真实比较在三项合入之后执行。
