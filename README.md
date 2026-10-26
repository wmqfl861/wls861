# 无量山 · wls861

C++ 通用多 Agent 系统，Linux 优先。**当前补丁：0.8.6-r3.2；当前蓝图：v0.4.1。** 原 `wls861` 继续提供 0.1.1-m1 合成内核，`wls` 提供持久化 FakeModel 流程、受控 Worker 工具和独立离线协议回放。

本补丁修复独立审核确认的共享文件读取/doctor EOF、清理名称、CLI异常与SSE默认填充边界。
[审核处置](docs/reviews/independent-2026-10-26.md)、[实际验证](docs/reports/independent-audit-validation.md)
和[后继优化计划](docs/implementation/R3-controlled-live-plan.md)分别记录问题、证据与下一步；
当前仍未开放真实模型调用。

当前增量已接入请求限定恢复、拒绝成果纠正、模型逐任务 AMEND、有界 fs_mkdir 和运行时工作区分配恢复/明确弃置；保留原说明、成果、用量和未知副作用证据。
[当前能力/证据矩阵](docs/STATUS.md)集中记录实现边界；[审核采纳记录](docs/reviews/claude-disposition.md)
及[第二份审核处置](docs/reviews/claude-2026-10-24-disposition.md)区分已修复和开放问题。
冻结容量窗口及Root检查点已接入；近期优先可用模型闭环，不继续优先扩大私有数据清除。
R1.1 新增任务内的只读历史索引与原反馈分片，见[实现契约](docs/implementation/R1-history-evidence.md)。
R1.3 在此基础上接通显式冻结的Worker窗口与模型回读；旧请求仍走原完整历史语义，见[当前窗口契约](docs/implementation/R1-worker-history-window.md)。

新增入口及证据要求见[进程结算/核对契约](docs/implementation/S1-process-settlement.md)，
说明恢复见[epoch恢复契约](docs/implementation/S1-epoch-recovery.md)。本地控制CLI不是网络认证服务。

## 已实现的三个离线节点：部署诊断、HTTP 编解码、SSE 回放

| 节点 | 可以使用的能力 | 契约 |
| --- | --- | --- |
| R2a.3 | doctor 显示实际输入路径、默认/显式来源、逐文件观察、具体阻塞原因和未检查项；只读且不打开 Store | [部署诊断](docs/implementation/R2a-deployment-diagnostics.md) |
| R3.1 | 外部 profile 与 structured-v2 请求编码；本地 Responses HTTP JSON 文本/function_call 解码，保留原始字节及未知用量 | [Responses 离线协议](docs/implementation/R3-offline-responses.md) |
| R3.2 | 有界 SSE 增量解析，校验序号/ID/完整终态，确定性回放取消、deadline 和断流 | [SSE 契约](docs/implementation/R3-offline-responses.md#r32-sse-离线回放) |

以下命令从仓库根目录运行；路径中的 `build-cmake` 按实际构建目录替换：

```sh
./build-cmake/wls doctor --source-root .
./build-cmake/wls encode-model \
  --profile config/examples/providers/responses-offline.json \
  --request tests/fixtures/responses/request.json
./build-cmake/wls replay-model \
  --profile config/examples/providers/responses-offline.json \
  --request tests/fixtures/responses/request.json \
  --http-response tests/fixtures/responses/http-response.json
./build-cmake/wls replay-model \
  --profile config/examples/providers/responses-offline.json \
  --request tests/fixtures/responses/request.json \
  --sse-stream tests/fixtures/responses/text.sse --chunk-bytes 1
```

示例 profile 使用 `.invalid` 域名和合成模型，认证环境变量名称只作引用。两个协议命令
不需要 `--source-root`，也不接受 `--db`；不联网、不解析凭据、不派发工具。HTTP 和 SSE
报告保留原宿主 tuple，已生成的错误/中断回放报告返回退出码 `4`，输入错误返回非零错误 JSON。
编码 body 的哈希不冒充已发送的 wire 证据，局部记录哈希只检验本地一致性。

本次[实际验证报告](docs/reports/r2a3-r3-offline-validation.md)区分本地协议夹具与真实系统观察。
后续 R3.3 接实际 HTTPS/TLS、Store attempt/wire 和预算结算；供应商能力与获准真实小任务
仍须独立验收。doctor 的只读前提观察也不能替代强沙箱运行、网络隔离和后代退出证明。

## 拒绝验收后的纠正（R2.1）

`pump-fake` 现在可处理当前拒绝成果：Dispatcher 明确选择 `change_task`（只给纠正理由与方向），
随后 TeamLead 发布新说明，Worker 重新确认并提交。`team_internal_rework_max` 是双方共享的冻结额度；
耗尽时仅声明 `escalate_task`，问题通过已有 `questions` / `answer` 查询和回答。回答不会重置额度或验收成果。
完整接口与安全条件见 [R2.1 契约](docs/implementation/R2-rejected-task-resolution.md)。

可运行的真实临时 SQLite／文件／CLI 场景在 `tests/r2_rework_cli_smoke.py`：

```sh
# 普通用户，在源码根目录；Python须具备 requirements-test.txt 的测试依赖。
python3 tests/r2_rework_cli_smoke.py ./build/wls .
```

本批使用固定 FakeModel 判断；它验证流程、持久化和门禁，不测真实模型质量，不发起网络调用。

## 模型 AMEND 处置（R2.3）

`amend` 后，不提供宿主处置文件的 `pump-fake` 会让 Dispatcher 使用
`submit_amendment_plan` 提交新计划和逐节点 update/keep/cancel。
遗漏节点继续暂停；之后用 `resolve_amendment` 补处理，沿用同一计划，不重发已处置兄弟任务的说明。
每次决定绑定调用前捕获的请求和任务版本。keep 仍需新说明与 Worker 确认，UNKNOWN 保持待核对。
原 `--dispositions-file` 及旧 `run-fake --disposition` 路径保留。

完整参数、版本检查和边界见 [R2.3 契约](docs/implementation/R2-model-amendments.md)。可重复场景：

```sh
# 普通用户、源码根目录；只使用固定脚本与私有临时测试数据。
python3 tests/r2_amend_cli_smoke.py ./build/wls .
```

## 有界目录创建（R2a.1）

新请求的 general 模板显式授予 `fs_mkdir`，参数仅为 `{"path":"out"}`。父目录须存在，
只新建一个权限0700的空目录；随后可用 `fs_write` 写入 `out/result.txt`。已有目标拒绝，
不递归建父目录，不接受权限参数。操作仍受完整版本、首次派发、预算、重放和UNKNOWN门禁约束。
旧请求的冻结grant和工作区策略不被补写；只读 `tool-query` 不能创建目录。

契约见 [R2a.1目录工具](docs/implementation/R2a-directory-tool.md)，真实临时文件与CLI回归入口：

```sh
# 普通用户、源码根目录，已安装测试依赖；固定FakeModel，不调用外部模型。
python3 tests/r2a_mkdir_cli_smoke.py ./build/wls .
```

## 工作区分配恢复与明确弃置（R2a.2）

`allocate-workspace` 先保存分配意图，在私有暂存目录完成创建并保存不可变收据后，
才以无覆盖 rename 发布并登记 OWNED。重试核对原父目录和 stage/work 的完整代次。
无收据的残留仍安全拒绝采用；Root 可以保留现场并明确放弃这次 PREPARED 分配：

```sh
./build/wls owned-workspaces --db state.sqlite --request REQUEST
./build/wls abandon-workspace --db state.sqlite --request REQUEST --resource RESOURCE \
  --expected-state-version VERSION --key ABANDON_KEY --reason '保留中断现场，改用新分配'
```

从查询结果取资源 ID 和 state_version。弃置只改元数据，不删除路径、不清 UNKNOWN、不宣称清理完成。
原分配键返回原 ABANDONED 记录；新的分配键才创建新资源。父目录缺失或被替换也不阻止弃置及原键重放。
所有弃置历史及 stage/final 路径继续保留和保护；清理只选择有效资源，原冻结清单保持不变。

新 CLI 使用数据库 408；已有带请求的 407 库须先执行显式全库 `init` 维护。旧 SQL、配置和证据不改写。
每请求 64 个有效资源、256 条终生分配历史；窗口检查点不会重置这些限制。
完整边界见[分配恢复契约](docs/implementation/R2a-allocation-recovery.md)和[验证报告](docs/reports/r2a-allocation-validation.md)。

## 请求限定恢复

请求写命令只接管 `--request` 指定请求；`submit` 只准入或精确去重，不暂停同库的其他请求。
选中请求仍提升执行代次、围栏旧操作/调用并重新确认同语义说明，不能借范围选择解除 UNKNOWN、等待或 AMEND。
数据库仍只有一个写者，不是并发写入或新的认证边界。
带有请求的旧数据库升级须先显式运行 `wls init --db ...`；**init 会恢复全库请求**，应作为维护操作执行。
当前合同及兼容说明见 [请求恢复契约](docs/implementation/S1-request-recovery.md)。

## Worker 上下文窗口（显式启用）

在新请求使用的 system YAML 中保留其他 context 字段，并设置：

```yaml
context:
  request_format: structured-v2
  worker_history:
    mode: window-v1
    recent_attempts: 4
    inline_bytes: 32768
  # 保留原 compaction_at_fraction、schema_retry_max 等必填设置。
```

这不是可单独替换全部配置的文件。先运行 `wls config check`，再用 `wls submit --config ...`
冻结给新请求；随后沿原 `pump-fake` / `run-fake` 入口执行。已有请求不因YAML变化改用窗口。
模型获得只读 `history_page`（每页最多4项）与 `history_read`（每片最多4096字节），
无需重新执行业务工具。回读仍计调用预算并保存完整反馈；不能解除WAITING、UNKNOWN或AMEND。
这是确定性视图，不是LLM摘要；完整原需求/约束及未决状态始终保留，原记录不删除。

## 蓝图、计划与审核入口

| 文件 | 用途 |
| --- | --- |
| [STATUS](docs/STATUS.md) | 唯一当前能力和阶段状态入口 |
| [R1.3 Worker窗口](docs/implementation/R1-worker-history-window.md) | 冻结策略、原证据回读、旧快照兼容及验收边界 |
| [R1.1 原始历史读取](docs/implementation/R1-history-evidence.md) | 只读分页、原反馈哈希、字节分片及兼容边界 |
| [首次审核修复验证](docs/reports/claude-review-fixes-validation.md) | 10-23审核补丁的历史复现、修复和测试证据 |
| [实施蓝图 v0.4.1](docs/spec/v0.4.1/01_实施规格_v0.4.1.md) | 当前目标架构、版本关系、运行行为与各切片边界 |
| [实施路线](docs/ROADMAP.md) | 已有 M1 与 S0–S5 的承接、优先顺序和完成条件 |
| [S1 任务卡](docs/spec/v0.4.1/07_S1任务卡.md) | 基于现有代码的增量任务，不从空项目重建 |
| [本次审核修订说明](docs/REVISION-v0.4.1.md) | 每项审核发现如何进入文档、契约或内核修复 |
| [决策记录](docs/spec/v0.4.1/00_决策记录.md) | 用户明确决定、工程默认值与实际接入参数 |
| [S1 基础实现契约](docs/implementation/S1-foundation.md) | 新 C++ Store、配置、模型、Schema、CLI 与明确未实现的部分 |
| [S1.2 Worker 工具契约](docs/implementation/S1-worker-tools.md) | 文件边界、强进程条件、派发/结果/控制、403 迁移与当前限制 |
| [S1.3 事件运行器契约](docs/implementation/S1-event-runtime.md) | 持久待办、404迁移、原子多任务、原反馈、等待与CLI边界 |
| [S1.4 问答与提交契约](docs/implementation/S1-workflow-runtime.md) | 405迁移、原问题/答案、文件快照、团队检查与返工、CLI边界 |
| [S1.5 验收与交付契约](docs/implementation/S1-acceptance-delivery.md) | 406迁移、当前验收、交付恢复、回执、经验和收尾边界 |
| [S1.6 受控清理契约](docs/implementation/S1-owned-cleanup.md) | 407迁移、所有权清单、退出证据、交付重核、有限删除与恢复边界 |
| [独立审核与修复](docs/reviews/independent-2026-10-26.md) | 本轮确认的五类问题、最小修复和代码/蓝图优化决定 |
| [本轮验证](docs/reports/independent-audit-validation.md) | 基线失败、普通用户回归、完整源码与原日志绑定 |
| [R3.3受控闭环计划](docs/implementation/R3-controlled-live-plan.md) | 传输/Store账本/真实任务三节点及长任务前置的依赖、反例、完成条件 |
| [0.8.5三节点验证](docs/reports/r2a3-r3-offline-validation.md) | 前一版本R2a.3诊断、R3.1 HTTP与R3.2 SSE的实际证据 |
| [S1.6 历史发布复验](docs/reports/s1-owned-cleanup-followup-validation.md) | 当时的非root复现、夹具修复、只读保护与完整门禁 |
| [S1.6首次本地验证](docs/reports/s1-owned-cleanup-validation.md) | 原始本地记录与边界；不能替代后续CI权限失败修复后的复验 |
| [S1.5 历史验证](docs/reports/s1-acceptance-delivery-validation.md) | 验收/交付/经验增量交付时的实际证据 |
| [S1.4 历史验证](docs/reports/s1-workflow-runtime-validation.md) | 原问答/提交/团队检查交付时的实际证据 |
| [S1.3 历史验证](docs/reports/s1-event-runtime-validation.md) | 持久事件/独立多任务交付时的实际证据 |
| [S1.2 历史验证](docs/reports/s1-worker-tools-validation.md) | 文件/进程/SQLite/CLI交付时的实际证据 |
| [S1.1 历史验证](docs/reports/s1-foundation-validation.md) | 基础层交付时的实际构建与回归 |
| [M1 实现契约](docs/implementation/M1-contract.md) | 原 Journal 内核的 API、持久化、身份和合成验收 |
| [v0.4.1 修订验证](docs/reports/v0.4.1-validation.md) | 前一批审核修正的历史证据 |

原 v0.3 的 33 个文件和本次 v0.4 审核包的 49 个文件分别保存在 [docs/handoff/v0.3](docs/handoff/v0.3) 与 [docs/handoff/v0.4](docs/handoff/v0.4)，有独立 SHA-256 校验。原件里的历史状态、提示词和 AGENTS 文件只作来源档案；当前开发入口是本页、根 AGENTS 和 v0.4.1 蓝图。

## 示例目录与权限

以下示例用于新建的一次性目录，先设置 `umask 077` 并以 `mkdir -p -m 700` 创建工作目录。
`-m` 不修复已存在目录；已有目录须由所有者检查用途、归属后自行去掉 group/other 写权限，
不要递归改动未知用户数据。运行时仍拒绝不安全权限，不能靠 root 跑测试规避检查。
`config check` 的 valid 表示形状/引用有效；查阅 runtime_support 才能区分已执行与 validated_only 设置。

## 当前可以运行什么

`wls` 已有 `config check/explain`、`schema check`、`doctor`、`encode-model/replay-model`、`init/submit`、`show/events/budget/config-snapshot`、`run-fake/pump-fake`、`inbox/history/history-page/history-read/wait/wake`、`questions/submissions/answer`、`deliveries/experience/closeout`、`publish-delivery/verify-delivery/ack-delivery`、`allocate-workspace/abandon-workspace/owned-workspaces/cleanup-workspaces`、`tool-query`、`amend/cancel/pending`。只读命令不会改变执行代次；写者重开会先围栏旧执行，保留未决模型预算与 UNKNOWN。

| S1 模块 | 当前实现 |
| --- | --- |
| SQLite Store | 真正的 C++ WAL/FULL、外键、CAS/事件、独占写者；非空402→403→404→405→406→407迁移，原tuple结果/输出/完整反馈与提交字节不可变；独立计划及初始团队原子分配 |
| 配置 | LibYAML 严格解析、完整依赖快照、逐字段直接继承冻结 Root、哈希恢复、来源解释；环境 check 保持为数据 |
| JSON / Schema | 严格 JSON、SHA-256、typed ULID；Valijson Draft-07、本地引用注册表、完整记录与模型输入投影 |
| 模型和上下文 | 有界脚本 FakeModel；实际前缀字节及依赖哈希；完整原文、AMEND、任务和说明；不截断约束 |
| 尝试与预算 | 完整调用输入先持久化，响应保留原 tuple；每次尝试独立计数，错误／迟到也记账，未知用量保留预留 |
| Worker 文件工具 | dirfd/openat2 限定工作目录；分页读取、字面搜索、创建、完整 SHA 覆盖/唯一匹配 patch、原子写入及 UNKNOWN |
| 进程与控制 | 显式 argv/env、固定运行文件 SHA、强 bwrap/cgroup 入口；缺条件拒绝；ack/cancel 精确重放与诊断不被业务门禁锁死 |
| 离线角色流程 | 原单任务run-fake保留；pump-fake按持久待办推进独立多任务，逐项确认，AMEND遗漏保持暂停，后补keep使用同一计划 |
| 等待与历史 | 宿主wait/wake持久化，Store阻断等待中的新调用和业务；原响应/完整反馈/绑定跨运行恢复，缺失明确标记，容量超限先阻止预约 |
| 问答 | Worker发起，TeamLead/Dispatcher逐级回答或附注转发，Root接收用户ANSWER；原文与原绑定不改，精确对象版本和等待代次防旧答唤醒 |
| 提交与团队检查 | 实际完整文件字节/哈希同事务保存；TeamLead看原快照、逐项检查，有界返工推进任务/指令并重ack；CANDIDATE_READY不等于业务验收 |
| 业务验收 | Dispatcher捕获单一当前候选、逐项业务判断；Store核原字节/检查/AC/版本/权限/UNKNOWN，肯定接受与经验作业同事务 |
| 交付与回执 | 先持久意图，原字节fsync与目录原子发布，再记录STORED；精确回执后DELIVERED，查询不代签，AMEND失效未确认包 |
| 经验与收尾 | 显式报告或无新增经验；终态可恢复专用TL管理调用，预算不足保留pending；留存全局cleanup作业；受控工作目录可删除/恢复，私有行仍保留 |

`wls861 demo` 创建两个独立请求，各自使用 `Root → Dispatcher → TeamLead → Worker`，并行完成合成工作、独立审核、交付回执与资源状态收尾，输出 JSON 证据。演示使用固定字节和符号模型，模型调用数为零。

| 模块 | 已实现范围 |
| --- | --- |
| TaskEngine | 请求与命令分别去重；NEW / AMEND / REPLACE / CANCEL；角色与团队归属检查；条件状态提交 |
| 版本与恢复 | 独立需求、计划、应用、状态、代次与团队指令版本；启动自动隔离旧代次；未知操作必须核对 |
| 控制与执行 | 变更确认和操作取消使用专门控制门禁；确认新说明不解除 UNKNOWN 对新业务的阻断；取消运行操作保留 Unknown |
| DurableJournal | 状态与事件同帧写入，成功前同步；断尾恢复；完整损坏拒绝；独占写者锁 |
| Scheduler | 固定线程池、有界队列；同资源键有序；独立键并行；审核和交接常态预留准入及运行容量 |
| 配置与权限 | 逐字段直接继承请求冻结的 Root 快照；能力不支持时明确拒绝；Worker 与允许列表交集 |
| 候选与交付 | 不复用成果版本；全部指定审核团队通过；受理的生产说明更新使当前候选失效，当前负面审核可撤回批准；匹配回执后才完成 |
| 资源状态 | 业务完成与资源关闭独立；未核对操作阻止停止；清理失败可重试 |
| v0.4.1 契约参考 | JSON Schema、消息示例、SQLite 表结构及 Python 事务参考；用于检验未来模块能否按同一契约连接 |

当前 `TaskEngine` 仍使用 Journal；新的 `wls_s1` 库使用显式 SQLite Store 和配置快照，两者分别验证。Python SQLite／Schema 帮助程序继续作为契约参考；它们的测试通过不代表 C++ 已有完整记忆、审核或清理运行器。新数据库也不把原 Journal 当作 SQLite 导入。

## 构建与验证

需要 Linux、支持 C++20 的 GCC/Clang、Make、Python 3.9+；CMake/CTest 是另一入口。S1 使用 **Boost 1.83+、SQLite 3.37+、OpenSSL 3+ 和 LibYAML 0.2.5 运行库**。Valijson 和 LibYAML 公共头已固定导入，见 [依赖来源与许可证](third_party/README.md)。契约测试需要 `requirements-test.txt` 中的 `jsonschema`，以及 Python 所链接的 SQLite 3.37+。

```sh
git clone https://github.com/wmqfl861/wls861.git
cd wls861
# Ubuntu 24.04 开发依赖；不安装或启动服务。
sudo apt-get install build-essential cmake libboost1.83-dev libsqlite3-dev libssl-dev libyaml-dev python3-venv
python3 -m venv .local/test-venv
.local/test-venv/bin/python -m pip install -r requirements-test.txt
make -j2 test PYTHON=.local/test-venv/bin/python
./build/wls861 demo
make -j2 ubsan PYTHON=.local/test-venv/bin/python
```

如果所用 Python 已有测试依赖，可直接指定它；每批实际解释器和依赖版本见对应验证报告。GitHub CI 使用 `requirements-test.txt` 固定版本。构建不会联网获取 C++ 源码；固定导入的上游文件有逐文件哈希清单。

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$PWD/.local/test-venv/bin/python"
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

仅构建 C++ 程序可使用 `make -j2`，或 CMake 的 `-DBUILD_TESTING=OFF`。只核对契约用 `make verify-contracts PYTHON=...`；原件核对用 `make verify-handoff PYTHON=...`。

完整测试入口包含 C++ 模块、CLI 演示、两个原始包校验及跨模块契约回归。严格编译启用主要警告和 `-Werror`，GitHub Actions 对 Release 与 UBSan 两种构建运行相同检查。原 51 项产品验收仍保留历史“未执行”，每批实际证据独立记录。

## S1.6 受控清理

现有目录不会自动获得删除权限。先由 `allocate-workspace` 为已规划任务创建新目录，将返回的
`identity.workspace_identity.path` 传给该任务的工作目录映射。完成正常验收、精确交付回执、
经验报告与用量结算后，显式 `cleanup-workspaces --confirm-request REQUEST` 才执行。
`cleanup.enabled` 必须在该请求准入时冻结为 true；例子配置仍为 false。

完整、可重复且仅使用临时目录的演示是：

```sh
python3 tests/s16_cli_smoke.py "$PWD/build/wls" "$PWD"
```

它实际启动 C++ CLI 验证默认拒绝、分批删除、跨进程恢复、丢失/篡改交付包和删除后记录失败。
结果中的 `workspace_cleanup_recorded=true` 不等于 `cleanup_complete=true`；本版本全局清理
仍为 PENDING，私有证据与原交付包继续保留。详见 [接口与边界](docs/implementation/S1-owned-cleanup.md)。

## 运行一次持久化离线流程

在仓库根目录执行；`--db` 指向你选定的运行数据路径，以下使用被 Git 忽略的开发目录：

```sh
umask 077
mkdir -p -m 700 .local/demo
./build/wls config check
./build/wls config explain --role worker
./build/wls submit --db .local/demo/request.db --key s1-example \
  --text-file tests/fixtures/s1_request.txt \
  --constraints-file tests/fixtures/s1_constraints.json
```

将 JSON 输出中的 `request.request_id` 作为后续 `--request` 参数：

```sh
./build/wls run-fake --db .local/demo/request.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s1_fake_initial.json
./build/wls amend --db .local/demo/request.db --request req_YOUR_RETURNED_ID \
  --text-file tests/fixtures/s1_amendment.txt \
  --constraints-file tests/fixtures/s1_amended_constraints.json
./build/wls run-fake --db .local/demo/request.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s1_fake_amended.json --disposition update
./build/wls show --db .local/demo/request.db --request req_YOUR_RETURNED_ID
./build/wls events --db .local/demo/request.db --request req_YOUR_RETURNED_ID
```

`req_YOUR_RETURNED_ID` 是待替换占位符。脚本中的 token 和费用是明确的合成用量；实际供应商调用数为零。note 是数据库中的操作记录，成功不等于业务成果已验收或交付。初始 `--constraints-file` 省略时目录为空，CLI 不会宣称已从自由文本抽取完整约束；AMEND 必须提供整份有效约束目录。运行目录不同时使用 `--source-root /path/to/wls861`。

## 运行真实文件工具

以下示例在一个独立开发目录中创建并修改 `result.txt`，固定模型脚本不访问外部服务。数据库放在工作目录之外；同一团队的目录身份和执行策略首次绑定后不自动改变。

```sh
umask 077
mkdir -p -m 700 .local/s12/work
./build/wls submit --db .local/s12/runtime.db --key s12-file-example \
  --text-file tests/fixtures/s12_request.txt \
  --constraints-file tests/fixtures/s12_constraints.json
./build/wls run-fake --db .local/s12/runtime.db --request req_YOUR_RETURNED_ID \
  --workspace .local/s12/work --worker-calls 3 \
  --script tests/fixtures/s12_fake_workspace.json
```

预期文件内容为 `verified from S1.2` 加换行；总共 5 次 Fake 调用，两个文件写操作和一次只读操作。覆盖已有文件需要正确旧 SHA，重复使用这个新建示例不会覆盖已经存在的结果。将输出中的 `task_id` 用于只读查询：

```sh
./build/wls tool-query --db .local/s12/runtime.db --request req_YOUR_RETURNED_ID \
  --task task_YOUR_RETURNED_ID --workspace .local/s12/work \
  --tool fs_read --input tests/fixtures/s12_read_query.json
./build/wls doctor
```

进程工具需要另行准备的强环境及 [manifest](config/examples/sandbox-manifest.json)。该样例故意没有 launcher SHA、可写 cgroup 委派和运行文件，doctor 会列出缺项，run 会拒绝启动。实际使用前填写 bwrap 和每个运行文件的 SHA-256、白名单目标及已委派路径，并在新请求的首次工作目录绑定前传入 `--sandbox-manifest`。程序不会安装 bwrap、修改 user namespace/AppArmor 或创建宿主委派。完整参数与边界见 [Worker 工具契约](docs/implementation/S1-worker-tools.md)。

## 运行持久事件与两个独立任务

以下使用新增的确定性脚本，各任务各有一个目录。调用次数达到上限时，后续工作仍在 inbox 中；示例输出不声明完成业务验收或交付。

```sh
umask 077
mkdir -p -m 700 .local/s13/alpha .local/s13/beta
python3 - <<'PYMAP'
import json
from pathlib import Path
root = Path('.local/s13').resolve()
(root / 'workspaces.json').write_text(json.dumps({
    'alpha': str(root / 'alpha'), 'beta': str(root / 'beta')
}))
PYMAP
./build/wls submit --db .local/s13/runtime.db --key s13-events \
  --text-file tests/fixtures/s13_request.txt \
  --constraints-file tests/fixtures/s13_constraints.json
./build/wls pump-fake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s13_fake_initial.json \
  --workspaces-file .local/s13/workspaces.json --worker-calls 2
```

第一次 pump 在 alpha/beta 目录各新建 `r1.txt` 并读取一次，共 7 次 Fake 调用。下面的变更只处置 alpha，beta 保持暂停；随后显式 keep beta，仍要求它取得新说明和 ack：

```sh
./build/wls amend --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --text-file tests/fixtures/s13_amendment.txt \
  --constraints-file tests/fixtures/s13_amended_constraints.json
./build/wls pump-fake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s13_fake_amended.json \
  --workspaces-file .local/s13/workspaces.json \
  --dispositions-file tests/fixtures/s13_update_alpha.json
./build/wls inbox --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID
```

补 beta 时，使用只包含 beta 后续步骤的脚本，避免再次要求 alpha 执行其文件新建动作。显式 keep 继续使用当前计划：

```sh
./build/wls pump-fake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s13_fake_keep_beta.json \
  --workspaces-file .local/s13/workspaces.json \
  --dispositions-file tests/fixtures/s13_keep_beta.json
```

只读历史和持久暂停的入口为：

```sh
./build/wls history --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --role worker --task tsk_YOUR_RETURNED_ID
./build/wls wait --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --task tsk_YOUR_RETURNED_ID --reason '等待宿主补充当前任务资料'
./build/wls show --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID
./build/wls wake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --task tsk_YOUR_RETURNED_ID --wait-generation 1
```

`--task` 与 `--wait-generation` 使用实际输出值。wake 不代替新的 AMEND 处置、说明确认或 UNKNOWN 核对，也不能绕过尚未回答的 blocking question。脚本分组、部分目录和原事件语义见 [S1.3 契约](docs/implementation/S1-event-runtime.md)，新的问答/提交行为见 [S1.4 契约](docs/implementation/S1-workflow-runtime.md)。

## 运行提交、检查和返工

下面的固定脚本在独立 alpha 目录写第一版文件，提交完整快照，接收 TeamLead 返工说明，再写第二版并重新提交检查。该示例没有外部模型调用。

```sh
umask 077
mkdir -p -m 700 .local/s14/alpha
python3 - <<'PYMAP'
import json
from pathlib import Path
root = Path('.local/s14').resolve()
(root / 'workspaces.json').write_text(json.dumps({'alpha': str(root / 'alpha')}))
PYMAP
./build/wls submit --db .local/s14/runtime.db --key s14-rework \
  --text-file tests/fixtures/s14_request.txt \
  --constraints-file tests/fixtures/s14_constraints.json
./build/wls pump-fake --db .local/s14/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s14_fake_rework.json \
  --workspaces-file .local/s14/workspaces.json --worker-calls 4
./build/wls show --db .local/s14/runtime.db --request req_YOUR_RETURNED_ID
./build/wls submissions --db .local/s14/runtime.db --request req_YOUR_RETURNED_ID \
  --task tsk_YOUR_RETURNED_ID
```

任务 ID 取实际输出。提交路径使用 `/work/...`；文件工具参数仍使用相对路径。每次最多 8 个文件、每个 64 KiB、合计 256 KiB，必须完整捕获，超限拒绝。旧提交字节和哈希不会随工作目录之后的修改而变化；TeamLead 通过只产生 `CANDIDATE_READY`，不表示业务已经验收或交付。

下面用另一个数据库和 alpha/beta 两个独立目录演示提问。alpha 的 Worker 等待答案时，beta 仍可写文件；脚本随后将 alpha 的原问题从 TeamLead 转给 Dispatcher，再转给用户：

```sh
umask 077
mkdir -p -m 700 .local/s14-questions/alpha .local/s14-questions/beta
python3 - <<'PY'
import json
from pathlib import Path
root = Path('.local/s14-questions').resolve()
(root / 'workspaces.json').write_text(json.dumps({
    'alpha': str(root / 'alpha'),
    'beta': str(root / 'beta')
}))
PY
./build/wls submit --db .local/s14-questions/runtime.db --key s14-question-route \
  --text-file tests/fixtures/s14_request.txt \
  --constraints-file tests/fixtures/s14_constraints.json
./build/wls pump-fake --db .local/s14-questions/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s14_fake_question_route.json \
  --workspaces-file .local/s14-questions/workspaces.json --worker-calls 1
./build/wls questions --db .local/s14-questions/runtime.db --request req_YOUR_RETURNED_ID
./build/wls answer --db .local/s14-questions/runtime.db --request req_YOUR_RETURNED_ID \
  --question q_YOUR_RETURNED_ID --expected-version QUESTION_STATE_VERSION \
  --text-file tests/fixtures/s14_answer.txt
```

`q_YOUR_RETURNED_ID` 和 `QUESTION_STATE_VERSION` 分别替换为查询得到的问题 ID 与 state_version；文本与 `--selected-option` 至少给一项。已答、过期、跨请求或未上报用户的问题不能被此入口重新授权，错误输入在打开写者前检查。回答只结束该问题的等待，恢复后的新说明/ack 和 UNKNOWN 检查继续生效。

## 运行当前验收与真实本地交付

使用新的演示目录，保持业务 workspace、控制数据库和交付目录分离：

```sh
umask 077
mkdir -p -m 700 .local/s15-demo/alpha .local/s15-demo/deliveries
python3 - <<'PY'
import json
from pathlib import Path
base = Path('.local/s15-demo').resolve()
(base / 'workspaces.json').write_text(json.dumps({'alpha': str(base / 'alpha')}))
PY
./build/wls submit --db .local/s15-demo/runtime.db --key s15-complete \
  --text-file tests/fixtures/s14_request.txt \
  --constraints-file tests/fixtures/s14_constraints.json
./build/wls pump-fake --db .local/s15-demo/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s15_fake_complete.json \
  --workspaces-file .local/s15-demo/workspaces.json --worker-calls 2
./build/wls experience --db .local/s15-demo/runtime.db --request req_YOUR_RETURNED_ID
./build/wls publish-delivery --db .local/s15-demo/runtime.db --request req_YOUR_RETURNED_ID \
  --delivery-dir .local/s15-demo/deliveries
./build/wls deliveries --db .local/s15-demo/runtime.db --request req_YOUR_RETURNED_ID
```

这个固定脚本包含七次 Fake 调用：计划、说明、写文件、提交、TL 检查、Dispatcher 验收、明确经验报告。实际文件、SQLite 和发布目录由 C++ 执行；验收判断是测试脚本，不表示真实模型质量已验证。发布后为 `STORED`，尚没有回执。

将发布输出中的 `delivery_id`、`manifest_sha256`、`requirements_revision` 和 `plan_revision` 分别替换到下面的占位符。查询和 `verify-delivery` 不会确认接收：

```sh
./build/wls verify-delivery --db .local/s15-demo/runtime.db --request req_YOUR_RETURNED_ID \
  --delivery dlv_YOUR_RETURNED_ID --delivery-dir .local/s15-demo/deliveries
./build/wls ack-delivery --db .local/s15-demo/runtime.db --request req_YOUR_RETURNED_ID \
  --delivery dlv_YOUR_RETURNED_ID --delivery-dir .local/s15-demo/deliveries \
  --manifest-sha256 YOUR_MANIFEST_SHA256 \
  --requirements-revision REQUIREMENTS_REVISION --plan-revision PLAN_REVISION \
  --recipient local-user --method durable_local --key s15-local-receipt
./build/wls closeout --db .local/s15-demo/runtime.db --request req_YOUR_RETURNED_ID
```

ACK 会重新核验并同步完整包，随后原子保存精确回执和收尾作业，业务状态成为 `DELIVERED`。`durable_local` 只表示约定位置已有持久包，不表示人已阅读。不同意图不能复用同一回执键；AMEND 或撤销验收后，旧未确认包只能作为历史保留。

若发布中断，使用 `publish-delivery --delivery dlv_YOUR_RETURNED_ID` 恢复原意图；重试不会覆盖包或读取后来变化的 workspace。经验未完成时可用 `s15_fake_report.json` 在后续 pump 中继续，预算耗尽则保持待办。`closeout` 会明确保留未完成清理；本例的普通目录不具备S1.6所有权，不能直接清理；新受控目录的创建/清理见上文S1.6，私有行仍保留。原验收/交付边界见 [S1.5契约](docs/implementation/S1-acceptance-delivery.md)。

## 后续实施边界

当前内核是受信宿主进程内 API，`Actor` 由宿主构造。本机 CLI 不提供远程认证；诊断速率限制在单进程内，写者持租约期间没有第二个 CLI 的 socket 取消通道。文件容量是代理的逻辑上限，不是任意进程磁盘配额；合作锁不防任意同 UID 宿主绕过。强进程正向测试、残留 cgroup 恢复及完整 SBX/SEC 验收仍需独立证据。

R2a.2已接入PREPARED分配恢复/显式放弃，R2a.3已接入doctor实际路径与有界阻塞诊断，R3.1/R3.2提供独立Responses HTTP/SSE离线回放。后继按[R3.3a/b/c计划](docs/implementation/R3-controlled-live-plan.md)推进本地TLS组件、原Store/账本集成及获准小任务与单Agent对照；管理视图与owned容量单列长任务前置。私有行purge与wlsd/API继续后置。跨功能独立审核、DAG和审核双容量按S2推进，S3记忆、S4 MCP/评测和S5证据驱动扩展保持原路线。S1-06/07强环境正向独立验收；真实协议的端点、凭据引用、能力和预算由外部配置提供。现有运行器仍用FakeModel，协议夹具不联网，付费重试为零。

### 存储准入窗口和人工续进

`wls capacity --db DB --request ID` 只读显示冻结上限、告警/暂停和窗口号；`pending`也列出容量暂停。
达到水位后，宿主先核清未决操作及用量，再执行 `wls capacity-checkpoint --db DB --request ID
--expected-window N --evidence-file FILE --key KEY`。证据文件为非空文本。
检查点不会重置工具/费用/返工，也不代替AMEND处置、问题回答或UNKNOWN核对；原事件和重放结果保留。
参见[容量契约](docs/implementation/S1-capacity-windows.md)及[当前状态](docs/STATUS.md)。

### R1.2 结构化输入（新请求显式选择）

在新请求使用的system配置中将`context.request_format`设为`structured-v2`，其余配置保持完整；
`submit --config /path/to/system.yaml --functions-dir /path/to/functions ...` 会冻结此选择。
`run-fake`和`pump-fake`随后使用独立工具Schema和ID配对的消息，`config explain`披露所选格式。
兼容样例仍为`framed-v1`；旧快照缺字段也保持旧格式，不因当前配置变化迁移。
这一步不启用真实模型、窗口压缩或付费重试；[契约](docs/implementation/R1-structured-context.md)
列出完整边界，当前状态见[STATUS](docs/STATUS.md)。
