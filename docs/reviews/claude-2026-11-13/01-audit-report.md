# 01 审核报告

审核对象：`main@83fcb9f`（产品基线 `b09848e`，0.11.1-r3.3c-audit，Store 410），以及本地 agent 的交接文档 `docs/reviews/g1-main-handoff-20261112.md`、`docs/reports/g1-exit.md`、`docs/reports/stage0-convergence.md`、`docs/STATUS.md`。文中行号都以 `main@83fcb9f` 为准。

证据标记：
- 【实测】在审核环境里实际运行。
- 【CI 日志】我下载并逐个读过的 GitHub Actions 原始日志。
- 【静态】读源码得出，没有运行。
- 【推算】由已有数字推出，要用真实运行校准。
- 【查证】2026-11-13 查阅的外部文档，实施时应再核对。

## 1. 结论

1. **上一轮计划的 G1 部分全部落地，过程守规矩。** 阶段 0 完成后 30 小时内合并了 14 个 PR，产品代码合计 2,936 行，最大的一个 413 行，全部直接合入 main，没有堆叠。上一轮审核时的状态是 3 天、13 个分支、0 合并。
2. **G1 合成门禁已经达成，可以判定通过。** `main@b09848e` 和 `main@83fcb9f` 的完整矩阵都是 9 个 job 全部成功【CI 日志】；交接文档写的“完整矩阵在途”已过时。
3. **合成门禁测不出真实模型能不能用。** 合成端点的应答是按完整契约预先写好的；真实模型只能看到宿主发给它的东西。这次读代码发现 4 个阻断问题（LIVE-12 至 LIVE-15）：按现在的 main 接真实端点，Dispatcher 提交计划这第一步大概率过不去；任何一次模型调用失败都会让请求永久卡住。
4. 另有 8 个会频繁打断无人值守运行的问题（LIVE-16 至 LIVE-23）和 4 个成本问题（CTX-8 至 CTX-11）。
5. 交接文档的事实是准确的，问题在写法：限定语太多，owner 需要做什么被埋在最后。
6. 下一步是加一个很短的“真实模型就绪”阶段（G2-P，约 10 个小 PR），然后做 G2。owner 只需要提供一个 API 密钥。细节见 `03-fix-list.md` 和 `04-plan.md`。

## 2. 核对了什么

| 对象 | 做法 | 结果 |
| --- | --- | --- |
| 产品代码改动 `4987f2c..83fcb9f`（43 个文件，+3919/−256） | 全文读了 `protocol_codec`、`structured_context`、`offline_runtime`、`worker_history`、`structured_history`、`run_cli`、`live_model`、`provider_profile`、`schema`、`tool_descriptions`、`correction_detail`、`response_metadata`、`responses_compat`；读了 `event_runtime`、`https_transport`、`store`、`store_live`、`store_history`、`config_loader`、`workspace_posix`、`worker_tools` 的全部改动和相关函数 | 发现见 §5–§8【静态】 |
| `sse_replay` 的改动 | 没有细读 | G2 用非流式，不受影响 |
| 完整矩阵 `b09848e`（run 43326432461） | 下载 9 个 job 的日志 | strict、umask-0002、Clang-18、UBSan 各 83/83；ASan 两片 42+41；Make-ASan 两片成功【CI 日志】 |
| 完整矩阵 `83fcb9f`（run 43331867644） | 查 job 结论 | 9 个 job 全部成功，2026-11-13 00:42:46 UTC 结束 |
| G1 门禁数字 | 在 Make-ASan-2/2 日志里找到测试输出的结果行 | 与交接文档逐项一致，见 §3.2【CI 日志】 |
| PR 门禁时长 | 查 #62、#66 的 PR 运行 | 10 分 35 秒、11 分 08 秒 |
| 每个 PR 的行数 | GitHub 接口逐 PR 统计 | 见 §3.1 |
| M1 内核 | 本环境构建并运行 | 52 项通过【实测】 |
| Python 契约测试 | 本环境运行 `tests/contracts_tests.py` | 38 项通过【实测】 |
| LIVE-12、LIVE-13 | 用仓库里的 Schema 和 `jsonschema` 复现 | 见 §5【实测】 |
| S1 库与 `wls` | 没有构建 | 本环境缺 Boost、libcurl 头文件，不能装包 |
| 外部事实 | OpenAI 定价页、提示缓存指南、函数调用指南、GPT-6 使用指南 | 见 §12【查证】 |

## 3. 上一轮清单的落实情况

### 3.1 逐项对照

| 条目 | 内容 | PR | 产品行 / 测试行 | 结论 |
| --- | --- | --- | --- | --- |
| F-01 | 合并 G1-2 协议兼容 | #48 | 756 / 1370 | 完成 |
| F-02 | 合并 G1-1 估算计费 | #49 | 609 / 720 | 完成。用普通 merge 跟进 main，代替变基，结果等价 |
| F-26 | 文档对齐 | #51、#67 | 0 | 完成。蓝图迁移等采纳，本轮已决定（D-13） |
| F-27a | 在途分支处置 | — | — | 完成。#50 已关闭，当前没有打开的 PR |
| F-30（G1-A） | 测试分层 | #53、#62 | 236 / 270 | 完成。PR 门禁约 11 分钟，达到 15 分钟目标 |
| F-09（G1-B） | 纠错回路 | #55 | 316 / 392 | 完成，但可纠正的范围比建议的窄，见 LIVE-14、LIVE-16 |
| F-03（G1-C） | 工具说明文字 | #56 | 279 / 172 | 完成，但任务说明的结构没给模型，见 LIVE-12；宿主强制的规则没写进说明，见 LIVE-22 |
| F-04（G1-D） | 提示词、环境卡片 | #57 | 68 / 136 | 完成，但示例模板与实际可用工具不一致，见 LIVE-18 |
| F-05（G1-E、F） | 请求布局、固定工具集 | #60、#63 | 496 / 641 | 完成。`tool_choice` 用了 auto，见 LIVE-21 |
| F-06（G1-G、H） | 元数据不出站、追加折叠 | #64、#65 | 498 / 641 | 完成。只瘦了 `fs_read` 的结果，见 CTX-8 |
| F-07（G1-I） | 域名解析、可选代理 | #54 | 181 / 222 | 完成，贴着 200 行预算 |
| F-08、F-28a（G1-J） | 付费保护、续批 | #58 | 293 / 340 | 完成。失败调用的记账没有解决，重试仍是 0，见 LIVE-15、LIVE-19 |
| F-11（G1-K） | 宽容解码 | #59 | 125 / 166 | 完成，按 D-4 |
| F-10（G1-L） | `wls run` | #61 | 413 / 327 | 完成。预留算法和摘要可读性见 LIVE-20、LIVE-23 |
| G1-M | 总门禁测试 | #66 | 31 / 331 | 完成 |

G1-A 至 M 合计：14 个 PR，产品 2,936 行，测试 3,638 行，测试与产品之比 1.24。上一轮给的预算合计是 2,950 行。

### 3.2 G1 门禁结果

来自 `b09848e` 的 Make-ASan-2/2 日志【CI 日志】：

| 门禁条件 | 结果 |
| --- | --- |
| 一条 `wls run`，一次确认 | 是 |
| 3 个只用文件工具的任务，30 次以上调用 | 32 次：Dispatcher 4、TeamLead 9、Worker 19 |
| 至少一次可纠正错误被自动纠正 | 1 次（读不存在的文件） |
| Worker 第 3 次调用起的模拟缓存读取占比超过 70% | 最低 72.0% |
| 结束时输出费用明细 | 是，估算 234,326 micro-USD |
| 其他数字 | 全部请求体合计 1,312,535 字节，平均每次 41.0 KB；整体模拟缓存读取 62.2%，同样材料按旧布局是 52.4%；请求体是旧布局的 94.8% |

`g1_exit_e2e` 在五个 CTest 配置里都通过：strict 8.74 秒、umask-0002 8.68 秒、Clang-18 8.25 秒、UBSan 61.09 秒、ASan 91.78 秒，时限 300 秒。

这些数字说明工程链路是通的，布局改造有效。它们来自合成端点（按“每 4 字节 1 个 token、前缀匹配”模拟缓存），不能当作供应商缓存或真实费用。

## 4. 交接文档核对

**准确的部分。** 核对了 14 个 PR 的合并提交、测试注册数 83、G1 门禁的全部数字，都与 GitHub 和 CI 日志一致。“真实调用 0 次”与代码和配置的现状相符。

**需要更新的部分。**

| 交接文档的说法 | 现在的事实 |
| --- | --- |
| `b09848e` 完整矩阵 IN_PROGRESS | 已于 2026-11-13 00:04:04 UTC 全部成功；`83fcb9f` 也已全部成功 |
| G1 未宣称正式通过 | 本轮判定通过，见 `02-decisions.md` D-10 |
| 两个 CI ASan 单项超时、本机 capture 超时保持 OPEN | 分片之后 `b267ae5`、`b09848e`、`83fcb9f` 三次完整矩阵都通过；处置见 D-11 |
| D-2、D-3 未给出 | 已定，见 D-2、D-3 |
| 私有 CI 原始证据托管 BLOCKED | 取消这项要求，见 D-12 |

**写法上的问题。** 227 行里“不代表”“不能”“不等于”一类的限定语占了很大篇幅；六项状态表、完整 SHA 的罗列对接手的人帮助不大；owner 需要做的事只在 §9 末尾出现一次。建议见 GOV-7。

## 5. 阻断问题

这四项不解决，接上真实端点后走不完第一个请求。它们在合成测试里都不会出现，原因相同：脚本模型的应答是按完整契约写好的，而且合成端点从不失败。

### LIVE-12 计划里的任务说明，模型看不到它的结构

- 发给 Dispatcher 的 `submit_plan` 参数定义里，`nodes[].brief` 只有一行：`{"type":"object","description":"team_task：TaskBrief，按 task_brief.schema.json 单独校验"}`（`schemas/plan.schema.json:56-59`）。
- 宿主收到计划后，却按 `task_brief.schema.json` 单独校验每个 `brief`（`src/schema.cpp:321-330`）：5 个必填字段；验收项必须是带 `ac1` 这类编号的对象；`constraints_verbatim` 必须逐字摘录原文；`constraint_refs` 必须列全。这些要求模型一条也看不到。
- 同类问题还有三处：`submit_amendment_plan` 的 `plan` 和 `dispositions`、`check_submission` 的 `work_order`，发给模型的都是指向另一个文件的 `$ref`。
- 【实测】用仓库里的 Schema 复现：一个只按可见定义写出来的合理计划（`brief` 里写了 `objective`、`acceptance_criteria` 字符串数组、`deliverables`），在计划层 0 个错误，在任务说明层 7 个错误。

### LIVE-13 校验失败时，模型只收到一句固定的话

- `src/schema.cpp:315-320` 调用校验器时不收集错误明细，统一返回 `draft-07 validation failed for <名字> model input`。
- 纠错回路把这句话原样交还模型（`src/offline_runtime.cpp:957-963, 1226-1227`）。模型不知道错在哪个字段、应该是什么。
- `schema_retry_max` 默认是 2（`config/examples/system.yaml:110`），含义是同一目标连续失败两次就停。
- LIVE-12 和 LIVE-13 加在一起的后果：第一次提交计划因为 `brief` 结构不对被拒，第二次仍然不知道错在哪，再次被拒，请求停止。

### LIVE-14 Schema 之后的规则检查不走纠错回路，其中一部分本应由宿主填写

- Schema 通过之后，Store 还会按一批规则检查模型给的参数，全部以 `InvalidInput` 抛出。例如：

| 规则 | 位置 |
| --- | --- |
| 任务说明必须引用全部约束 | `src/store.cpp:1586` |
| 约束摘录必须是请求原文的子串 | `src/store.cpp:1595` |
| 检查通过时不能带返工说明；不通过时必须带一份完整的返工说明 | `src/store.cpp:2384-2385` |
| 检查通过时必须覆盖每一条验收项，而且每条都是 `yes` | `src/store.cpp:323, 2412` |
| 检查通过时，Worker 的验证记录不能有 `fail` 或 `not_run` | `src/store.cpp:337` |
| 检查通过时，Worker 的 `known_issues` 必须为空 | `src/store.cpp:347` |
| 验收通过必须带完整的逐条证据 | `src/store.cpp:2490` |

- 这些异常没有归入纠错回路。`src/offline_runtime.cpp:1231-1245` 把它们记成 `runtime_error` 后继续上抛，`src/event_runtime.cpp:877-889` 停掉整批。
- 再执行一次 `wls run` 时，管理角色只能看到上一次“可纠正失败”的尝试（`src/offline_runtime.cpp:761-767`），看不到这次的错误原因，很容易原样再犯。
- 约束相关的两条规则，模型其实没有选择余地：当前实现要求每个任务引用全部约束、逐字包含全部约束原文（`src/store.cpp:1584-1598`）。让模型逐字抄写宿主已经掌握的内容，只会增加出错机会。`issue_work_order` 也有同样的两个必填字段。

### LIVE-15 一次失败的模型调用会让请求永久卡住

- 按冻结单价估算费用，只对状态为 `completed` 的应答生效（`src/store_live.inc:151-158, 190-196`）。限流（429）、服务端错误（5xx）、超时、连接失败、输出被截断（`incomplete`）、拒答、解码失败、模型名不匹配，都算“费用未知”。
- 只要请求里有一次费用未知的调用，后面所有付费调用都被拒绝（`src/store_live.inc:436-447`），`wls run` 返回 `needs_accounting`。
- 唯一的核销命令 `wls reconcile-billing` 需要供应商的签名收据（`src/cli.cpp:366`）。普通端点不提供这种收据，所以没有办法解除。
- 现有测试把这个行为固定了下来：`tests/test_billing_baseline_e2e.py:1074-1108`（中断后重跑仍是 `needs_accounting`）、`:1607-1632`（HTTP 429、503 要靠签名收据核销）。
- 真实端点上限流和超时是常事；推理模型的输出被 `max_output_tokens` 截断也不少见。
- 相关的一个陷阱：估算要求价格配置与应答里的缓存字段完全对应，否则也算未知（`src/provider_profile.cpp:236-240`）。配置写“缓存写入不适用”而应答带了写入数，或者配置写了缓存读取单价而应答没带读取数，都会卡住。OpenAI 文档的示例里用量带有缓存写入数，按时效的明细对应 GPT-5.6 及以后的型号；哪些型号会返回缓存写入数，文档没有写明【查证】。这只能靠第一次真实调用得知，而那一次失败就会卡住请求。

## 6. 会频繁打断运行的问题

| 编号 | 问题 | 位置 | 后果 |
| --- | --- | --- | --- |
| LIVE-16 | 只读工具一出错就停批：路径不存在、路径格式不对、偏移越界都抛异常，只有“`fs_read` 读不存在的文件”一种算可纠正。写工具在产生任何效果之前失败（父目录不存在、路径非法、超容量、目录已存在）记为 FAILED，只有“内容前置条件不符”一种算可纠正 | `src/workspace_posix.cpp:45-56, 254-258, 748`；`src/correction_detail.hpp:76-97`；`src/event_runtime.cpp:841-845, 1099-1104` | 模型的常见小错会停掉整批。重跑 `wls run` 能继续，但做不到一条命令跑完 |
| LIVE-17 | 路径约定不一致：`fs_*` 只收相对路径；`submit` 的交付路径必须以 `/work/` 开头；功能提示词和环境模板里的例子都是 `/work/...` | `src/workspace_posix.cpp:254-258`；`src/offline_runtime.cpp:131`；`config/examples/functions/general/prompts/worker.md:8`；`env.yaml` | 模型照着例子写 `/work/x`，文件工具拒绝 |
| LIVE-18 | 示例模板给 Worker 授予 `run`、`op_status`、`op_output`、`op_cancel`、`note`、`ack_change`，环境模板介绍 bash、git、python3。但 `wls run` 创建工具宿主时没有沙箱清单，调用 `run` 在启动进程之前就被拒绝，记为 FAILED，该批停止。功能提示词还要求把成果放 `/work/out`、中间文件放 `/work/tmp` | `config/examples/functions/general/function.yaml:28`；`src/run_cli.inc:352`；`src/sandbox_posix.cpp:1535-1536` | 模型被引导去用一个用不了的工具；每个任务至少多两次建目录调用 |
| LIVE-19 | 没有重试。`max_paid_retries` 非 0 会被配置加载器拒绝 | `src/config_loader.cpp:536`；`src/provider_profile.cpp:366` | 一次 429 就停批。D-3 已定为最多重试 2 次 |
| LIVE-20 | `wls run` 的单次费用预留按“整个上下文窗口的输入加最大输出”计算，预算小于这个数就拒绝启动；运行中每次调用都按这个固定值预留 | `src/run_cli.inc:281-293`；`src/event_runtime.cpp:673` | 以 40 万窗口、12.8 万最大输出、每百万 token 输入 1.25 美元、输出 10 美元的型号为例，预留是 1.78 美元，1 美元的预算直接被拒；预算里始终有这么一块用不上【推算】 |
| LIVE-21 | `tool_choice` 的模式是 `auto`，模型可以只回文字不调工具，白白用掉一次纠错机会 | `src/protocol_codec.cpp:554` | 配合上限 2 次，容易无谓停止 |
| LIVE-22 | 工具说明没有写出宿主强制的规则。例：`check_submission` 的说明写“不通过时可以带返工说明”，实际是必须带；通过时的四条规则（见 LIVE-14 的表）说明里都没有 | `src/tool_descriptions.hpp:33` 等 | 模型第一次做这些动作时几乎必然出错。Worker 如实填写 `not_run` 或 `known_issues` 时，这个任务按现有规则永远过不了检查，而模型不知道原因 |
| LIVE-23 | `wls run` 停下时，摘要里只有错误码，原因统一写成 “inspect private Store evidence”；交付目录里的文件名是 `artifact-<节点>-<序号>.bin`，原文件名只在清单里 | `src/event_runtime.cpp:527, 875, 887`；`src/run_cli.inc:225-236`；`src/store.cpp:2658` | 停了不知道为什么，要自己去查数据库；用户拿到的交付文件要自己改名 |

## 7. 成本问题

平均每次请求 41 KB，而门禁里的任务只是写 11 字节的文件，所以这 41 KB 基本是固定开销【CI 日志】。其中工具定义和系统文本约 22 KB，是可缓存的稳定前缀；其余是契约、历史和本次状态【推算】。

| 编号 | 问题 | 位置 | 影响 |
| --- | --- | --- | --- |
| CTX-8 | 写工具的结果把参数原样带回：结果里的 `operation` 行连着 `arguments` 一起取出，`fs_write` 写入的全文因此出现两遍；结果里还带着设备号、inode、临时文件名、各种哈希等宿主内部证据。瘦身目前只对 `fs_read` 做了 | `src/store.cpp:851-853`；`src/worker_tools.cpp:582`；`src/protocol_codec.cpp:340-372, 511-528` | 写文件多的任务，历史体积接近翻倍 |
| CTX-9 | “本次状态”每次调用都排在最后，不能缓存，按全价计费。现在只去掉了来源元数据，预算流水、各种版本号和代次、任务行的全部字段都还在 | `src/protocol_codec.cpp:374-378, 539-540`；`src/offline_runtime.cpp:496-563` | Worker 每次调用没有命中缓存的部分约 8–11 KB，按全价计费【推算】。其中多少是本次状态，要等 P6 的分段统计 |
| CTX-10 | 折叠阈值和输入预留都拿字节数直接当 token 数用。字节数总是不小于 token 数，所以安全，但折叠会在真实窗口的 15%–20% 处就发生 | `src/offline_runtime.cpp:783-805, 812, 827-836` | 长任务会过早折叠，每次折叠都让缓存失效一次。G2 的小任务碰不到 |
| CTX-11 | OpenAI 从 GPT-5.6 起收缓存写入费（默认是未缓存输入价的 1.25 倍），并用 `prompt_cache_options` 取代 `prompt_cache_retention`；更早的型号不收写入费，可以用 `prompt_cache_key` 改善路由【查证】。编码器目前两个参数都不发 | `src/protocol_codec.cpp:545-559` | 选哪一代型号，决定价格表怎么填。G2 第一轮选不收写入费的型号，可以回避 |

## 8. 并入后续阶段的发现

| 编号 | 问题 | 位置 | 并入 |
| --- | --- | --- | --- |
| TOOL-6 | `op_output` 只返回 base64；`run` 的结果不带输出正文，模型看一次命令输出要再调一次工具再解码 | `src/store.cpp:3681`；`src/worker_tools.cpp:523-526` | G4-C |
| ARCH-6 | 请求的墙钟从创建时刻算起，等用户回答问题的时间也算在内，过了 `max_wall_minutes` 请求就不能再调用模型 | `src/store.cpp:104-121` | G9；G2 先把上限配大 |
| LIVE-24 | 已知类型的输出项（`message`、`function_call`、`reasoning`）内部仍是严格白名单，供应商给这些项加一个新字段就会解码失败，而这些项本来就是原样回传的 | `src/responses_compat.hpp:81, 115, 157` | G2-P 的 P8 |
| LIVE-25 | 请求不带 `User-Agent`；代理只支持 `http://数字IPv4:端口`，不支持主机名、认证和 SOCKS | `src/https_transport.cpp:780-787, 476-493` | `User-Agent` 并入 P4；代理形式按 G2 实际需要再定 |

## 9. 流程与文档

| 编号 | 问题 | 建议 |
| --- | --- | --- |
| GOV-7 | 交接文档和 STATUS 写得像证据清单，读者要自己从限定语里找出“现在能做什么、下一步要我做什么” | STATUS 开头固定三行：现在能做什么、还不能做什么、需要 owner 做什么。交接文档只在阶段出口写，不超过 80 行，不列完整 SHA，用 PR 链接代替。不再写六项状态表 |
| GOV-8 | 新增了约 3,000 行产品代码，版本号还是 `0.11.1-r3.3c-audit` | G2-P 出口升 0.12.0，G2 出口升 0.13.0，见 D-15 |
| GOV-9 | 远端有 123 个分支，绝大多数已合并或已处置 | 按 D-14 清理 |

## 10. 值得保持的做法

- 每个 PR 一件事、直接基于 main、行数在预算内。
- 合成端点的门禁测试写得扎实：真实命令行、真实数据库、真实 TLS，逐次核对线上字节与账目。G2-P 的预演门禁可以直接在它上面加剧本。
- 新行为都用冻结取值表达，旧请求按原解释继续。
- 失败和取消的 CI 结果如实保留，没有用后来的成功去覆盖。

## 11. 审核局限

- S1 没有在审核环境构建和运行，对 S1 的结论除标了【CI 日志】【实测】的以外都是【静态】。
- LIVE-12 至 LIVE-15 是按代码路径推断的，没有真实模型的验证。`04-plan.md` 安排了 G2-0 探针，密钥一到就用现有 main 发一个真实请求，用来证实或修正这些判断。
- 请求体各段的大小是【推算】，G2-P 的 P6 会给出实测的分段统计。
- 没有逐行读 `sse_replay.cpp` 和 `store.cpp` 中与本轮改动无关的部分。

## 12. 来源

- OpenAI 定价页（型号与每百万 token 单价）：<https://developers.openai.com/api/docs/pricing.md>
- OpenAI 提示缓存指南（最小长度、缓存写入计费、`prompt_cache_key`、`prompt_cache_options`、用量字段）：<https://developers.openai.com/api/docs/guides/prompt-caching>
- OpenAI 函数调用指南（工具定义、`function_call` 与 `function_call_output` 的形状、推理项回传）：<https://developers.openai.com/api/docs/guides/function-calling>
- OpenAI GPT-6 使用指南（型号、推理档位、哪些型号的工具调用必须走 Responses 协议）：<https://developers.openai.com/api/docs/guides/latest-model>
- GitHub Actions 运行：[43326432461](https://github.com/wmqfl861/wls861/actions/runs/43326432461)、[43331867644](https://github.com/wmqfl861/wls861/actions/runs/43331867644)、[43324561502](https://github.com/wmqfl861/wls861/actions/runs/43324561502)、[43275846213](https://github.com/wmqfl861/wls861/actions/runs/43275846213)
