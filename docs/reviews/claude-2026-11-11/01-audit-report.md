# 01 审核报告：代码、蓝图与在途工作

- 审核日期：2026-11-11（Asia/Shanghai）
- 审核对象：`main` @ [`4987f2c`](https://github.com/wmqfl861/wls861/commit/4987f2c6a6fc0da4b91e53d3689019ba3f0e8631)（0.11.1-r3.3c-audit，蓝图 v0.4.1，Store 410），以及 11-08 至 11-11 的全部在途 G1 分支与 PR #48/#49/#50
- 需求依据：仓库内的用户原话 T01–T21（`docs/handoff/v0.3/03_聊天档案/`）与需求基线 A1–F1（`docs/spec/v0.4.1/05_需求追踪.md`）
- 本轮没有修改任何产品代码、测试、配置或数据库

## 0. 证据等级（全文沿用）

| 标记 | 含义 |
| --- | --- |
| 【实测】 | 本次审核在审核环境中实际运行得到 |
| 【静态】 | 阅读 `main@4987f2c` 源码得到，给出 `文件:行号`，未运行复现 |
| 【记录】 | 取自仓库文档、PR 正文或 GitHub 接口，本轮未独立复核 |

**本轮实际运行的内容【实测】**（root 用户，g++ 13.3，Python 3.13）：M1 内核严格编译通过；`journal_tests` 10/10、`config_tests` 5/5、`scheduler_tests` 9/9、`task_engine_tests` 28/28；`wls861 demo` 正常；`demo_smoke` 3/3；`contracts_tests.py` 38 项通过；`verify_handoff.py` 与 `verify_review_source.py` 通过。

**本轮没有运行的内容**：S1 库 `libwls_s1`、`wls` 命令行及其全部测试（78 项注册测试中的绝大部分）。原因是审核环境缺 Boost 与 libcurl 开发头文件，且该环境不允许安装软件包。因此本报告对 S1 的全部结论都是【静态】。另外项目要求测试以非 root 用户运行，本轮实测是 root 身份，只能说明内核逻辑通过，不能替代项目自己的门禁。

## 1. 总体结论

1. **代码工程质量是好的。** 严格编译选项（`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`）、无危险 C 函数、SQL 取值全部参数绑定、文件工具用 `openat2` 限根、凭据只从环境变量读、状态变更带 CAS 与代次围栏。本轮没有发现可直接利用的安全漏洞。
2. **产品离“能用”还很远。** 自 10-17 开工约三周半、版本到 0.11.1，真实模型调用仍是 0 次。用户最看重的几件事——真实端点、高缓存率、并行、能跑命令的工具、MCP/Skill、记忆、Agent 互调——要么没有代码，要么现有设计恰好妨碍它们。
3. **最大的问题不在代码缺陷，而在方向和流程。** 11-06 外审后制定的止损规则只执行了三天：G0 完成后，G1 又发散成 13 个分支、3 个长期 Draft PR、0 合并，其中两个 PR 的 CI 早已全绿却没人合并，另一些分支陷在“配对复现、硬审、资源采样”的证据流程里。
4. **本轮新发现三类此前没有登记的问题**，都直接挡在“真实模型好用”前面：请求布局使供应商缓存几乎不可能命中（§4）；模型拿到的工具没有任何说明文字、提示词引用不存在的工具、环境卡片写着 `adapter: fake`（§3）；`run` 工具的沙箱设计跑不了 Python、git 这类真实工具链（§5）。

## 2. 需求符合度

判定：✅ 已实现（注明验证方式）；🟡 部分；❌ 未实现。

| 需求 | 用户原话出处 | 现状 | 判定 |
| --- | --- | --- | --- |
| A1 C++；Linux 先，Windows 后 | T01 T02 | C++20、Linux；Windows 冻结 NOT_RUN | 🟡 符合当前排序 |
| A2 能用现有 MCP、Skill、插件 | T01 | 只有规格，无代码 | ❌ |
| A3 能调用别的 Agent，也能被调用 | T01 T08 | 无代码 | ❌ |
| A4 支持主流模型端点 | T06 | 只有 OpenAI Responses 文本/function_call 子集；真实调用 0 | ❌ |
| A5 通用多 Agent，同一结构用于所有功能 | T04 T06 | 功能模板机制已有，只有 `general` 一个模板 | 🟡 |
| A6 方便配置单一功能 Agent | T02 | YAML 可配；但运行要约 40 个子命令加手写授权文件 | 🟡 |
| B1 四角色、一对一团队 | T03 T09 | 已实现，Fake 与本机合成 TLS 验证 | ✅ 未经真实模型 |
| B2 Root 不过滤直接交 Dispatcher | T09 | Root 为纯代码准入 | ✅ |
| B3 只有 Worker 执行 | T02 | Store 门禁强制 | ✅ |
| B4 异步接收和反馈 | T02 | 有持久 inbox；由命令行同步驱动，无常驻服务 | 🟡 |
| B5 中途改方向 | T03 | AMEND 逐任务 update/keep/cancel，含模型处置（Fake） | ✅ 未经真实模型 |
| B6 多请求多团队不乱 | T03 T04 | 版本、CAS、代次、幂等齐全，是项目最强的部分 | ✅ |
| B7 并行、快 | T09 | 事件泵单线程串行；并发相关配置全部只校验不执行 | ❌ |
| B8 全部完成并审核后统一交付 | T09 | TeamLead 检查、Dispatcher 验收、交付包与回执已有；独立审核团队未实现 | 🟡 |
| B9 交付后删除记录、停进程、清空间 | T03 T09 | 受控工作区可清理但默认关闭；数据库私有行不清；全局清理停在 PENDING | 🟡 |
| B10 Agent 互调与工作流 | T08 | 只支持单节点或独立多节点计划；无 DAG、无能力调用 | ❌ |
| C1 各角色模型与思考等级单独设置 | T05 | 配置解析与请求冻结已实现 | ✅ |
| C2 高缓存率 | T06 | 只有指纹；实际请求布局让缓存基本失效（§4） | ❌ |
| D1 默认工具与通用基础包 | T10–T12 | 文件工具可用；`run` 实际不可用；环境检查未执行 | 🟡 |
| E1–E9 经验、Skill、统一记忆 | T13–T20 | 规格与 Schema；经验报告作业已有；无记忆引擎 | ❌（E2 为 🟡） |
| F1 单用户本机 | 工程默认 | 是 | ✅ |

需求基线共 29 条（E1–E9 在表中合并为一行）。标 ✅ 的 7 条集中在角色边界、“不乱”和配置继承，其中 2 条还没有经过真实模型验证；用户在 T01、T06、T09、T13–T20 反复强调的端点、缓存、并行、生态（MCP/Skill/互调）、记忆这五组全部未达成。

## 3. 真实模型闭环（LIVE）

| 编号 | 级别 | 发现 | 证据 | 与既有登记的关系 |
| --- | --- | --- | --- | --- |
| LIVE-1 | P0 | 响应里没有金额，费用恒为未知，首次调用后即 `needs_accounting` 停批 | 【静态】`src/protocol_codec.cpp:174-176` | G1-1；PR #49 已实现按冻结单价估算，CI 绿，未合并 |
| LIVE-2 | P0 | 推理项、拒绝项、模型别名不被接受；系统指令被包成 JSON 字符串 | 【静态】`protocol_codec.cpp:185-241, 309-311, 377-378` | G1-2；PR #48 已实现，六配置 CI 绿，未合并 |
| LIVE-3 | P0 | **发给模型的工具没有 `description`**，只有名字和参数 Schema。模型要靠名字猜 `fs_patch` 的 `offset/expected/expected_sha256` 怎么填 | 【静态】`src/structured_context.cpp:114-118`、`src/worker_tools.cpp:138-147`、`protocol_codec.cpp:280-285` | 新发现；G1 分支未处理 |
| LIVE-4 | P0 | **提示词与实际工具不一致**：`prompts/dispatcher.md` 让模型用 `set_amendment_disposition`、`accept_task`、`deliver`，实际声明的是 `submit_amendment_plan`、`resolve_amendment`、`accept_candidate`；`prompts/worker.md` 谈 ack、幂等键、`memory_id`、`/input`，Worker 并无对应工具。真实路径的环境卡片固定写 `"adapter":"fake"`；规则段是审计口吻的否定句（“工具成功不代表真实模型用量”等），不告诉模型怎么把事做完 | 【静态】`prompts/*.md`；`src/offline_runtime.cpp:76-81, 189, 195-197, 498-520, 575-579`；真实路径同用此函数见 `src/event_runtime.cpp:610` | 新发现 |
| LIVE-5 | P1 | 响应的每一层都用白名单严格拒绝未知字段。供应商任何一次增加字段，都会让已经付费的响应整体作废；叠加“不自动重试、出错停批”后非常脆 | 【静态】`protocol_codec.cpp:35-39, 143, 151, 156, 198, 205, 212, 367-372` | 与 G1-2 “未声明字段仍被拒绝”的决定相反，需 owner 取舍（决定 D-4） |
| LIVE-6 | P1 | 每个响应必须恰好一个工具调用；只回文字、参数不合 Schema、读不存在的文件都会停整批 | 【静态】`offline_runtime.cpp:770` | G1-3；`feat/g1-correction` 有 304 行实现，卡在验证流程 |
| LIVE-7 | P1 | 域名解析要求 c-ares 或手工 IPv4 映射，Ubuntu 24.04 自带 libcurl 两者都不满足；代理被强制禁用 | 【静态】`src/https_transport.cpp:682-684, 703-705` | G1-5；在途方案是 576 行的独立解析子进程加 3625 行测试（PR #50），过重 |
| LIVE-8 | P1 | 请求级墙钟、80% 预警、停滞检测只校验不执行；每批最多 64 次调用、管理角色每次唤醒 12 次，稍大的请求要人工多次执行 | 【静态】`src/config_loader.cpp:975-990`、`src/live_cli.inc:72-74`、`event_runtime.cpp:187` | G1-6；在途实验分支相对 main 已有 4931 行产品改动，硬审结论 FAIL/OPEN |
| LIVE-9 | P1 | 只有 Responses 一种协议。Chat Completions、Anthropic Messages、Gemini 都没有 | 【静态】`src/provider_profile.cpp:198`、`src/live_model.cpp:113-114` | 原排在 S4；对 A4 是硬缺口 |
| LIVE-10 | P2 | 跑一次真实请求的步骤：`submit` → `live-status` 取模板 → 手工填请求号、配置哈希、每个模型 profile 的哈希 → 文件放到工作区外 → `pump-live`，超限再来一遍 | 【静态】`src/live_cli.inc:20-127` | 与 A6“方便”相悖 |
| LIVE-11 | P2 | 每次调用新建 TCP 与 TLS 连接，不复用 | 【静态】`https_transport.cpp:711-712` | 影响延迟 |

## 4. 上下文与缓存（CTX）——对应需求 C2

供应商缓存只认“请求开头逐字节相同的那一段”。OpenAI 当前文档写明：缓存顺序是工具定义 → 指令 → 对话历史；要把静态内容放前面、动态内容放最后；历史只追加不改写；工具集要保持稳定，要限制可用工具应使用 `tool_choice`/`allowed_tools`，不要删改工具定义（见文末来源）。现实现逐条相反：

| 编号 | 级别 | 发现 | 证据 |
| --- | --- | --- | --- |
| CTX-1 | P0 | 任务契约里混着每次调用都会变的数据（预算余额、`captured_state_version`、待办事件、任务列表、等待状态），而且整个契约被放在 `input` 的第一条。结果是第一条消息每次都不同，它后面的全部历史永远无法命中缓存 | 【静态】`offline_runtime.cpp:425-447, 473-492`；`protocol_codec.cpp:292-294` |
| CTX-2 | P0 | 管理角色每次调用只声明“当前这个动作”的工具，工具集随动作变化。工具定义排在缓存前缀的最前面，它一变，连系统指令的缓存也一起失效 | 【静态】`offline_runtime.cpp:498-520` |
| CTX-3 | P0 | 每个历史回合的来源元数据（attempt 记录、哈希、ID）被汇总进第一条消息的 `message_sources`。它随回合数线性增长，既改变前缀又白白消耗 token | 【静态】`src/structured_history.cpp:69-75`；`protocol_codec.cpp:286-294` |
| CTX-4 | P1 | `window-v1` 是滑动窗口，只内联最近 1–8 次（示例为 4 次），每回合丢掉最旧一项，前缀再次变化；更早的内容要模型自己用 `history_page`（每页最多 4 项）和 `history_read`（每次最多 4096 字节 base64）逐次付费取回，提示词还要求模型“解码 base64 并核对 SHA” | 【静态】`src/worker_history.cpp:45-56, 91-93`；`offline_runtime.cpp:632-635` |
| CTX-5 | P1 | token 浪费：`fs_read` 同时返回 `content` 与 `content_base64`；任务契约是 JSON 套 JSON 的字符串；系统指令里带一串依赖哈希 | 【静态】`src/workspace_posix.cpp:743-746`；`protocol_codec.cpp:292-294, 309-311` |
| CTX-6 | P2 | 没有 token 估算，输入按字节数预约；输出上限写死 8192 | 【静态】`offline_runtime.cpp:659, 662-664` |
| CTX-7 | P2 | 最简单的一个任务也要至少 7 次模型调用（计划、说明、写文件、提交、检查、验收、经验），每次都全量重发上下文 | 【记录】README“运行当前验收与真实本地交付”一节 |

CTX-1、CTX-2、CTX-3 叠加的后果：Worker 的缓存最多命中“工具 + 系统指令”，管理角色经常连这一段也命中不了。蓝图 §7 的“稳定前缀”只落实到了本地指纹，没有落实到真正发出去的字节顺序。G1-4 在途分支没有改动这一布局（已核对 `feat/g1-context-testclosure` 的 `protocol_codec.cpp`，`message_sources` 仍在第一条）。

## 5. 工具与沙箱（TOOL）——对应需求 D1

| 编号 | 级别 | 发现 | 证据 |
| --- | --- | --- | --- |
| TOOL-1 | P0 | `run` 只能看到清单里逐个登记的文件：最多 128 个普通文件，逐个核对 SHA 后以 `--ro-bind-data` 注入，不支持挂载目录。Python 标准库、git、编译器都是成百上千个文件，无法运行。`env.yaml` 列出的 bash、git、ripgrep、python3、pandas、jq 实际都用不了 | 【静态】`src/sandbox_posix.cpp:528, 1567-1583`；`docs/implementation/S1-worker-tools.md` 第 101、106 行 |
| TOOL-2 | P0 | 强沙箱的正向验收从未做过，G1/G2 的验收任务也都限定“只用文件工具”。照现路线，首个真实闭环完成后 Worker 仍然不能执行任何命令 | 【记录】`docs/STATUS.md` 能力矩阵“文件/进程”行 |
| TOOL-3 | P1 | 网络固定 `--unshare-net`，没有可配置的放行档；`/input` 是空目录 | 【静态】`sandbox_posix.cpp:1568, 1571` |
| TOOL-4 | P2 | `fs_write` 单次内容上限 64 KiB，`fs_read` 每页 64 KiB；一次提交最多 8 个文件、每个 64 KiB、合计 256 KiB。对代码类任务偏紧 | 【静态】`src/worker_tools.cpp:139, 142`；`offline_runtime.cpp:94-112` |
| TOOL-5 | P2 | 功能模板允许声明 `progress`、`ack_change`、`recall`、`load_memory`，但没有实现 | 【静态】`config_loader.cpp:756`、`config/examples/functions/general/function.yaml` |

说明：蓝图 §9.6 写的是“挂载只读系统运行文件”，实现（ADR-040）比蓝图严得多。逐文件封存适合高保障场景，不适合作为通用 Agent 的唯一档位。

## 6. 架构（ARCH）

| 编号 | 级别 | 发现 | 证据 |
| --- | --- | --- | --- |
| ARCH-1 | P1 | 事件泵单线程；Store 以整库 `flock` 独占写者，内部一把全局互斥锁。同一个库不能有两个进程同时推进，配置里的并发上限全部是 `validated_only` | 【静态】`src/store.cpp:488, 1726`；`config_loader.cpp:986-990` |
| ARCH-2 | P1 | `store.cpp` 311 KB，另有 6 个 `.inc` 片段共用一个实现类；旧构造函数要逐个传入 10 段 SQL（另有基于迁移注册表的新构造函数）。迭代成本很高：在途报告记录一次只含两个测试目标的 Release 构建 530 秒仍未完成，本地全量测试 2056 秒（约 34 分钟），CI 单个作业的超时上限是 40–55 分钟 | 【静态】`include/wls/store.hpp:95-106`、`.github/workflows/ci.yml`；【记录】`g1-billing-validation.md` 与 `g1-controls-owner907-status-20261111.md` 中的计时 |
| ARCH-3 | P2 | 两套内核并存：M1 的 Journal `TaskEngine`（`wls861`，0.1.1-m1）与 S1 的 SQLite Store（`wls`）；两套构建系统（Make 与 CMake）各维护一份清单；还有一套 Python 参考实现 `db/contract_store.py` | 【静态】`Makefile`、`CMakeLists.txt`、`src/main.cpp:21` |
| ARCH-4 | P2 | 签名计费是项目自定义协议，现实中没有对端实现；PR #49 合并后它不再是真实调用的前提 | 【静态】`src/billing.cpp`、`docs/implementation/R3-signed-billing.md` |
| ARCH-5 | P3 | 版本号写在三处：`src/cli.cpp:316, 463`、`src/main.cpp:21`、`CMakeLists.txt:2` | 【静态】 |

## 7. 流程与治理（GOV）

| 编号 | 级别 | 发现 | 证据 |
| --- | --- | --- | --- |
| GOV-1 | P0 | G1 再次发散。11-08 以来 13 个分支、0 合并。PR #48 在 11-08 已六配置全绿、可干净合并，至今仍是 Draft；PR #49 三核心配置绿；两者都超过了项目自定的“48 小时内决定” | 【记录】GitHub 接口，2026-11-11 查询 |
| GOV-2 | P0 | 证据流程的成本高过产品本身。以 G1-6 为例：需求是“总墙钟、80% 预警、停滞检测”三个检查，在途分支相对 main 产品改动 4931 行、测试 7971 行，报告里是配对 RED/GREEN、162 个资源采样点、26 行运行矩阵，结论仍是 FAIL/OPEN，构建还因超时没跑完 | 【记录】`experimental/controls-partial-20261110-40f7e303` 分支的 `docs/reports/g1-controls-owner907-status-20261111.md` |
| GOV-3 | P1 | 分支堆叠：context 叠在 protocol 上，integration 叠 billing 加 protocol，test 分支再叠 integration。违反“PR 基础只能是 main 或一个待合并 PR”“在制不超过 3 个”“单 PR 产品代码不超过 800 行” | 【实测】`git log main..<branch>` 逐分支比对 |
| GOV-4 | P1 | 测试与文档的体量已超过产品：产品代码 26,630 行，测试 30,539 行；`docs/reports` 197 个文件 5.9 MB，其中 7 个压缩包、3 个 XML | 【实测】`git ls-files` 统计 |
| GOV-5 | P2 | 文档与事实不符：`docs/STATUS.md` 第 5、14–17 行与 `CHANGELOG.md` 的 Unreleased 段仍写 #46、#47 “进行中、未合并”，实际两者 11-08 已合并；`05_需求追踪.md` 停在 10-21 的 0.7.0；`ROADMAP.md` 没有 G 阶段 | 【静态】 |
| GOV-6 | P3 | 远端 104 个分支 | 【实测】`git ls-remote --heads` |

### 在途 G1 分支逐项现状【实测 + 记录】

全部基于 `main@4987f2c`，“产品行”指 `src/`、`include/` 的增删。

| 分支 | 提交数 | 产品行 | 测试行 | 状态 | 建议 |
| --- | --- | --- | --- | --- | --- |
| `feat/g1-protocol`（PR #48） | 3 | +704 | +1363 | 六配置 CI 绿，可合并 | **立即合并** |
| `feat/g1-billing`（PR #49） | 5 | +549 | +711 | 三核心 CI 绿，可合并 | 在 #48 之后变基、合并 |
| `feat/g1-integration-i1` | 8 | +1253 | +2515 | billing 与 protocol 的叠加 | 仅作 #49 变基时解决冲突的参考，不合并 |
| `test/g1-i1-native-capture` | 9 | 同上 | +3068 | 叠在 integration 上 | 不合并 |
| `feat/g1-correction` | 2 | +304 | +2908 | 自述 BLOCKED 快照 | 取产品改动，测试裁到三个场景，重开小 PR |
| `feat/g1-context`、`feat/g1-context-testclosure` | 5 / 13 | +1170（含 protocol 的 704） | +3681 | 运行验收未完成 | #48 合并后变基，自身约 470 行；与 §4 的布局重排合并设计 |
| `feat/g1-network` | 1 | +960 | +724 | 自述“17 个未验证文件的脏快照” | 放弃，改用简单方案 |
| `feat/g1-network-resolver`（PR #50） | 4 | +708 | +3625 | CI 绿；内容是独立解析子进程 | 关闭 PR，保留分支 |
| `feat/g1-controls` | 2 | +784 | +772 | 自述 BLOCKED 快照 | 放弃，按最小实现重做 |
| `experimental/controls-partial` | 3 | +4676 | +7971 | 硬审 FAIL/OPEN | 不合并，保留作参考 |
| `ci/g1-correction-*-v8`（2 个） | 各 1 | — | — | 流程用临时分支 | 不合并 |

## 8. 蓝图（BP）

| 编号 | 级别 | 发现 |
| --- | --- | --- |
| BP-1 | P0 | **优先级与用户需求倒挂。** 蓝图把 S1 定义成“一个端点的最小闭环”，却在 S1 里不断加深恢复、容量窗口、签名计费、证据链；用户最在意的并行（S2）、记忆（S3）、MCP 与第二端点（S4）、互调（S5）全在后面。照现路线，这些要等很久 |
| BP-2 | P1 | **蓝图里混着进度和历史。** `01_实施规格` 自称“描述目标而非版本进度”，但 §5.7、§7.2、§8.1、§9.x、§10.2a、§14.2、§17 大段写的是“某版本已落地什么、不证明什么”。54 KB 的正文很难当作执行依据 |
| BP-3 | P1 | **默认值过度防御，没有区分“安全不变量”和“可调默认”。** 例如付费重试恒为 0、任何错误停批、每次调用只给一个目标、逐文件封存运行文件。这些被当成与“只有 Worker 能执行”“UNKNOWN 不盲重试”同等级的铁律，结果是系统安全但不可用 |
| BP-4 | P1 | **缓存只规定了指纹，没有规定线上字节顺序**（见 §4）。§7.1 的布局次序没有落到适配器 |
| BP-5 | P2 | **缺少用户能感知的验收标准。** 现有验收全是内部不变量；没有“一条命令提交任务并拿到交付包”“三个角色用三个不同模型跑通”这类里程碑 |
| BP-6 | P2 | **清理语义与用户原话有差距。** T09 说交付后“删除所有记录并停止进程”；现实现默认不清理、私有行永久保留。蓝图 §10.3 的目标是对的，但被 S1.6 的保守实现替代了 |
| BP-7 | P2 | 阶段命名有四套：M1、S0–S5、R0–R4、G0–G3，外加 P1/P2/P3 级别和 P-01…P-05 待配置项，容易混淆 |
| BP-8 | P2 | 四角色的层级开销没有量化目标。ADR-011 允许模板化 Dispatcher 免模型规划，但没有落地为“快路径” |

蓝图里应当原样保留的部分：四角色职责边界（§4.1）、独立修订与应用（§5.2）、AMEND 显式处置（§5.7）、UNKNOWN 核对（§5.9）、预算预留与未知费用（§5.10）、交付与回执（§10.1–10.2）、思考语义与继承（§8.2–8.3）。这些是对用户“不能乱”要求的正确落实。

## 9. 做得好、应当保留的部分

- 状态一致性：请求、计划、任务、指令四套修订分开，状态变更带 CAS 和代次，迟到结果不能盖新版本。
- 副作用保护：先持久意图再执行；结果不可证明时标 UNKNOWN，必须核对后才能继续。
- 原始证据：每次尝试的原请求、原响应、原用量不可变，未知用量不填 0。
- 配置冻结：请求创建时冻结完整配置，角色逐字段继承 Root，已有请求不受后续配置变化影响。
- 文件工具：`openat2` 限根、整文件 SHA 覆盖保护、原子写入。
- 传输安全：证书与主机名校验恒开、不跟随重定向、凭据不入库不入日志。
- CI 分级：文档改动走轻量检查，PR 三配置，main 六配置。

## 10. 与 2026-11-06 外审的关系

11-06 外审的结论（投入结构失衡、零闭环、G0–G3 路线）本轮复核后仍然成立，G1-1 至 G1-6 六项仍是接入真实模型前必须解决的问题。本报告的增量是：

1. 核实了 G1 的执行现状，指出同一失败模式正在复发，并给出逐分支处置（§7）。
2. 新增 LIVE-3、LIVE-4、CTX-1 至 CTX-5、TOOL-1、TOOL-2。这些不解决，即使 G1 六项全部完成，真实模型的成功率、费用和可做的任务范围也会很差。
3. 对 G1-5、G1-6 给出比在途实现小一个数量级的方案（见 `04-fix-list.md`）。
4. 对蓝图给出按用户需求重排的版本（见 `03-blueprint-v0.5-proposal.md`）。

## 11. 审核范围限制

- S1 未在本环境构建和运行，原因见 §0。
- 26,630 行产品代码没有逐行读完。完整阅读的是协议编解码、上下文编排、历史窗口、真实调用授权与运行主路径；Store、沙箱、配置加载器、清理、交付按结构和关键路径抽查。未读到的部分可能还有本报告没有列出的问题。
- 外部事实（OpenAI 缓存规则、MCP 规范版本、libcurl 选项）在 2026-11-11 查证，实施时应再核对一次。

## 来源

- [OpenAI Prompt caching 指南](https://developers.openai.com/api/docs/guides/prompt-caching/index.html)
- [MCP 规范 2026-07-28 变更说明](https://modelcontextprotocol.io/specification/2026-07-28/changelog.md)
- [libcurl CURLOPT_QUICK_EXIT](https://curl.se/libcurl/c/CURLOPT_QUICK_EXIT.html)
- [Agent Skills 规范](https://agentskills.io/specification.md)
