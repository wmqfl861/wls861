# 无量山 · wls861

C++ 通用多 Agent 系统，Linux 优先。**当前增量：0.3.0-s1.2；当前蓝图：v0.4.1。** 原 `wls861` 继续提供 0.1.1-m1 合成内核，`wls` 提供持久化 FakeModel 流程和受控 Worker 工具。

本批沿 S1-06/07 新增 **真实文件读写/搜索/哈希补丁、唯一派发权、原始结果与二进制输出持久化、独立控制分支，以及 bwrap/cgroup 强进程入口和 doctor**。FakeModel 可在一个独立任务中连续调用这些工具，结果进入后续完整上下文。当前环境已验证文件闭环和强模式拒绝，完整强沙箱正向验收仍待具备条件的环境；真实模型、守护进程、业务验收/交付和可恢复清理继续按任务卡开发。

## 蓝图、计划与审核入口

| 文件 | 用途 |
| --- | --- |
| [实施蓝图 v0.4.1](docs/spec/v0.4.1/01_实施规格_v0.4.1.md) | 当前目标架构、版本关系、运行行为与各切片边界 |
| [实施路线](docs/ROADMAP.md) | 已有 M1 与 S0–S5 的承接、优先顺序和完成条件 |
| [S1 任务卡](docs/spec/v0.4.1/07_S1任务卡.md) | 基于现有代码的增量任务，不从空项目重建 |
| [本次审核修订说明](docs/REVISION-v0.4.1.md) | 每项审核发现如何进入文档、契约或内核修复 |
| [决策记录](docs/spec/v0.4.1/00_决策记录.md) | 用户明确决定、工程默认值与实际接入参数 |
| [S1 基础实现契约](docs/implementation/S1-foundation.md) | 新 C++ Store、配置、模型、Schema、CLI 与明确未实现的部分 |
| [S1.2 Worker 工具契约](docs/implementation/S1-worker-tools.md) | 文件边界、强进程条件、派发/结果/控制、403 迁移与当前限制 |
| [本批验证报告](docs/reports/s1-worker-tools-validation.md) | S1.2 实际构建、文件/进程/SQLite/CLI 分层证据与限制 |
| [S1.1 历史验证](docs/reports/s1-foundation-validation.md) | 基础层交付时的实际构建与回归 |
| [M1 实现契约](docs/implementation/M1-contract.md) | 原 Journal 内核的 API、持久化、身份和合成验收 |
| [v0.4.1 修订验证](docs/reports/v0.4.1-validation.md) | 前一批审核修正的历史证据 |

原 v0.3 的 33 个文件和本次 v0.4 审核包的 49 个文件分别保存在 [docs/handoff/v0.3](docs/handoff/v0.3) 与 [docs/handoff/v0.4](docs/handoff/v0.4)，有独立 SHA-256 校验。原件里的历史状态、提示词和 AGENTS 文件只作来源档案；当前开发入口是本页、根 AGENTS 和 v0.4.1 蓝图。

## 当前可以运行什么

`wls` 已有 `config check/explain`、`schema check`、`doctor`、`init/submit`、`show/events/budget/config-snapshot`、`run-fake`、`tool-query`、`amend/cancel/pending`。只读命令不会改变执行代次；写者重开会先围栏旧执行，保留未决模型预算与 UNKNOWN。

| S1 模块 | 当前实现 |
| --- | --- |
| SQLite Store | 真正的 C++ WAL/FULL、外键、CAS/事件、独占写者；非空 402→403 迁移，原 tuple 结果及二进制输出不可变保存 |
| 配置 | LibYAML 严格解析、完整依赖快照、逐字段直接继承冻结 Root、哈希恢复、来源解释；环境 check 保持为数据 |
| JSON / Schema | 严格 JSON、SHA-256、typed ULID；Valijson Draft-07、本地引用注册表、完整记录与模型输入投影 |
| 模型和上下文 | 有界脚本 FakeModel；实际前缀字节及依赖哈希；完整原文、AMEND、任务和说明；不截断约束 |
| 尝试与预算 | 完整调用输入先持久化，响应保留原 tuple；每次尝试独立计数，错误／迟到也记账，未知用量保留预留 |
| Worker 文件工具 | dirfd/openat2 限定工作目录；分页读取、字面搜索、创建、完整 SHA 覆盖/唯一匹配 patch、原子写入及 UNKNOWN |
| 进程与控制 | 显式 argv/env、固定运行文件 SHA、强 bwrap/cgroup 入口；缺条件拒绝；ack/cancel 精确重放与诊断不被业务门禁锁死 |
| 离线角色流程 | 一个独立任务；显式 workspace 时 1–16 次 Worker 调用，默认一次；逐次保存反馈，失败/UNKNOWN 停止本批，中途变更仍需显式处置 |

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

如果所用 Python 已有测试依赖，可直接指定它。本批使用独立环境 `.local/test-venv/bin/python` 安装仓库固定的测试依赖。构建不会联网获取 C++ 源码；固定导入的上游文件有逐文件哈希清单。

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$PWD/.local/test-venv/bin/python"
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

仅构建 C++ 程序可使用 `make -j2`，或 CMake 的 `-DBUILD_TESTING=OFF`。只核对契约用 `make verify-contracts PYTHON=...`；原件核对用 `make verify-handoff PYTHON=...`。

完整测试入口包含 C++ 模块、CLI 演示、两个原始包校验及跨模块契约回归。严格编译启用主要警告和 `-Werror`，GitHub Actions 对 Release 与 UBSan 两种构建运行相同检查。原 51 项产品验收仍保留历史“未执行”，每批实际证据独立记录。

## 运行一次持久化离线流程

在仓库根目录执行；`--db` 指向你选定的运行数据路径，以下使用被 Git 忽略的开发目录：

```sh
mkdir -p .local/demo
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
mkdir -p .local/s12/work
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

## 后续实施边界

当前内核是受信宿主进程内 API，`Actor` 由宿主构造。本机 CLI 不提供远程认证；诊断速率限制在单进程内，写者持租约期间没有第二个 CLI 的 socket 取消通道。文件容量是代理的逻辑上限，不是任意进程磁盘配额；合作锁不防任意同 UID 宿主绕过。强进程正向测试、残留 cgroup 恢复及完整 SBX/SEC 验收仍需独立证据。

下一批沿 S1-03/09/10 接持久事件、等待/唤醒和多任务 AMEND，并继续完成 S1-06/07 正向隔离验收与恢复；随后是交付包、回执、清理和 wlsd/API。真实模型协议在外部端点、凭据引用、能力和预算齐备后独立接入。配置样例保持 FakeModel、记忆关闭、真实调用关闭、付费重试为零。
