# 无量山 · wls861

C++ 通用多 Agent 系统，Linux 优先。**当前代码版本：0.1.1-m1；当前蓝图：v0.4.1。**

本仓库已经具有可运行的无模型任务内核。v0.4.1 吸收新增审核中的角色行为、结构化交接、纵向切片和轻量记忆方案，并修正变更门禁、控制操作、版本证据及跨 Schema／数据库契约。下一条产品切片是 S1 真实任务闭环；当前尚未完成真实模型、业务工具、沙箱或记忆引擎。

## 蓝图、计划与审核入口

| 文件 | 用途 |
| --- | --- |
| [实施蓝图 v0.4.1](docs/spec/v0.4.1/01_实施规格_v0.4.1.md) | 当前目标架构、版本关系、运行行为与各切片边界 |
| [实施路线](docs/ROADMAP.md) | 已有 M1 与 S0–S5 的承接、优先顺序和完成条件 |
| [S1 任务卡](docs/spec/v0.4.1/07_S1任务卡.md) | 基于现有代码的增量任务，不从空项目重建 |
| [本次审核修订说明](docs/REVISION-v0.4.1.md) | 每项审核发现如何进入文档、契约或内核修复 |
| [决策记录](docs/spec/v0.4.1/00_决策记录.md) | 用户明确决定、工程默认值与实际接入参数 |
| [M1 当前实现契约](docs/implementation/M1-contract.md) | 当前 C++ API、持久化、身份、状态和信任边界 |
| [本次验证报告](docs/reports/v0.4.1-validation.md) | 实际构建与回归结果、依赖和验证限制 |

原 v0.3 的 33 个文件和本次 v0.4 审核包的 49 个文件分别保存在 [docs/handoff/v0.3](docs/handoff/v0.3) 与 [docs/handoff/v0.4](docs/handoff/v0.4)，有独立 SHA-256 校验。原件里的历史状态、提示词和 AGENTS 文件只作来源档案；当前开发入口是本页、根 AGENTS 和 v0.4.1 蓝图。

## 当前可以运行什么

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

SQLite／Schema 的契约检查与 C++ 内核分别验证。当前 `TaskEngine` 仍使用 Journal；尚未将参考 SQLite 存储接进 C++ 守护进程，也没有自动执行提示词或加载生产 YAML。

## 构建与验证

需要 Linux、支持 C++20 的 GCC/Clang、Make、Python 3.9+；CMake/CTest 是另一入口。C++ 运行时无需第三方库。契约测试需要 `requirements-test.txt` 中的 `jsonschema`，以及 Python 标准库所链接的 SQLite 3.37+（使用 STRICT 表）。

```sh
git clone https://github.com/wmqfl861/wls861.git
cd wls861
python3 -m venv .local/test-venv
.local/test-venv/bin/python -m pip install -r requirements-test.txt
make -j2 test PYTHON=.local/test-venv/bin/python
./build/wls861 demo
make -j2 ubsan PYTHON=.local/test-venv/bin/python
```

如果所用 Python 已有测试依赖，可直接指定它。本次工作区的完整测试使用 `PYTHON=/usr/bin/python3`，没有为此下载依赖。

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$PWD/.local/test-venv/bin/python"
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

仅构建 C++ 程序可使用 `make -j2`，或 CMake 的 `-DBUILD_TESTING=OFF`。只核对契约用 `make verify-contracts PYTHON=...`；原件核对用 `make verify-handoff PYTHON=...`。

完整测试入口包含 C++ 模块、CLI 演示、两个原始包校验及跨模块契约回归。严格编译启用主要警告和 `-Werror`，GitHub Actions 对 Release 与 UBSan 两种构建运行相同检查。原 51 项产品验收仍保留历史“未执行”，每批实际证据独立记录。

## 后续实施边界

当前内核是受信宿主进程内 API，`Actor` 由宿主构造。真实外部身份认证、文件与进程工具、bubblewrap/cgroup、模型网关、异步角色运行器、交付包和记忆模块按 S1–S5 增量实现。

下一批先用确定性模型替身打通角色与工具接口，再在已配置端点、认证、能力和预算下做真实闭环。配置样例继续默认 FakeModel、记忆关闭、真实预算空缺；真实模型尝试和自动付费重试不会因本次开发授权而自动启用。
