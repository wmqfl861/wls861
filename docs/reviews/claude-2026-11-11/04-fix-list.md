# 04 修改与修复清单

每一项对应 `01-audit-report.md` 的发现编号，按 `05-development-plan.md` 的执行顺序排列。行号以 `main@4987f2c` 为准；PR #48、#49 合并后行号会移动，实施时以符号名定位。

通用约定：
- 每项一个 PR，直接基于 main；“产品行预算”是 `src/` 与 `include/` 的增删行数上限。
- 涉及持久化格式或提示词的改动只对**新请求**生效：用新的冻结取值（如 `request_format: structured-v3`）表达，旧请求按原格式继续。
- 数据库版本保持 410，除非条目明确写了需要迁移。
- 标 “需 D-x” 的条目在 owner 作出对应决定（`02-optimization-plan.md` §5）之前不实施。

## 一、立即收敛（阶段 0）

### F-01 合并 PR #48（G1-2 协议兼容）
- 对应：LIVE-2
- 现状：head `01c330d`，六配置 CI 全绿，可干净合并，产品改动 795 行，Draft 已 3 天。
- 做法：转为 Ready，按 D-1 的权限合并（普通 merge commit）。不追加任何改动。
- 验收：合并后的 main 六配置 CI 通过。

### F-02 合并 PR #49（G1-1 按冻结单价估算费用）
- 对应：LIVE-1
- 现状：head `834efcd`，三核心配置 CI 绿，产品改动 610 行。与 #48 同改 `provider_profile.{hpp,cpp}`，会有冲突。
- 做法：F-01 之后把分支变基到新 main；冲突解法参考 `feat/g1-integration-i1` 的提交 `0b97b5a` 与 `c7d5075`。PR 说明里“单次 pump 跑完 31 次调用”未达成属于 F-08 的范围，不在本 PR 解决。
- 验收：变基后 CI 通过；`tests/test_billing_baseline_e2e.py` 的 31 次调用用例通过。

### F-26 文档与事实对齐
- 对应：GOV-5、BP-2、BP-7
- 位置与改法：
  - `docs/STATUS.md` 第 5 行：main 基线改为当前提交；第 14–17 行“进行中”一节：#46、#47 移入已合并，写入当前在途 PR。
  - `CHANGELOG.md` Unreleased 段：去掉 #46、#47 的 “draft, not merged”。
  - `docs/ROADMAP.md`：在第 1 节前加一节“当前执行路线”，指向本目录的 `05-development-plan.md`。
  - `docs/spec/v0.4.1/05_需求追踪.md`：文首更新日期和版本，并加一句“当前符合度见 `docs/reviews/claude-2026-11-11/01-audit-report.md` §2”。
  - owner 采纳蓝图后：`03-blueprint-v0.5-proposal.md` 迁入 `docs/spec/v0.5/01_实施规格_v0.5.md`，`00_决策记录.md` 追加对应决策，`AGENTS.md` 的蓝图版本号同步。
- 验收：以上文件互相不矛盾；CI 的文档轻量检查通过。注意 `docs/spec/**` 的改动会触发三配置编译，属预期。

### F-27a 在途分支处置
- 对应：GOV-1、GOV-3
- 做法：按 `01-audit-report.md` §7 的表逐项处置。关闭 PR #50 并写明替代方案是 F-07；`feat/g1-network`、`feat/g1-controls`、`experimental/controls-partial`、`feat/g1-integration-i1`、`test/g1-i1-native-capture`、两个 `ci/g1-correction-*` 不再推进，分支保留不删。
- 验收：打开的 PR 不超过 3 个；没有以未合并分支为基础的 PR。

## 二、G1：接入真实模型前的改造（阶段 1）

### F-09 纠错回路（G1-3）
- 对应：LIVE-6
- 位置：`src/offline_runtime.cpp:770`（恰好一个工具调用）；`src/event_runtime.cpp` 的批次停止逻辑；`src/config_loader.cpp:984-986`（`schema_retry_max` 仅校验）。
- 做法：从 `feat/g1-correction` 取产品改动（约 304 行：`correction_detail.hpp`、`event_runtime.cpp`、`offline_runtime.cpp`、`workspace_posix.cpp`、`config_loader.cpp`），变基到新 main。测试只保留三个场景加一个回归，其余 2900 行测试不带入。
- 行为：只回文字、参数不合 Schema、调用未声明或本次不允许的工具、读不存在的文件、工具返回已结算失败——都把错误作为该次调用的反馈写入历史，下一次调用继续；同一目标连续失败达到 `schema_retry_max`（默认 2，建议改 3）后阻塞并上报。UNKNOWN、权限、预算、取消、超时、存储、不变量错误照旧立即停。
- 验收：Fake 脚本的三种错误都在下一次调用内被纠正且批次不中断；必须停的七类错误各有一个回归仍然停。
- 产品行预算：350。

### F-03 工具说明文字
- 对应：LIVE-3
- 位置：`src/worker_tools.cpp:138-147`（注册表）；`include/wls/context.hpp` 的 `ContextTool`；`src/structured_context.cpp:114-118`；`src/protocol_codec.cpp:280-285`；`src/offline_runtime.cpp:76-81, 498-520`（管理工具）；`src/worker_history.cpp:45-56`、`src/management_context.cpp`（历史工具）。
- 做法：
  1. `ContextTool` 增加 `description` 字段；注册表每个工具写 1–3 句说明：做什么、关键参数怎么取值、失败后该做什么。
  2. 参数 Schema 的每个属性加 `description`。
  3. 结构化前缀的工具定义带上说明（属于 `structured-v3` 的格式变化）；编码器把它写进协议的工具 `description`。
  4. 注册没有说明的工具时抛错。
- 说明文字示例（据实现核对后定稿）：
  - `fs_write`：“在工作区内创建或覆盖一个文本文件。新建文件不需要 `expected_sha256`；覆盖已有文件必须填 `fs_read` 返回的该文件完整 SHA-256，不一致会被拒绝，此时先重新读取。”
  - `submit`：“任务完成后提交成果。`deliverables` 列出 /work 下要交付的文件。提交后由 TeamLead 检查，在收到新说明前不要继续修改这些文件。”
- 验收：编码后的请求里每个工具都有非空说明；新增一个测试遍历注册表断言这一点。
- 产品行预算：250。

### F-04 提示词、环境卡片与规则段
- 对应：LIVE-4
- 位置：`prompts/common.md`、`prompts/dispatcher.md`、`prompts/team_lead.md`、`prompts/worker.md`、`config/examples/functions/general/prompts/*.md`；`src/offline_runtime.cpp:186-197, 568-635`。
- 做法：
  1. 重写四份提示词：以“你的目标、你能用的工具、标准步骤、遇到阻塞怎么办、完成的标志”为骨架，每份不超过约 400 token。工具名一律用反引号，并且只出现注册表里存在的名字。删掉 `set_amendment_disposition`、`accept_task`、`deliver`、ack、幂等键、`memory_id` 等没有对应工具的内容；记忆相关句子等 F-22 落地时再加回。
  2. 环境卡片的 `adapter` 取自该角色的模型条目，不再写死 `fake`（第 189 行）。
  3. 规则段改成正面指引。`checks_executed:false`、`process_isolation_verified:false`、“工具成功不代表真实模型用量”这类审计声明不再发给模型，需要的话留在证据里。
  4. 删除“解码 base64 并核对 SHA”的要求（第 634 行）；哈希核对由宿主完成（见 F-06）。
  5. 新增测试：提取提示词里反引号内形如工具名的标识符，断言每个都在注册表中或在一份显式的非工具词白名单中。
- 验收：上述测试通过；Fake 全流程回归通过；真实路径生成的系统文本里不再出现 `fake`。
- 产品行预算：200（不含提示词文件）。

### F-05 请求布局重排与工具集稳定
- 对应：CTX-1、CTX-2、BP-4
- 位置：`src/offline_runtime.cpp:425-492`（契约组装）、`498-520`（按动作替换工具）；`src/structured_context.cpp:108-143`；`src/protocol_codec.cpp:286-314`；`config/examples/system.yaml` 的 `context.request_format`。
- 做法：
  1. 新增冻结取值 `request_format: structured-v3`，示例配置的新请求默认用它。
  2. 把契约拆成两部分。**稳定契约**：请求原文、约束目录、任务正文、当前说明正文、验收项。**本次状态**：预算余额、`captured_state_version`、待办事件、任务状态列表、等待状态、`workflow_action`、未决操作、版本与代次。两部分都完整保存在持久化的逻辑请求里，宿主的 CAS 核对（`accept_captured_response`，第 734–741 行）照常从逻辑请求读取。
  3. 线上顺序：工具 → 系统文本 → 稳定契约 → 历史回合 → 本次状态。本次状态是最后一条 user 消息，不写入历史。
  4. AMEND 或新说明到来时，在历史末尾追加一条“契约更新”消息，不改写第一条。
  5. 工具集按角色固定：Dispatcher、TeamLead、Worker 各自声明完整工具集；`capture_call` 现在算出的“本次动作的工具”改为 `allowed_tools`。协议支持时写进 `tool_choice` 的允许列表（字段形状按当时的官方接口文档核对）；模型调用不在允许列表内的工具时按 F-09 的可纠正错误处理。
  6. 核对系统文本里是否有团队唯一的值（如 `execution_policy_sha256`，`src/worker_tools.cpp:352`），有则移到本次状态。
- 验收：
  - 新测试：同一 Worker 连续 5 次调用，编码后的请求体，第 N 次是第 N+1 次去掉“本次状态”后的严格前缀。
  - 新测试：Dispatcher 在计划、问答、验收三种动作下，请求体的工具段和系统文本段逐字节相同。
  - 本机合成端点按前缀匹配规则模拟缓存命中，Worker 第 3 次调用起的缓存读取占比超过 70%（前缀长度达到模拟的最小缓存长度时）。
  - `framed-v1`、`structured-v2` 的旧请求回归不变。
- 产品行预算：600，可拆成“布局”和“工具集稳定”两个 PR。

### F-06 历史与 token 减负
- 对应：CTX-3、CTX-4、CTX-5、LIVE-6 的一部分（G1-4）
- 位置：`src/structured_history.cpp:69-75`；`src/protocol_codec.cpp:286-294`；`src/worker_history.cpp`；`src/workspace_posix.cpp:743-746`；`src/store_history.inc:85-123`。
- 做法：
  1. `message_sources` 不再发给模型；来源信息只保存在逻辑请求里。模型侧需要指代历史记录时用回合序号。
  2. `fs_read` 给模型的结果：文本文件只带 `content`，二进制文件只带 `content_base64`。宿主内部取提交快照的调用（`offline_runtime.cpp:106-115`）继续拿 base64，不受影响。
  3. 新增历史策略 `append-fold-v1`（新请求默认）：历史只追加；估算输入超过上下文窗口的 `compaction_at_fraction` 时，把最旧的一半回合折叠成一条记录（回合范围、每回合的工具名与一行结果摘要、回读用的序号）。`window-v1` 保留给已冻结它的旧请求。
  4. `history_read` 返回 UTF-8 文本分页（附序号、`next_offset`），哈希由宿主核对后再返回；二进制才用 base64；单页上限按 token 预算配置（建议默认 16 KiB）。可从 `feat/g1-context-testclosure` 取对应实现。
  5. 稳定契约用纯文本或单层 JSON，不再 JSON 套 JSON。
- 验收：60 KB 的 UTF-8 旧结果不超过 3 次调用读完；折叠只在跨过阈值时发生一次，其后的调用仍满足 F-05 的前缀测试；同一任务的请求体字节数相对 `structured-v2` 明显下降（记录实测比例）。
- 产品行预算：500，可拆两个 PR。

### F-07 联网（G1-5，最小方案）
- 对应：LIVE-7
- 位置：`src/https_transport.cpp:682-684`（域名被拒）、`703-705`（代理禁用）、`743-747`（显式地址映射）。
- 做法：
  1. 域名解析：端点是域名且没有显式映射时，宿主在一个独立线程里调用 `getaddrinfo`，主线程最多等到请求截止时间和解析超时（默认 10 秒）中较早的一个，期间响应取消。成功则取第一个 IPv4 地址，走现有的 `CURLOPT_RESOLVE` 映射路径；超时或失败则在发送任何字节之前返回 `dns_failed`。超时后解析线程被放弃，它只持有自己的共享状态。不使用 c-ares，不引入解析子进程。
  2. 代理：传输选项增加可选的 `proxy_url`，来自外部配置；设置后启用 `CURLOPT_PROXY`，目标域名由代理解析，对目标的证书与主机名校验不变；证据里记录是否经过代理。默认仍不使用代理，也不读环境变量里的代理设置。
- 验收：在 Ubuntu 24.04 自带 libcurl 上，用域名 `localhost` 和本机自签证书完成一次 TLS 调用；解析超时的用例在截止时间内返回且未发送请求；经本机 CONNECT 代理的调用成功并保持证书校验。
- 产品行预算：200。测试不超过 300 行。

### F-08 付费保护与自动续批（G1-6，最小方案）
- 对应：LIVE-8
- 位置：`src/config_loader.cpp:984-986`；`src/event_runtime.cpp` 的 `live_continue`（约第 470 行）和 `manager_cap`（第 187、445 行）；`src/live_cli.inc:72-74`。
- 做法：
  1. 请求总墙钟：每次预约前比较当前时间与请求创建时间，超过 `budget_defaults.max_wall_minutes` 则不再预约，记录 `wall_clock_exceeded` 阻塞。用数据库里已有的创建时间戳即可。
  2. 80% 预警：每次结算后，任一预算维度首次达到 `warn_at_fraction` 时写一条 `budget_warning` 事件，每个维度只写一次。
  3. 停滞检测：泵里按“角色 + 任务”记录最近一次错误签名（错误码 + 工具名 + 参数哈希）及连续次数，达到 `stall_same_error_repeats` 时阻塞该任务并上报。
  4. 自动续批：一次授权内循环执行“唤醒 → 推进”，直到静止、阻塞、预算用尽或墙钟到点。管理角色每次唤醒的调用上限保留，到上限后自动开始下一次唤醒。每次命令的总调用数另设一个可配置的安全阀（建议默认 200）。
  5. 这三项从 `runtime_support()` 的 `validated_only` 列表中移除。
- 验收：三项各有一个回归；一个 3 任务请求在一次授权、一条命令内跑完；`wls config check` 不再把这三项报为仅校验。
- 产品行预算：300。不沿用 `feat/g1-controls` 与 `experimental/controls-partial` 的实现。

### F-11 宽容解码（需 D-4）
- 对应：LIVE-5
- 位置：`src/protocol_codec.cpp:35-39` 的 `only_fields` 及其在 143、151、156、367-372 行的调用；输出项内部的 198、205、212 行与 236 行；`src/sse_replay.cpp` 的同构检查。
- 做法：顶层和 usage 明细里的未知键不再报错，键名写入 `evidence["unknown_fields"]`；输出项内部字段保持严格；未知的输出项类型跳过并记录，若因此没有可用内容，按 F-09 的可纠正错误处理。原始字节照常保存。
- 验收：在官方样例上加一个未知顶层字段和一个未知 usage 明细字段，响应仍被接受并记录键名；未知输出项类型不触发工具派发。
- 产品行预算：150。

### F-10 一键入口 `wls run`
- 对应：LIVE-10、BP-5
- 位置：新增 `src/run_cli.inc`，在 `src/cli.cpp` 注册。
- 做法：`wls run --config FILE --text-file FILE [--constraints-file FILE] [--budget-usd N] [--data DIR] [--yes]`。依次完成：准入 → 为计划中的任务分配受控工作区 → 按 F-08 循环推进 → 发布交付包 → 打印摘要（状态、交付位置、各角色调用次数、四项 token、缓存占比、估算费用、耗时、阻塞原因）。真实调用时先打印将使用的模型、端点主机名和预算上限，`--yes` 或交互确认后继续；内部仍构造并校验现有的授权结构，Store 与运行时的检查一条不少。请求数据放在 `<data>/requests/<请求号>/`。
- 验收：对本机合成端点，一条命令跑完 3 个文件任务（即 G1 总门禁）；中途 Ctrl-C 后再次执行同一命令能从持久状态继续。
- 产品行预算：350。

### F-28a 输出上限与输入估算
- 对应：CTX-6
- 位置：`src/offline_runtime.cpp:659`（写死 8192）、`662-664`（按字节预约）。
- 做法：`max_output_tokens` 取模型条目的配置；输入预约按“字节数 ÷ 3”估算并在结算时按实际用量修正。
- 产品行预算：60。可并入 F-08。

## 三、G2 之后

### F-14 沙箱 `standard` 档（需 D-5）
- 对应：TOOL-1、TOOL-2、TOOL-3
- 位置：`src/sandbox_posix.cpp:528, 1535-1583`；`include/wls/sandbox.hpp`；功能模板的 `isolation` 与 `network` 字段。
- 做法：新增策略取值 `isolation: standard`。bwrap 参数骨架：
  ```
  --unshare-user --unshare-pid --unshare-ipc --unshare-uts --unshare-net
  --die-with-parent --new-session --cap-drop ALL --clearenv
  --ro-bind /usr /usr  --symlink usr/bin /bin  --symlink usr/lib /lib  --symlink usr/lib64 /lib64
  --ro-bind <白名单 /etc 文件，如 ld.so.cache、alternatives>
  --proc /proc --dev /dev --tmpfs /tmp --dir /tmp/home
  --bind <团队工作区> /work   --ro-bind <输入目录> /input
  --setenv PATH /usr/bin:/bin --setenv HOME /tmp/home --chdir /work
  ```
  模板声明 `network: host` 时去掉 `--unshare-net` 并在环境卡片和证据里标明。后代清理依靠 PID 命名空间。资源上限：有 cgroup 委派用 cgroup，没有则用 rlimit 加墙钟超时，并在证据里写明用的是哪一种。`strict` 档现有行为不变。`doctor` 报告两档各自是否可用及原因；Ubuntu 24.04 默认限制非特权 user namespace，不可用时给出说明，不自动修改系统策略。
- 验收（都用真实进程）：`python3 -c 'print(1)'` 成功；访问网络失败；写 `/work` 之外失败；读宿主 home 失败；宿主环境变量不可见；父进程退出后其后台子进程不存在；超时后进程被终止且结果按现有语义结算。
- 产品行预算：500，拆成“策略与参数”和“doctor 与模板字段”两个 PR。

### F-15 环境卡片、输入目录与体积上限
- 对应：TOOL-3、TOOL-4、D1
- 位置：`config/examples/functions/general/env.yaml`；`src/worker_tools.cpp:347-360`；`src/offline_runtime.cpp:94-112`。
- 做法：团队启动时在沙箱内执行 `env.yaml` 的检查命令，结果写入环境卡片并参与指纹；`run` 可见 `/input`；`fs_write` 单次内容上限、单次提交的文件数与字节数改为 `system.limits` 下的配置项，默认值适度放宽（建议单次写入 1 MiB、提交 32 个文件共 8 MiB）。
- 验收：卡片列出实际版本和缺失项；带 `/input` 的数据处理任务端到端通过。
- 产品行预算：300。

### F-16 普通单 Agent 循环与对照评测
- 对应：CTX-7、BP-8
- 做法：新增冻结策略 `single-agent-plain-v1`：一个模型、Worker 的全部工具、宿主生成的单任务计划，没有管理角色调用。扩展 `scripts/evaluate_r3.py` 增加该条件。评测用同一组任务、同一模型、同一预算，比较四角色、现有的角色代理单 Agent、普通单 Agent 的成功率、调用数、token、费用、耗时。
- 验收：一页报告；给出快路径（F-29）的调整建议。
- 产品行预算：300。

### F-29 快路径
- 对应：CTX-7、BP-8
- 做法：功能模板可声明固定的单任务计划，命中时不调用 Dispatcher 规划；TeamLead 检查通过时允许在同一响应里同时调用 `check_submission` 与 `report_experience`。权限与版本检查不变。
- 验收：单任务请求的固定调用从 6 次降到 4 次；检查不通过时行为与现在相同。
- 产品行预算：250。

### F-12 Chat Completions 适配器
- 对应：LIVE-9
- 位置：`src/provider_profile.cpp:198`、`src/live_model.cpp:113-114`（写死 `openai-responses`）；新增 `src/chat_codec.cpp`。
- 做法：抽出编解码接口（编码请求、解码响应、解析用量），现有 Responses 实现为其一。新增 `adapter: openai-chat`：系统文本进 system 消息；工具为 `{"type":"function","function":{name,description,parameters}}`；历史用 assistant 的 `tool_calls` 与 `tool` 消息配对；用量取 `prompt_tokens`、`completion_tokens` 及明细里的缓存 token。先做非流式，流式另开 PR。字段以实施时的官方接口文档为准，附官方样例回放测试。
- 验收：同一个 Fake 流程分别经两种适配器对本机合成端点跑通；U5。
- 产品行预算：两个 PR，各 ≤600。

### F-13 Anthropic Messages 适配器
- 对应：LIVE-9
- 做法：`adapter: anthropic-messages`；认证头、版本头按适配器配置；工具为 `{name, description, input_schema}`；内容块 `text`、`tool_use`、`tool_result`；在工具与系统文本末尾、稳定契约末尾、历史末尾放缓存断点；用量取输入、缓存写、缓存读、输出四项。
- 验收：同 F-12。
- 产品行预算：两个 PR，各 ≤600。

### F-17 请求级并行
- 对应：ARCH-1、B7
- 做法：`wls run` 已按 F-10 把每个请求放在独立目录。新增一个登记库 `<data>/registry.sqlite`（独立的小表：幂等键、请求号、目录、状态、回执），用于跨请求去重和列出请求；不改 Store。多个 `wls run` 进程可同时运行。实施前核对 `Store::admit` 的幂等范围在“每请求一库”下的行为。
- 验收：两个请求同时运行，各自交付正确，互不可见；重复提交同一幂等键返回原请求。
- 产品行预算：300。

### F-18 团队级并行
- 对应：ARCH-1、B7
- 位置：`src/event_runtime.cpp` 的泵主循环。
- 做法：计划提交后，每个就绪团队一个线程推进它的 TeamLead 与 Worker；Dispatcher 的事件处理留在主线程串行。先审计泵内共享状态（调用计数、结果数组、阻塞列表）并加锁；Store 每次调用已有互斥，模型调用与工具执行在锁外。并发上限取冻结配置，并把这些配置项从 `validated_only` 移除。
- 验收：本机合成端点每次调用延迟 2 秒，3 个团队的总耗时小于串行的一半；ASan、UBSan、TSan 下通过；运行中强杀后重开能恢复；U6。
- 产品行预算：两个 PR，各 ≤500。

### F-19 Store 拆分与构建提速
- 对应：ARCH-2
- 做法：引入内部头 `src/store_impl.hpp`，把 `store.cpp` 与各 `.inc` 按子域移到独立编译单元，每个 PR 只移一个子域，不改 SQL、事务范围和公开接口；预编译语句按 SQL 文本缓存；为 Boost.JSON 加预编译头；CI 启用 ccache；CTest 并行运行互不依赖的测试。
- 验收：每步之后全部测试通过；记录拆分前后的全量构建与增量构建时间。
- 产品行预算：纯移动不计入 800 行上限，但每个 PR 只做一个子域。

### F-30 测试分层
- 对应：GOV-2、GOV-4
- 做法：给测试打 `fast` / `slow` 标签；PR 门禁跑 `fast` 加受影响的 `slow`，全量留给 main、夜间和手动触发。目标是 PR 门禁 15 分钟内。
- 验收：PR 门禁实际时长有记录并达到目标。

### F-20 MCP 客户端
- 对应：A2
- 做法：新增 `src/mcp_client.cpp`：JSON-RPC 2.0；stdio 与 Streamable HTTP 两种传输；实现发现、`tools/list`、`tools/call`。以规范 2026-07-28 为准（无状态、无 `initialize` 握手、`server/discover`），对只支持旧握手的服务器做兼容；实施时按规范原文核对。服务器清单由功能模板声明（命令、参数、环境变量白名单），属于 owner 的受信配置。工具注册为 `mcp__<服务器>__<工具>`，默认“外部副作用、非幂等”，走现有的操作生命周期（先持久意图，结果不可证明标 UNKNOWN）。
- 验收：Worker 经本机一个示例 MCP 服务器完成一次工具调用；未授权的 MCP 工具对模型不可见；服务器崩溃时操作为 UNKNOWN；U7 的前半。
- 产品行预算：三个 PR，各 ≤600。

### F-21 Skill 加载
- 对应：A2、E3、E4
- 做法：读取 Agent Skills 格式目录（`SKILL.md` 的 YAML 头里有名字与说明）；模板固定加载的 Skill 把名字和说明放进系统文本，正文经 `load_skill` 工具按需读取；Skill 自带脚本经 `run` 在沙箱内执行，Skill 目录只读挂载。格式以实施时的规范原文为准。
- 验收：加载一个现成的 Skill 并在任务中用到它；U7 的后半。
- 产品行预算：400。

### F-22 记忆最小闭环
- 对应：E1、E2、E5、E6、E9
- 做法：按 `03-blueprint-v0.5-proposal.md` §9 第一步。新增 `MemoryEngine`（独立模块，同进程）：Git 目录存 Markdown，独立的 SQLite 文件存元数据、标签与全文索引；经验报告条目写入 `draft`；`recall`、`load_memory` 两个 Worker 工具；提交时的使用声明按版本统计；`wls memory list / show / approve / block`。实现现有模板里已声明的 `recall`、`load_memory`（见 TOOL-5）。
- 验收：第二次同类任务时相关经验以卡片出现并被加载，费用明细里能看到用了哪条；被 `block` 的记忆立即不可加载；U8。
- 产品行预算：四个 PR，各 ≤600。

### F-23 记忆演化
- 对应：E7
- 做法：同一问题多种方法并存；按验证结果调整权重；达到阈值后生成合并草稿，矛盾部分归档；撤销先作用于派发门禁。阈值为配置项。
- 产品行预算：三个 PR，各 ≤600。

### F-24 清理默认值（需 D-7）
- 对应：BP-6、B9
- 做法：回执确认且经验完成后，`wls run` 默认删除该请求目录下的工作区、输入副本和运行数据库；保留交付包、经验，并在登记库保留回执与费用摘要，默认 30 天。有未核清 UNKNOWN、未完成经验或未确认交付时不清理并说明原因。`--keep` 可保留全部。
- 验收：U10；三种不清理的情形各有一个回归。
- 产品行预算：250。

### F-25 服务化、本地 API、MCP 服务端
- 对应：B4、A3
- 做法：常驻进程按 v0.4.1 §12.2；MCP 服务端暴露 submit、status、amend、answer、get_delivery、ack、cancel，全部走 RootGateway；计划节点支持调用另一个功能。
- 验收：U9。

## 四、随时可做的小项

| 编号 | 对应 | 位置 | 改法 |
| --- | --- | --- | --- |
| F-27b | ARCH-5 | `src/cli.cpp:316, 463`、`CMakeLists.txt:2` | 版本号由 CMake 生成到一个头文件，命令行引用它 |
| F-27c | TOOL-5 | `config/examples/functions/general/function.yaml`、`src/config_loader.cpp:756` | 示例里去掉 `progress`、`ack_change`；`recall`、`load_memory` 在 F-22 之前也从示例去掉 |
| F-27d | GOV-4 | `docs/reports/` | 不再新增压缩包、XML、长日志；已有的不动 |
| F-27e | GOV-6 | 远端分支 | 需 owner 同意：已合并和已处置的分支先打 `archive/<分支名>` 标签再删除 |
| F-28b | LIVE-11 | `src/https_transport.cpp:711-712` | 同一批内对同一端点复用连接；排在 F-18 之后 |
| F-27f | ARCH-3 | `Makefile`、`CMakeLists.txt` | 两套构建清单择一为准，另一套由它生成或加一致性测试；M1 内核与 Python 参考实现保持现状 |
| F-27g | ARCH-4 | 签名计费 | 保持现状，不再扩展 |
