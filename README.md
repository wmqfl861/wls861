# 无量山 · wls861

C++ 通用多 Agent 系统，Linux 优先。当前版本：**0.1.0-m1，首批可运行的无模型内核原型**。

本仓库从用户提供的 2026-10-17 v0.3 交接包接手。该包原本只有设计，没有产品源码；现在新增了任务内核、持久化、调度、配置解析和完整合成演示。原交接包的 33 个文件保留在 [docs/handoff/v0.3](docs/handoff/v0.3)，校验清单与历史“未执行”验收记录保持原样。

## 当前可以运行什么

`wls861 demo` 创建两个独立请求，各自使用 `Root → Dispatcher → TeamLead → Worker`，并行完成合成工作、独立审核、交付回执与资源状态收尾，输出 JSON 证据。演示使用固定 `hello` 字节和符号模型；模型调用数为零。

| 模块 | M1 已实现范围 |
| --- | --- |
| TaskEngine | 请求与命令分别去重；NEW / AMEND / REPLACE / CANCEL；角色和团队归属检查；条件状态提交 |
| 版本与恢复 | 独立要求、计划、应用、状态、执行代次与团队指令版本；启动自动隔离旧代次；未知操作必须核对 |
| DurableJournal | 状态与事件同帧写入，成功前同步；尾部中断恢复；完整损坏拒绝；独占写者锁 |
| Scheduler | 固定线程池、有界队列；同资源键有序；独立键并行；审核与交接预留槽位；异常经 future 返回 |
| 配置与权限 | 每个未设模型/思考字段直接继承请求冻结的 Root 快照；能力不支持时明确拒绝；Worker 与允许列表交集 |
| 候选与交付 | 成果版本不可复用；所有指定审核团队通过；新成果或交付前负面审核使旧批准失效；回执匹配才能完成 |
| 资源状态 | 业务完成与资源关闭独立；未核对操作阻止停止；清理失败可重试且不抹除已成功交付 |

这里的执行、审核证据、交付回执和资源清理都是**合成测试契约**。M1 尚未包含真实模型适配、业务 Shell/文件工具、生产沙箱、MCP/A2A、通用工作流运行器或记忆引擎。

## 构建与运行

需要 Linux、支持 C++20 的 GCC/Clang、Make 和 Python 3.9+。本次实际验证使用 GCC 13.3.0、Make 4.3、Python 3.12.14。运行内核只依赖 C++ 标准库和 Linux 系统接口，无第三方库下载步骤。

```sh
git clone https://github.com/wmqfl861/wls861.git
cd wls861
make -j2 test
./build/wls861 demo
```

演示 JSON 中可核对的摘要字段：

```json
{
  "execution_mode": "synthetic/no_model",
  "model_calls": 0,
  "requests": 2,
  "peak_parallel": 2,
  "all_completed": true,
  "all_resources_closed": true,
  "event_count": 38,
  "temporary_workspace_removed": true
}
```

实际输出还包含每个请求的 Dispatcher、团队、操作、候选版本、审核、回执和事件序列。演示只创建并删除自己生成的临时目录，不接受真实业务目录或持久 Journal 参数。

UBSan 检查使用独立构建目录：

```sh
make -j2 ubsan
```

也提供 CMake 3.20+ / CTest 入口：

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

可通过 `BUILD_DIR`、`CXX`、`CXXFLAGS` 等覆盖 Make 构建设置。严格编译会开启 C++20、线程支持、主要警告和 `-Werror`。GitHub Actions 定义了 Linux Release 与 UBSan 两个检查任务，实际执行状态见 [Actions](https://github.com/wmqfl861/wls861/actions)。

## 验证口径

实际测试命令、环境、结果与原验收条目的覆盖边界见 [M1 验证报告](docs/reports/M1-validation.md)。核心回归包含并发、角色越界、旧版本与旧代次、未知副作用、真实子进程 SIGKILL、部分写失败、审核撤销及额度耗尽后的收尾。

原 51 项拟定验收覆盖整个产品。M1 的内核/模拟测试只能证明其中对应的契约部分，不能把它们全部改成产品验收通过。

## 代码与文档入口

| 路径 | 用途 |
| --- | --- |
| [include/wls](include/wls) | 四角色身份、配置、任务内核、日志和调度器公开接口 |
| [src](src) | C++ 实现，Linux 日志操作独立在 `journal_posix.cpp` |
| [tests](tests) | 模块回归和 CLI 集成检查 |
| [scripts/verify_handoff.py](scripts/verify_handoff.py) | 原始交接文件、SHA、清单和历史验收状态核对 |
| [M1 契约](docs/implementation/M1-contract.md) | 当前接口、身份、状态、容量和信任边界 |
| [交接审阅](docs/implementation/handoff-review.md) | 需求解释、被覆盖旧设计和未决政策 |
| [路线图](docs/ROADMAP.md) | M0—M6 实施顺序、Q01—Q12 与下一批工作 |

## 当前工程限制

M1 是单机受信进程内 API，`Actor` 由宿主构造；这不是外部身份认证或完整操作系统沙箱。任务状态提交目前在一个短互斥区内完成，回调在锁外并行。调度器不会抢占已经运行的回调，永久阻塞或同池循环等待仍需未来执行层处理。

Journal 每次保存状态快照和事件，历史随运行增长；没有压缩、备份、生产保留或安全擦除能力。普通工作有事件与帧容量准入，必要结算、审核撤销、取消、恢复和正常收尾保留通路，但磁盘/设备故障仍可能阻止持久化。禁止将本批临时存储方案当作已批准的生产数据库或长期记忆库。

下一批按照交接顺序进入 **M2 基础执行闭环**：Worker 文件/进程工具、工作区和身份隔离、异步输出与取消、明确端点的模型适配。生产端点、凭据、预算、回执和保留政策在实际接入时单独配置；后续阶段与待定项已在路线图列明。
