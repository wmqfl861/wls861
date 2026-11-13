# 03 修改与修复清单

对应 `01-audit-report.md` 的发现编号。行号以 `main@83fcb9f` 为准，实施时以符号名定位。

通用约定：
- 每项一个 PR，直接基于 main，产品行预算是 `src/` 与 `include/` 的增删行数上限，测试行数不超过产品行数的 2 倍。
- 按 D-10，`structured-v3`、`lean-v1`、`fixed-role-v1`、`append-fold-v1`、`guided-v1`、`bounded-v1` 在 G2 出口之前可以原地调整，不另起版本名。`framed-v1`、`structured-v2` 的旧请求回归保持不变。
- 数据库版本保持 410。本清单没有一项需要新表。
- 已有的 `claude-2026-11-11/04-fix-list.md` 里 F-12 及以后的条目继续有效，本文只列新增和调整。

## 一、G2-P：真实模型就绪

### P1 发给模型的工具参数自包含，宿主补全其余字段

对应：LIVE-12、LIVE-14、LIVE-22。分两个 PR。

**P1a 计划、任务说明、工作说明**

- 位置：`schemas/` 下的 `plan`、`task_brief`、`work_order`、`submit_amendment_plan`、`check_submission` 五个 Schema；`src/schema.cpp:189-267`（模型侧投影）；`src/offline_runtime.cpp:1045-1057, 1110-1125, 1140-1156, 1189-1198`；`src/tool_descriptions.hpp`。
- 做法：
  1. 生成发给模型的参数定义时，内联全部 `$ref`，去掉顶层的 `$schema`、`$id`、`title`。
  2. `submit_plan` 发给模型的定义收敛为下面的形状：

     ```
     summary                              必填
     nodes[]                              1–20 项
       node_id                            必填，小写字母、数字、连字符
       title                              必填
       template                           可选
       brief                              必填
         goal                             必填
         acceptance_criteria[]            必填，至少 1 项
           text                           必填
           check                          可选
         scope[]、non_goals[]             可选
         expected_outputs[]{path, description}   可选
         notes                            可选
     ```

  3. 其余字段由宿主在按完整 Schema 校验和入库之前补全：

     | 字段 | 补全值 |
     | --- | --- |
     | `nodes[].kind` | `team_task` |
     | `nodes[].depends_on` | `[]` |
     | `nodes[].required` | 模型没给时为 `true` |
     | `brief.title` | 取 `nodes[].title` |
     | `brief.acceptance_criteria[].id` | 模型没给时按顺序补 `ac1`、`ac2`…… |
     | `brief.constraint_refs` | 约束目录的全部编号 |
     | `brief.constraints_verbatim` | 约束目录的全部原文 |

  4. `issue_work_order` 和 `check_submission.work_order` 发给模型的定义收敛为 `approach`、`steps[]{text, check}`、`done_definition`、`pitfalls[]`、`notes`。宿主补 `steps[].id`（`s1`、`s2`……）、`constraint_refs`、`constraints_verbatim`。返工说明的约束摘录要并入上一版说明已有的摘录，满足 `src/store.cpp:2418-2424` 的检查。
  5. `submit_amendment_plan` 的 `plan` 用第 2 点的形状，`dispositions` 内联。
  6. 不再向模型展示的字段：`capability`、`input`、`review`、`inputs`（等 `/input` 接通再放回）。
  7. 兼容已有脚本：宿主接受的输入可以是发给模型的定义的超集。输入里带了宿主字段时，值与宿主推导的一致就接受，不一致按可纠正错误返回。入库的完整记录和它的 Schema 不变。
- 验收：
  - 导出每个角色每个工具发给模型的参数定义，断言没有 `$ref`，没有运行时字段，没有说明里写着“必须省略”“必须为空”的属性。
  - `01-audit-report.md` LIVE-12 那种只有 `summary`、`node_id`、`title`、`goal`、验收项文字的计划，经宿主补全后通过完整校验并建出任务。
  - 约束目录有 2 条时，模型不写任何约束字段，入库的任务说明和工作说明都带着这 2 条原文。
  - 现有 Fake 脚本回归不变。
- 产品行预算：300。

**P1b 其余工具的定义与说明**

- 位置：`schemas/` 下的 `submission`、`experience_report` 两个 Schema；`src/tool_descriptions.hpp`。
- 做法：
  1. `submit` 不再展示 `used_memories`；`report_experience` 不再展示 `memory_assessments`，由宿主补空数组。
  2. 说明里写明宿主会强制的规则，并给出取值样例：

     | 工具 | 说明里要写明 |
     | --- | --- |
     | `submit_plan` | 每个独立的交付物一个节点；节点之间不能有依赖；验收项写成可以逐条核对的句子 |
     | `issue_work_order` | 步骤写成 Worker 可以直接执行的动作；文件路径相对于工作区根 |
     | `check_submission` | 判通过时：不带 `work_order`；`criteria_results` 覆盖每一条验收项且都是 `yes`。Worker 的自检有非 `yes`、验证记录有 `fail` 或 `not_run`、`known_issues` 非空时，不能判通过。判不通过时：必须带一份完整的 `work_order` |
     | `accept_candidate` | 判通过时：`criteria_results` 覆盖每一条验收项，都是 `yes`，每条写证据 |
     | `submit` | `deliverables[].path` 用 `/work/` 开头的完整路径；`self_check` 覆盖每一条验收项，编号形如 `ac1`；全部满足才提交，做不到的用 `ask`；`verification` 只列实际做过并且通过的检查 |
     | `report_experience` | 没有新经验时 `no_new_experience` 为 `true`，`entries` 为空 |
     | `fs_write`、`fs_patch` | 父目录必须已存在，用 `fs_mkdir` 逐级创建；覆盖已有文件要带 `fs_read` 返回的 `sha256` |

- 验收：一个测试逐条核对上表：对每条规则构造一次违反它的调用，断言宿主返回的原因在该工具的说明文字里有对应的句子。
- 产品行预算：150（不含说明文字）。

### P2 校验失败时写明位置和原因

- 对应：LIVE-13。
- 位置：`src/schema.cpp:315-320`。
- 做法：校验时收集错误，消息里列出前 3 条，每条是 JSON 指针路径加原因（缺哪个必填字段、哪个属性不在定义里、类型或格式不符）。总长不超过 600 字节。不回显参数值，至多带 40 字节的片段。任务说明的错误路径带上 `nodes/<序号>/brief` 前缀。Store 抛出的规则错误消息本来就具体，原样使用。
- 验收：对 LIVE-12 的例子，返回的消息里有 `goal`、`acceptance_criteria/0` 这样的路径和原因；长度有界；现有断言“校验失败”的回归相应更新。
- 产品行预算：80。

### P3 把可纠正的范围补齐

- 对应：LIVE-14、LIVE-16、LIVE-21。
- 位置：`src/offline_runtime.cpp:1216-1252`；`src/correction_detail.hpp:76-97`；`src/protocol_codec.cpp:550-555`；`src/provider_profile.cpp:329, 373-376`；`config/examples/system.yaml:110`。
- 做法：
  1. **模型参数引起的规则错误可纠正。** `apply()` 里在任何效果产生之前，由 Store 的规则检查抛出的 `InvalidInput`，以及提交快照检查抛出的路径错误和容量错误（文件过大、数量过多），转成纠错反馈，原因用异常消息。
  2. **只读工具的失败可纠正。** `fs_list`、`fs_read`、`fs_search`、`op_status`、`op_output` 和历史读取工具的失败都作为反馈交还模型。`Storage`、`Corrupt` 和频率限制除外。
  3. **写工具“效果未开始”的失败可纠正。** 操作状态是 FAILED，并且证据表明没有尝试改名、没有残留临时文件（`fs_write`、`fs_patch`），或目录没有创建（`fs_mkdir`）；以及 `run` 在启动进程之前被拒绝。
  4. **输出被截断可纠正。** 应答状态是 `incomplete` 且原因是输出上限时，这次应答不采用，下一次请求里加一句“上一次应答因输出上限被截断，请缩短输出，大文件分多次写入”。连续两次即停。
  5. `tool_choice` 的模式改为 `required`。给不支持允许列表的端点加一个取值 `required-v1`，只发 `"tool_choice": "required"`。
  6. `schema_retry_max` 的示例默认值改为 3。
  7. 凡是转成纠错反馈的失败，管理角色的下一次请求都要带上它，包括跨两次 `wls run` 的情形。
- 仍然必须停的：UNKNOWN、预算、取消、超时、存储错误、不变量被破坏、非只读工具的权限错误、同一错误重复达到 `stall_same_error_repeats`。
- 验收：下面四个场景各一个回归，都在一条命令里自动纠正后完成；七类必须停的错误各一个回归仍然停。
  - 检查时误判通过，收到具体原因后改判。
  - `fs_write` 的父目录不存在，`fs_mkdir` 之后重写成功。
  - `fs_list` 列一个不存在的目录，改路径后继续。
  - 请求体里 `tool_choice` 的 `mode` 是 `required`。
- 产品行预算：250。

### P4 失败调用的记账、按次预留、有界重试

对应：LIVE-15、LIVE-19、LIVE-20、LIVE-25。规则见 `02-decisions.md` §3.3 和 §4。分两个 PR。

**P4a 记账与预留**

- 位置：`src/store_live.inc:151-203, 436-447`；`src/store.cpp` 的 `settle_model_attempt`；`src/provider_profile.cpp:230-268`；`src/offline_runtime.cpp:810-837`；`src/event_runtime.cpp:673-688`；`src/run_cli.inc:273-298`。
- 做法：
  1. 估算函数对任何状态的应答都可用，只要输入和输出 token 有值。
  2. 缓存维度与价格配置对不上时取上界并产生一条警告，不再返回“不可用”。
  3. 已结算但没有完整用量的调用，账本把它的预留额转为已花费，这次调用的账目视为完整。原始用量列保持为空。
  4. 冻结了单价时，`capture_call` 按本次请求体和本次最大输出计算预留额。`wls run` 的启动检查改为“预算能覆盖一次这样的预留”。
  5. 以上只在“冻结了单价且没有签名收据挑战”时生效。有签名收据的路径不变。
- 验收：
  - 合成端点依次返回 429、503、带用量的 `incomplete`、中途断开。每种情形下请求都不进入 `needs_accounting`，账本的已花费等于规则给出的数，摘要里有对应的说明。
  - 价格配置写“缓存写入不适用”而应答带了写入数：请求继续，摘要里有价格配置警告。
  - `tests/test_billing_baseline_e2e.py:1074-1108` 按新规则更新：中断后重跑可以继续。
  - 签名收据的现有回归（`test_90` 至 `test_95`）不变。
  - 以每百万 token 输入 1.25 美元、输出 10 美元、窗口 40 万、最大输出 12.8 万的配置，1 美元预算可以启动。
- 产品行预算：300。

**P4b 重试与请求头**

- 位置：`src/event_runtime.cpp` 的 `invoke`；`src/config_loader.cpp:536`；`src/https_transport.cpp:780-787`。
- 做法：`model_calls.max_paid_retries` 允许 0 到 2。用传输记录里的 `upload_may_have_occurred`、`http_status`、`transfer_complete` 判定是否属于可重试的两类。退避用现有的可取消等待。其余失败停批。请求加 `User-Agent: wls861/<版本>`。
- 验收：429 一次后成功，同一条命令完成，记录 2 次调用；连续 3 次 429，停，原因写明“限流，已重试 2 次”；超时不重试，停，再次执行可以继续。
- 产品行预算：200。

### P5 路径规范化，只含文件工具的模板

- 对应：LIVE-17、LIVE-18。
- 位置：`src/worker_tools.cpp:160-203`；`config/examples/functions/general/`。
- 做法：
  1. `fs_*` 的 `path` 参数在校验之前规范化：`/work` 变成 `.`，`/work/x` 变成 `x`。其他绝对路径拒绝，消息写明“路径相对于工作区根，或以 /work/ 开头”。入库的操作参数是规范化之后的值。
  2. 示例功能新增团队模板 `files`，只授予 `fs_list`、`fs_read`、`fs_search`、`fs_mkdir`、`fs_write`、`fs_patch`、`ask`、`submit`。它的环境模板只写三件事：这个环境只有文件工具，不能运行命令；路径相对于工作区根；目录用 `fs_mkdir` 逐级创建。把 `files` 设为默认模板，原来带 `run` 的模板改名 `shell`，留给 G4。
  3. Worker 的功能提示词去掉“成果放 /work/out、中间文件放 /work/tmp”，改为“除非任务指定了目录，成果直接写在工作区根”。
  4. 加一个测试：环境模板里列出的每个程序，只有当 `run` 在该模板的工具集里时才允许出现。
- 验收：Worker 用 `/work/a.md` 调 `fs_write` 成功，落盘为 `a.md`；默认模板下发给模型的工具集不含 `run`、`op_status`、`op_output`、`op_cancel`、`note`、`ack_change`；系统文本里不出现 bash、python3、git。
- 产品行预算：60，另加配置与提示词。

### P6 运行摘要可读，分段统计，按原文件名导出

- 对应：LIVE-23，以及 CTX-8、CTX-9 的测量。
- 位置：`src/run_cli.inc:194-237, 358-372`；`src/event_runtime.cpp:521-528, 870-890`。
- 做法：
  1. 停下来时用一句话写明：哪个角色、哪个任务、错误码、原因、建议的下一步（再执行一次、改配置、回答问题、核对文件效果）。原因文本先过滤凭据（Authorization 头、密钥环境变量的值），其余照实写。
  2. 按角色给出：调用次数、纠正次数（按原因分）、重试次数、输入 / 缓存读取 / 输出 token、估算费用。
  3. 给出请求体的分段字节数，按角色取平均：工具定义、系统文本、契约（含更新）、历史、本次状态。
  4. 交付完成后，在请求目录下导出 `output/<节点>/<原相对路径>`，每个文件按交付清单的哈希核对。交付包本身不变。
- 验收：人为制造一次停止，摘要里能读到原因和下一步；G1 门禁场景的摘要带分段统计；`output/` 下的文件名与 Worker 提交的路径一致，内容与交付包一致。
- 产品行预算：250。

### P7 G2 预演门禁：朴素模型剧本

- 对应：蓝图修订 A3。只加测试。
- 位置：`tests/test_billing_baseline_e2e.py` 的合成端点，注册为独立的 `g2_rehearsal_e2e`。
- 做法：在 G1 门禁场景上加一份剧本，模拟真实模型第一次接触这套系统时的典型行为。端点先给出“朴素”的应答，再给出改正后的应答。测试断言第二次请求里带着足以让模型改正的信息。

  | 序号 | 朴素行为 | 断言 |
  | --- | --- | --- |
  | 1 | 计划里的 `brief` 只有 `goal` 和验收项文字 | 一次通过 |
  | 2 | 验收项写成字符串数组 | 被拒；下一次请求里有字段路径和原因；改正后通过 |
  | 3 | `fs_write` 写 `/work/notes/a.md`，目录不存在 | 路径被规范化；因父目录不存在失败；下一次请求里有原因；`fs_mkdir` 后重写成功 |
  | 4 | `fs_list` 列一个不存在的目录 | 可纠正，不停批 |
  | 5 | TeamLead 在 Worker 自检有 `partial` 时判通过 | 被拒，原因具体；改判不通过并带完整返工说明；返工后通过 |
  | 6 | 端点返回一次 429 | 自动重试成功，记 2 次调用 |
  | 7 | 端点返回一次带用量的 `incomplete` | 按用量记账；作为可纠正失败继续 |
  | 8 | 端点中途断开一次 | 停，摘要写明原因；再次执行同一条命令后继续并完成 |

- 门禁：除第 8 项需要再执行一次命令外，全程无人介入，最终状态 DELIVERED；摘要里的纠正次数、重试次数与剧本一致。
- 测试行预算：500。

### P8 已知类型输出项里的未知字段

- 对应：LIVE-24，D-4 补充。
- 位置：`src/responses_compat.hpp:81, 115, 157` 及内容片段处的白名单检查。
- 做法：`response_metadata_policy` 启用时，`message`、`function_call`、`reasoning` 项及其内容片段里的未知键不再报错，键名记入已有的 `unknown_fields`。已知键的类型和取值检查不变。未知的项类型仍然拒绝。项在下一次请求里照旧原样回传。
- 验收：官方样例的三种项各加一个未知键，应答被接受，键名被记录，回传的字节与原项一致。
- 产品行预算：60。

## 二、G2-7 候选

第一份真实报告出来之前不做。报告出来以后，从下表里挑对成功率或费用影响最大的，最多 3 个。

| 编号 | 内容 | 对应 | 什么情况下值得做 |
| --- | --- | --- | --- |
| E-1 | 工具结果瘦身：给模型的结果只留状态、路径、哈希、大小、错误原因，不带 `operation` 行、参数和宿主证据 | CTX-8 | 分段统计显示历史段占比高 |
| E-2 | 本次状态瘦身：只留本次动作的目标、允许的工具、剩余返工次数、未答问题、未决操作 | CTX-9 | 分段统计显示状态段超过 2 KB |
| E-3 | 按实测校准 token 估算：用已结算调用的“输入 token ÷ 请求体字节”加 20% 余量代替字节数，用于折叠阈值。费用预留仍用字节上界 | CTX-10 | 出现过早折叠 |
| E-4 | 缓存写入计费与 `prompt_cache_options`；GPT-5.6 之前的型号加 `prompt_cache_key` | CTX-11 | 第二轮换用 GPT-5.6 及以后的型号之前 |
| E-5 | 按真实错误率改写工具说明 | LIVE-22 | 某个工具的参数错误率明显偏高 |
| E-6 | 验证记录里的 `not_run` 是否应该阻止通过 | LIVE-22 | 报告里出现由它引起的返工 |

## 三、并入后续阶段的调整

| 阶段 | 增加的内容 | 对应 |
| --- | --- | --- |
| G4-C | `op_output` 对 UTF-8 输出返回文本；`run` 的结果直接带输出的前 8 KB | TOOL-6 |
| G4 | `shell` 模板启用 `run`；环境卡片实际执行检查 | LIVE-18 |
| G5 | 新适配器从一开始就按蓝图修订 A1 做，各带一份朴素模型剧本 | A1、A3 |
| G9 | 墙钟不计等待用户回答的时间 | ARCH-6 |
| G9 | 交付包内按原文件名存放，替代 P6 的导出副本 | LIVE-23 |
