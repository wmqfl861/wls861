# S1.2 Worker 工具增量实际验证报告

日期：2026-10-18（UTC）。目标蓝图：**v0.4.1**；当前库/CLI：**0.3.0-s1.2**；原 M1 入口保持 **0.1.1-m1**。开发基线：`c0206fcd02a35f83218691cd34c50637621e88d1`。

本批沿 S1-06/07 把真实受控文件工具接入 C++ Store、原始模型调用和有界 Worker 流程，并增加仅强模式的进程入口。代码、蓝图、任务卡和验证证据随本批一并提交。**完整 S1、强 bwrap/cgroup 正向、真实供应商、业务验收/交付和可恢复清理仍未完成。** 文件成功或结果 `accepted=1` 不表示业务交付。

**最终 Make 严格门禁和 UBSan 门禁均退出 0：当前 S1 C++ 138/138、S1 实际 CLI 16/16、原 M1 内核/CLI 55/55、Python 参考契约 36/36 全部通过。CMake Release/CTest 19/19 组通过；两套交接原件和 29 个固定上游文件完整性核对通过。** 相对 S1.1，本批增加 66 个 C++ 测试组内用例和 8 个 CLI 用例；这些数量不等于完整产品验收数。

## 1. 本批实际接入的行为

| 模块 | 已接入行为 | 对应实现 |
| --- | --- | --- |
| 有界文件工具 | 固定目录身份；dirfd/openat2 路径约束；读取/列表/字面搜索；只新建、SHA 覆盖与全文件唯一匹配 patch；暂存、原子替换和同步证据 | [Workspace](../../src/workspace_posix.cpp)、[文件回归](../../tests/workspace_tests.cpp) |
| SQLite 403 | 已知非空 402 库事务升级；不可变团队目录/策略；首次派发领取；原始 tuple、结果/证据、二进制 stdout/stderr 与哈希原子持久化 | [Store](../../src/store.cpp)、[005 迁移](../../db/migrations/005_worker_tools.sql) |
| 工具注册与控制 | 宿主静态 11 工具；当前权限交集；原 key 只读重放；独立诊断/ack/取消；结果落盘失败阻断新业务 | [WorkerTools](../../src/worker_tools.cpp)、[集成回归](../../tests/worker_tools_tests.cpp) |
| 进程入口 | 冻结 launcher/runtime SHA、sealed 运行文件、显式环境、整树检查、bwrap/cgroup、启动证明、有界输出/超时/取消和后代证明 | [ProcessRunner](../../src/sandbox_posix.cpp)、[沙箱回归](../../tests/sandbox_tests.cpp) |
| FakeModel 与 CLI | 显式工作目录和 manifest；一次运行 1–16 个 Worker 调用；完整工具反馈进入后续上下文；只读 tool-query 和 doctor | [运行器](../../src/offline_runtime.cpp)、[CLI](../../src/cli.cpp)、[CLI 回归](../../tests/s12_cli_smoke.py) |

原 note 路径、M1 Journal 与演示继续保留。每次真实文件业务先保存意图和原版本，只有首次 `dispatch_claimed=true` 的调用执行；参数相同的旧 key 可读回原结果，不能作为再次执行许可。AMEND、取消或恢复后的迟到事实保存原身份，不重盖当前版本或自行解除 UNKNOWN。

工作目录由宿主明确提供，目录身份和完整执行策略首次绑定后不可漂移；本 Store 的控制数据库/租约/WAL/SHM 必须在业务目录外。模型参数不能提供宿主 actor、类别、挂载、环境或 cgroup 配置。当前权限由冻结模板、请求 allowlist 与最新撤销政策取交集。

完整行为与限制见 [S1.2 实现契约](../implementation/S1-worker-tools.md)，当前余项见 [S1 任务卡](../spec/v0.4.1/07_S1任务卡.md)。

## 2. 最终实际检查结果

### 当前 S1 C++ 与实际 CLI

| 检查组 | 最终结果 | 关键范围 |
| --- | ---: | --- |
| Runtime types | 4 / 4 | 原严格 JSON、SHA、typed ID 与绑定回归 |
| SQLite Store | 30 / 30 | 原 17 项保留；新增非空 402→403/回滚、workspace/策略绑定、唯一领取、输出 BLOB、原 tuple、控制重放、迟到事实与只读兼容 |
| Config loader | 15 / 15 | 原配置容量、严格来源、冻结恢复与能力拒绝回归 |
| Model / Context | 15 / 15 | 原模型绑定/用量及完整上下文帧、稳定前缀和依赖回归 |
| Schema | 12 / 12 | 原完整记录/模型输入投影、封闭本地引用和业务规则回归 |
| Offline runtime | 9 / 9 | 原 note 流程、AMEND、迟到调用、预算、取消和恢复兼容 |
| Workspace | 19 / 19 | 真实文件正例、分页/搜索、SHA 覆盖、全文件唯一 patch、字节/容量/路径限制、链接与特殊文件拒绝、跨进程 CAS、提交边界故障及 openat2 不可用拒绝 |
| Sandbox | 17 / 17 | 生产强模式拒绝与 doctor；固定运行字节、可信子程序监督、输出/超时/取消/启动屏障、最终后代查询失效、mountinfo 解析和临时不安全树拒绝；无强模式正向成绩 |
| WorkerTools | 17 / 17 | 真实文件与 Store/Fake 集成、权限/撤销、原 key 幂等/并发、AMEND/UNKNOWN、独立控制、manifest、完整反馈帧，以及全结算兜底失败后的确定性并发围栏 |
| **当前 S1 C++ 合计** | **138 / 138** | 严格构建与 UBSan 均通过；含 S1.1 的原 72 项与本批 66 项 |
| 原 `wls` CLI 驱动 | 8 / 8 | 保留配置/Schema、请求/快照、note、AMEND、查询和取消等集成 |
| 新 S1.2 CLI 驱动 | 8 / 8 | doctor/403、5 次 Fake 调用的真实写/读/patch、只读不改状态、路径/分类反例、强 run 拒绝无 spawn、目录/策略漂移、失败停批与参数拒绝 |
| **S1 实际 CLI 合计** | **16 / 16** | 两个 Python 驱动实际启动 C++ `wls`，与参考契约分列 |

新 CLI 固定脚本产生 `verified from S1.2\n`，并保留两个业务操作的原始结果、读取反馈和全部 5 个模型 attempt；`business_delivered=false`，实际供应商调用为 0。诊断查询后预算、事件、任务状态与执行代次不变。

并发故障回归在测试 TU 编译实际 WorkerTools 源码，只增加准备返回后的私有观察点；生产对象没有该钩子或公开测试入口，CLI 回归链接正常库。测试暂停已 PREPARED 的 B，让 A 真正写入文件，再用 SQLite trigger 使完整结果、最小 UNKNOWN 事实和 UNKNOWN 状态全部失败。恢复 SQLite 后释放 B，确认 B 仍 PREPARED、未领取、无文件副作用；新实例也被围栏阻断，诊断与旧 key 读取仍可用。它没有模拟物理磁盘耗尽或整机掉电。

### 既有回归、构建与完整性

| 检查组 | 最终结果 | 证据范围 |
| --- | ---: | --- |
| 原 M1 Journal / Config / Scheduler / TaskEngine | 10 + 5 + 9 + 28 = **52 / 52** | 原内核的持久化、状态、合成执行、调度、审核与回执回归 |
| 原 `wls861` CLI | **3 / 3** | 与原 C++ 合计 55 项 |
| Python Schema/SQLite 参考 | **36 / 36** | 13 项 Schema/语义参考、23 项 SQLite 事务参考；Python 本身没有 UBSan 插桩 |
| v0.3 原始交接包 | **通过，33 个文件** | 固定原件/清单/哈希；原验收记录保持未执行 |
| v0.4 原始审核包 | **通过，49 个文件** | 全部匹配固定 SHA-256 清单 |
| 固定上游导入文件 | **29 / 29** | 字节数与 Git blob SHA-1 均匹配 `third_party/upstream_files.json` |
| Make 严格完整门禁 | **通过，退出码 0** | 上述全部实际测试及来源检查 |
| Make UBSan 完整门禁 | **通过，退出码 0** | 同组全部实际测试及来源检查，无 UBSan 错误 |
| CMake Release / CTest | **19 / 19 组，退出码 0** | 13 个 C++ 组、3 个 CLI 组、Python 契约与两套来源校验 |

不同构建模式、内部断言和 CTest 分组不重复计为产品验收用例；原 51 项产品验收记录保持历史未执行。实际文件/SQLite/CLI 测试、私有进程机制与强隔离正向分别记录，不混合证据层级。

## 3. 交叉审查发现与修复

**命令重放不能授予第二次派发权。** Store 单独返回首次 PREPARED→RUNNING 的领取结果；同 key、同 command、多实例并发只有一位执行者。当前权限允许读取原 key 时仅返回旧事实，即使说明或执行代次已变化，也不套用当前 tuple 重做副作用。

**文件完成与数据库结算之间必须保守处理。** 结果事件写入失败时，不向调用者返回成功，尝试保存小型 UNKNOWN 事实/状态，并设置当前团队/代次的进程内故障围栏。最后一次围栏检查和首次 claim 与结果保存/故障登记共用准入互斥边界，防止早先通过检查、尚未派发的并发调用穿过新围栏。它不把文件系统和 SQLite 描述为一个原子事务。

**patch 的明确位置不能替代唯一匹配。** 完整旧 SHA、指定位置和原字节全部匹配后，仍扫描整个旧文件；重复片段、重叠匹配、重复 NUL 和空 expected 在暂存前拒绝，原文件不变。插入使用唯一相邻原文作为锚点。

**只固定工作区根 inode 不足以约束递归目录挂载。** 执行前的有界 dirfd/statx 整树检查拒绝预存硬链接、符号链接、FIFO/socket/device、不安全 owner/mode 和不同 mount ID。启动屏障内还校验实际 mountinfo，拒绝 `/work` 子挂载和传播属性；不会修改宿主挂载或以文件代理的 NO_XDEV 代替进程挂载证明。

**旧的空 cgroup 观察不能冒充最终后代证明。** 每次查询前清除先前观察；最终读取失败时保持未确认，保留 UNKNOWN 和合作锁围栏。启动屏障前及最终放行字节写入前都检查取消/截止时间。launcher 的空 exec-error EOF 还需真实 bwrap 启动记录佐证，避免把 exec 前 SIGKILL 误称为执行证明。

**原输出必须能无损且有界地结算。** stdout/stderr 以独立 BLOB 持久保存，页面返回 base64；结果 JSON 不重复容纳完整展示文本和编码输出，避免大量控制字节转义后超过结果限额。UNKNOWN 迟到事实与 original binding 不因分页或展示而改变。

**后续上下文需要完整调用信封。** 单独回填诊断返回值会丢失未被工具原样返回的参数。运行器现在把原 attempt_id、完整模型响应中的工具名/参数及完整工具结果一起追加；超过上下文容量明确阻塞，不削减原参数或结果。

**控制重试使用原命令身份。** 旧 ack 重放不会确认后来发布的新说明；取消重试不会读取新 op version 去改变原命令摘要。诊断/控制独立检查身份、权限及速率，不消耗业务额度。只读 402 不迁移，403 查询不接管写者或改变代次。

文档复核同步了 README、路线图、主蓝图、ADR、任务卡、验收计划、需求追踪与新实现契约。S1.1 历史表和报告明确保留当时范围；两套交接原件和原产品验收状态没有重写。

## 4. 环境和复现方式

| 项目 | 本批实际环境 |
| --- | --- |
| 平台 | Ubuntu 24.04.3 LTS；Linux 6.18.55；x86_64；glibc 2.39 |
| 编译/构建 | GCC 13.3.0；GNU Make 4.3；CMake/CTest 3.28.3 |
| C++ 依赖 | Boost 1.83.0；SQLite 3.45.1；OpenSSL 3.0.13；LibYAML 0.2.5；固定 Valijson v1.0.6 |
| Python 驱动 | `.local/test-venv/bin/python` 3.12.14；jsonschema 4.10.3；Python SQLite 3.53.1 |
| 输入/数据 | 自建临时目录、文件和 SQLite 库；显式 Fake 响应与合成用量；真实模型调用 0 次 |
| 强环境观察 | bwrap 文件可读、内核支持 cgroup v2；默认清单未冻结 launcher SHA、未提供运行文件或可用委派根；强执行前提不满足 |

Python 参考测试与 C++ Store 链接不同版本的 SQLite，分别记录。本环境的 `/usr/bin/python3` 没有 jsonschema，首轮 Make 在 Python 契约检查处停止；随后按仓库固定要求创建独立测试环境再跑完整门禁，没有跳过检查或降低预期。没有安装服务、配置模型凭据、启用弱隔离或改变 user namespace/AppArmor/cgroup 宿主政策。

最终重跑期间还观察到 `build/wls` 与 `build-ubsan/store_tests` 两个生成物为 0644，且文件头前 32 bytes 为零；恢复执行位后仍有 exec-format 错误。仅移除这两个损坏构建产物并按原 Make 目标重新链接，得到有效 ELF 后完整门禁退出 0；新目录 CMake 构建也独立通过。损坏原因未确定，没有因此改动源码预期或将生成二进制加入 Git。

实际复现命令：

```sh
python3 -m venv .local/test-venv
.local/test-venv/bin/python -m pip install -r requirements-test.txt
make -j2 test PYTHON=.local/test-venv/bin/python
make -j2 ubsan PYTHON=.local/test-venv/bin/python
cmake -S . -B build-cmake-s12 -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$PWD/.local/test-venv/bin/python"
cmake --build build-cmake-s12 --parallel 2
ctest --test-dir build-cmake-s12 --output-on-failure
./build/wls doctor --source-root .
```

Make 严格编译保持 C++20、`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`。UBSan 使用 `-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer` 和 `halt_on_error=1:print_stacktrace=1`。没有执行 ASan 或整机断电测试。

最终可复核输出：

- [Make 严格门禁](s1.2-make-test.txt)
- [Make UBSan 门禁](s1.2-ubsan-test.txt)
- [CMake Release/CTest 门禁](s1.2-ctest.txt)
- [只读 doctor 观察](s1.2-doctor.json)
- [活动源码、构建、配置、契约与文档 SHA-256](s1.2-source-SHA256SUMS.txt)
- [固定依赖来源、许可证和字节清单](../../third_party/README.md)

日志将当前工作区绝对路径归一化为 `<repo>`，并去掉行尾空白；实际命令与退出状态另在本报告记录。Make 日志来自最终增量重建后的完整门禁，CMake 日志包含配置、最终增量构建和 CTest。源码清单不包含报告/日志、自身、交接档案及固定上游导入文件；后两类使用各自固定清单独立核对。源码清单可在仓库根目录用 `sha256sum -c docs/reports/s1.2-source-SHA256SUMS.txt` 复核。旧报告中的源码指纹属于旧提交，不能拿来要求后续实现保持原字节。

[GitHub Actions 工作流](../../.github/workflows/ci.yml) 在 Ubuntu 24.04 分别进行 strict Release 与 UBSan 构建，运行相同 19 组 CTest。本报告记录推送前的实际本地门禁；远端成绩以 [Actions 中对应提交](https://github.com/wmqfl861/wls861/actions) 为准，不预填未运行的 CI 结果。

## 5. 证据范围与后续开发

**强隔离正向未执行。** doctor 是只读前提观察，保留 `programs_executed=0`、`namespace_execution_tested=false` 和 `process_isolation_verified=false`。实际文件工具运行在有界宿主代理中；沙箱测试中的固定可信子程序通过私有监督入口运行，不向模型或生产导出弱模式。挂载反例使用 mountinfo 解析夹具；socket 测试只创建临时 socket inode，没有监听或发送数据。它们不能替代真实 bwrap/cgroup、网络/凭据/后代逃逸正向验收。

**恢复和资源仍有明确余项。** 已启动但非零退出、超时、取消、输出超限、后代不确定，或同 op_id 残留 cgroup，即使本次没有新 spawn，也保守保留 UNKNOWN。fork 后非局部异常展开只尽力终止 cgroup，未完成覆盖全部路径的直接子进程回收；runner 对象销毁后合作锁会释放。跨进程恢复仍依赖持久状态与后续核对，历史残留扫描/回收执行器尚未接入。文件容量是逻辑边界，未建立进程磁盘配额；合作锁不防任意同 UID 宿主绕过。

当前运行仍限一个独立任务和显式有界批次，同次调用内追加工具反馈；没有持久事件消费、等待/唤醒、跨运行完整历史、后台 run 或跨 CLI 取消。`/input` 目前为空只读目录。没有真实成果包、验收回执、经验/清理作业、真实模型协议、memory 或 MCP 运行器。

下一增量沿 **S1-03/09/10** 接持久事件、Worker 等待/唤醒、跨运行历史和多任务 AMEND；S1-06/07 继续补强环境正向、残留恢复和真实输入挂载；随后接 S1-11/12 交付包、回执与可恢复收尾。每项能力单独给出实际证据，完整闭环满足后再标 S1 完成。
