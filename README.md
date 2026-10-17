# 无量山 · wls861

C++ 通用多 Agent 系统，Linux 优先。**当前增量：0.2.0-s1.1；当前蓝图：v0.4.1。** 原 `wls861` 继续提供 0.1.1-m1 合成内核，新 `wls` 提供 S1 基础层的持久化离线流程。

本批按新计划接入了真正的 **C++ SQLite Store、YAML 加载与冻结、Draft-07 Schema、FakeModel、ContextCompiler 和本地 CLI**。现在可以提交请求，让 Dispatcher 规划、TeamLead 下发说明、Worker 确认并写入持久 note，再执行 AMEND、重开和迟到响应的回归。S1 的真实文件／进程工具、沙箱、供应商调用、守护进程与业务交付仍待后续任务卡。

## 蓝图、计划与审核入口

| 文件 | 用途 |
| --- | --- |
| [实施蓝图 v0.4.1](docs/spec/v0.4.1/01_实施规格_v0.4.1.md) | 当前目标架构、版本关系、运行行为与各切片边界 |
| [实施路线](docs/ROADMAP.md) | 已有 M1 与 S0–S5 的承接、优先顺序和完成条件 |
| [S1 任务卡](docs/spec/v0.4.1/07_S1任务卡.md) | 基于现有代码的增量任务，不从空项目重建 |
| [本次审核修订说明](docs/REVISION-v0.4.1.md) | 每项审核发现如何进入文档、契约或内核修复 |
| [决策记录](docs/spec/v0.4.1/00_决策记录.md) | 用户明确决定、工程默认值与实际接入参数 |
| [S1 基础实现契约](docs/implementation/S1-foundation.md) | 新 C++ Store、配置、模型、Schema、CLI 与明确未实现的部分 |
| [本批验证报告](docs/reports/s1-foundation-validation.md) | S1.1 实际构建、回归、独立复核与限制 |
| [M1 实现契约](docs/implementation/M1-contract.md) | 原 Journal 内核的 API、持久化、身份和合成验收 |
| [v0.4.1 修订验证](docs/reports/v0.4.1-validation.md) | 前一批审核修正的历史证据 |

原 v0.3 的 33 个文件和本次 v0.4 审核包的 49 个文件分别保存在 [docs/handoff/v0.3](docs/handoff/v0.3) 与 [docs/handoff/v0.4](docs/handoff/v0.4)，有独立 SHA-256 校验。原件里的历史状态、提示词和 AGENTS 文件只作来源档案；当前开发入口是本页、根 AGENTS 和 v0.4.1 蓝图。

## 当前可以运行什么

`wls` 已有 `config check/explain`、`schema check`、`submit`、`show`、`events`、`budget`、`config-snapshot`、`run-fake`、`amend`、`cancel`、`pending` 入口。只读命令不会改变执行代次；写者重开会先围栏旧执行，保留未决模型预算与操作结果。

| S1.1 模块 | 本批实现 |
| --- | --- |
| SQLite Store | 真正的 C++ 事务、WAL/FULL、外键、CAS、状态与事件原子提交、独占写者、迁移检查与启动恢复 |
| 配置 | LibYAML 严格解析、完整依赖快照、逐字段直接继承冻结 Root、哈希恢复、来源解释；环境 check 保持为数据 |
| JSON / Schema | 严格 JSON、SHA-256、typed ULID；Valijson Draft-07、本地引用注册表、完整记录与模型输入投影 |
| 模型和上下文 | 有界脚本 FakeModel；实际前缀字节及依赖哈希；完整原文、AMEND、任务和说明；不截断约束 |
| 尝试与预算 | 完整调用输入先持久化，响应保留原 tuple；每次尝试独立计数，错误／迟到也记账，未知用量保留预留 |
| 离线角色流程 | 一个独立任务，最多三次显式 Fake 调用，当前说明确认后执行持久 note；中途变更需显式 update/keep/cancel |

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

如果所用 Python 已有测试依赖，可直接指定它。本次工作区的测试使用 `PYTHON=/usr/bin/python3`。构建不会联网获取 C++ 源码；固定导入的上游文件有逐文件哈希清单。

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

## 后续实施边界

当前内核是受信宿主进程内 API，`Actor` 由宿主构造。真实外部身份认证、文件与进程工具、bubblewrap/cgroup、模型网关、异步角色运行器、交付包和记忆模块按 S1–S5 增量实现。

下一批沿 S1-06/07 接入实际文件／进程工具及 bubblewrap/cgroup 能力检查，再补完整角色唤醒循环、交付包、回执和清理，以及 wlsd/API。真实模型协议在外部端点、凭据引用、能力和预算齐备后独立接入。配置样例保持 FakeModel、记忆关闭、真实调用关闭、付费重试为零；本批没有提供服务部署或真实模型授权。
