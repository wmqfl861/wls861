# S1.3 事件与独立多任务增量实际验证报告

日期：2026-10-19（UTC）。目标蓝图：**v0.4.1**；库/CLI：**0.4.0-s1.3**；原 M1 入口保持 **0.1.1-m1**。开发基线：`94ddb1ae4b850999de9a06b440d05dab19177af5`。

本批沿 S1-03/08/09/10 增加持久事件消费、原子独立多任务计划、逐任务 AMEND、宿主等待/唤醒以及不可变原始反馈。代码、蓝图、任务卡、示例和本报告共同交付。完整问题/回答、提交/审核返工、强沙箱正向、真实模型、业务交付和可恢复清理继续按任务卡实施；工具成功不表示业务已验收或已交付。

**最终 Make 严格门禁和 UBSan 门禁均退出 0：S1 C++ 167/167、S1 实际 CLI 26/26、原 M1 内核/CLI 55/55、Python 参考契约 36/36 全部通过；独立 CMake Release / CTest 22/22 组通过。** 新增 29 个 C++ 组内用例与 10 个实际 CLI 用例；不将多种构建重复计数为产品验收。

## 1. 本批实际接入

| 模块 | 当前行为 | 对应实现 |
| --- | --- | --- |
| SQLite 404 | 非空 403 事务升级、资产指纹、启动待办、独立角色领取、等待和不可变反馈；旧 402/403 接口兼容 | [Store](../../src/store.cpp)、[006 迁移](../../db/migrations/006_event_runtime.sql) |
| 持久事件 | 短事务捕获具体 inbox ID，显式完成子集；遗漏、失败、新到项保持待办；接管释放旧领取并围栏旧身份 | [Store 回归](../../tests/event_store_tests.cpp) |
| 独立计划与 AMEND | 初始计划、任务和团队原子分配；每节点显式 update/keep/cancel；后补遗漏任务使用同一计划 | [EventRuntime](../../src/event_runtime.cpp)、[运行器回归](../../tests/event_runtime_tests.cpp) |
| 等待/唤醒 | 持久 generation 和原绑定；WAITING 阻断新 Worker 预约、prepare/start；精确 wake 不清除 AMEND、applied 或 UNKNOWN 门禁 | [Store 接口](../../include/wls/store.hpp) |
| 原始反馈与上下文 | 原响应、参数、诊断字节、业务/控制结果和错误持久化；Worker 只恢复自己任务历史；缺失明确、容量超限先拒绝预约 | [OfflineRuntime](../../src/offline_runtime.cpp) |
| CLI | 新增 pump-fake、inbox、history、wait、wake；节点目录映射与逐节点处置文件；只读查询不接管 | [CLI](../../src/cli.cpp)、[实际 CLI 回归](../../tests/s13_cli_smoke.py) |

`EventRuntime::pump` 是同步、有限的宿主入口。默认最多 12 次 Fake 调用，每任务最多 1 次 Worker 调用；显式上限分别为 64 和 16，管理调用另受请求冻结配置限制。它不在数据库事务中等待模型或工具，不建立守护进程、永久线程或无限重试。

新 Worker 工具仍经过原 S1.2 的权限、完整绑定、首次持久派发、目录/策略身份和 UNKNOWN 检查。反馈持久化不会授予第二次执行许可。`business_delivered=false`、`environment_checks_executed=false`、`real_model_calls=0` 继续明确输出。

完整约束见 [S1.3 事件运行器契约](../implementation/S1-event-runtime.md) 和 [S1.2 Worker 工具契约](../implementation/S1-worker-tools.md)。

## 2. 实际测试与证据层级

### S1 C++ 与实际 CLI

| 检查组 | 用例数 | 关键范围 |
| --- | ---: | --- |
| Runtime types | 4 | 原严格 JSON、SHA、typed ID 与完整绑定 |
| 原 Store | 30 | 原 402/403、事务、权限、CAS、操作、账本、目录/策略与不可变执行事实 |
| 新 Event Store | 15 | 404 迁移/回滚、部分消费/新到项、并发领取、恢复、原子多任务、AMEND、等待、原反馈与控制容量 |
| Config loader | 15 | 严格来源、完整冻结快照、逐字段继承、恢复和能力拒绝 |
| Model / Context | 15 | 原模型绑定/用量、完整输入帧、稳定前缀和依赖 |
| Schema | 12 | 完整记录与模型输入分开校验、闭合引用、语义门禁参考 |
| 原 Offline runtime | 9 | 原单任务 note、AMEND、迟到响应、预算、取消和恢复 |
| 新 Event runtime | 14 | 真实双任务、遗漏/后补 AMEND、wait、错误/额度、原始/并发响应、跨运行历史与终态待办 |
| Workspace | 19 | 原真实文件、SHA/CAS/patch、路径/链接/容量、并发与故障回归 |
| Sandbox | 17 | 原强模式拒绝、doctor、固定字节与私有进程监督；不包含强隔离正向成绩 |
| WorkerTools | 17 | 原真实文件/Store/Fake、权限与控制、唯一派发、结果持久化失败围栏 |
| **S1 C++ 合计** | **167** | 原 138 项保留，新增 15 + 14 项 |
| 原 S1.1 CLI | 8 | 实际启动 C++ wls 的配置/Schema、note、AMEND、查询与取消 |
| 原 S1.2 CLI | 8 | 实际文件链、强 run 拒绝、只读诊断、身份漂移、失败停批；新建库预期更新为 404 |
| 新 S1.3 CLI | 10 | 两任务文件、原历史、逐任务 AMEND、等待和精确 generation、只读/目录/输入校验、上限与缺脚本 |
| **S1 实际 CLI 合计** | **26** | Python 驱动真实 C++ CLI，与下面的参考契约分列 |

### 原内核、参考契约和来源

| 检查组 | 实际结果 | 证据范围 |
| --- | ---: | --- |
| M1 Journal / Config / Scheduler / TaskEngine | 10 + 5 + 9 + 28 = **52/52** | 原内核状态、持久化、并发、合成审核与回执 |
| M1 CLI | **3/3** | 原 C++ CLI 合成生命周期及输入/错误行为 |
| Python Schema / SQLite 参考 | **36/36** | 13 项 Schema/语义参考、23 项 SQLite 事务参考；Python 无 UBSan 插桩 |
| v0.3 原始交接包 | **33 个文件通过** | 原件/固定清单；51 项原产品验收记录保持未执行 |
| v0.4 原始审核包 | **49 个文件通过** | 固定 SHA-256 清单 |
| 固定上游文件 | **29/29** | 实际字节数与 Git blob SHA-1 匹配原导入清单 |
| 原 schema / 004 / 005 | **字节未变** | 与本批开发基线逐字节比较；新能力只追加 006 |

### 完整门禁

| 门禁 | 最终状态 |
| --- | --- |
| Make 严格完整门禁 | 退出 0；全部上述测试与来源检查通过 |
| Make UBSan 完整门禁 | 退出 0；最终 S1 C++ 167/167、S1 CLI 26/26 及原内核/契约/来源检查通过，无 UBSan 错误 |
| 独立 CMake Release / CTest | 退出 0；最终 22/22 组通过，35.35 秒 |

CTest 的 22 组由 15 个 C++ 组、4 个 CLI 组、1 个 Python 参考契约组及 2 个原件完整性组构成。组内断言、构建模式和不同入口不重复计为独立产品验收；S1 整阶段和原 51 项产品验收没有因此完成。

## 3. 关键路径的实际观察

**双任务文件与原始反馈。** 固定脚本的首次 pump 产生 7 次 Fake 调用：1 次 Dispatcher、两任务各 1 次 TeamLead、各 2 次 Worker。alpha/beta 各自创建 `r1.txt`，随后读取前 6 字节。回归核对两个独立目录、原调用参数、原响应、反馈哈希、完整绑定，以及按 UTF-8 字节长度解析的上下文帧。Worker 历史没有兄弟任务记录，也没有递归嵌入原 compiled context。

**遗漏 AMEND 与后补 keep。** 首轮完成后追加需求修订，只处置 alpha。beta 不产生新的模型调用或文件，旧 applied 值不能使它继续；Dispatcher 的 beta 主题仍未确认。后续只给 beta keep，plan_revision 不增加、没有新的 Dispatcher 调用，beta 取得新 TeamLead 说明和 Worker ack 后才执行。同一 Store 写者实例内的后补处置不改变 alpha 的状态、说明和代次。新 CLI 写者仍执行既有恢复围栏；CLI 示例的 beta 专用后续脚本没有要求 alpha 再次执行业务。

**等待与恢复。** alpha 进入 WAITING 后，重开写者仍保留等待，pump 不为它调用 TeamLead/Worker，而 beta 继续。唤醒必须匹配当前等待 generation；旧 generation 不能唤醒后来新的等待，AMEND 或说明语义改变后旧等待失效。等待直接进入 Store 预约、准备、开始门禁，绕开 EventRuntime 也不能执行新业务。已有诊断、取消、确认及结果结算仍走各自通路；wake 后 UNKNOWN 仍阻断新副作用。

**逐项消费与故障恢复。** 回归让领取期间产生新事件，只完成捕获 ID 的明确子集；新到项和遗漏项没有被标记完成。并发领取只有一个拥有者；接管释放旧 claim 并拒绝旧 token/generation。对第二个团队创建注入 SQLite 失败时，计划、两个团队、任务、事件全部回滚，没有半个计划。对反馈事件写入注入失败时，反馈与其事件一起回滚，原 attempt/操作事实保持可查。

**重放、并发与缺失观察。** 读取后改变实际文件，再次接收原模型响应仍返回原诊断字节。两个 OfflineRuntime 使用 barrier 同时接收同一 64 KiB 写响应，得到完全相同的原结果，只有一条反馈事件、一条操作结果事件和一份文件效果。若响应已结算而反馈尚未持久化，重复接收明确拒绝，不重新读取文件或派发旧工具来补历史。

**容量与控制。** 缺脚本组、总调用上限、冻结管理额度和模型预算满都保留尚未完成的具体待办；上下文无法完整容纳历史时，不新增 attempt。将普通事件记录填至 4096 后，正常业务变更被拒绝，但结算、原反馈、取消、迟到事实记录和唤醒仍成功使用既有控制预留。该回归没有模拟物理磁盘耗尽。

## 4. 交叉审查与修正

- 新运行器补全 SUPERSEDED 终态识别；pending 汇总使用请求范围读取，终止团队的遗留待办仍如实可见，不使终态重新执行。
- 重复响应路径改为读取原始不可变反馈，按 attempt 分离的有界进程互斥覆盖并发接收；已结算但缺反馈明确保留缺失。
- 管理上下文包含当前预算快照；Worker 历史严格限定原角色、任务、团队和身份，保持原绑定。
- CLI 区分“未给数字参数”与“显式空数字”；超长 wait reason 和纯空白 AMEND rationale 在 writer 或模型预约前拒绝。目录同一 identity、嵌套、符号链接、相对路径、重复 JSON key、DB 位于业务目录内均有前置回归。
- 文档与旧入口的交叉审查发现 `run-fake` 自动重发说明可能把 WAITING 置为 SUPERSEDED。旧入口已在任何自动重绑定、说明或模型预约之前增加等待检查，并用真正单任务的 C++ 和跨进程 CLI 回归验证；显式 wake 后仍需正常的新说明和 ack。
- 回归辅助函数修正 FTS5 shadow table 无 rowid 的快照比较；额度夹具从真实临时 YAML 加载完整冻结依赖，未直接篡改冻结快照来绕过完整性校验。

Store、运行器、CLI 分别做了跨模块只读审查，范围包括 catch 后释放、旧 claim、AMEND 主题隔离、新到事件、原响应回放、只读不变和目录预检。最终判断以本报告记录的稳定源码完整门禁为准。

## 5. 环境与可复现命令

本次 Linux 环境为 Ubuntu 24.04.3 LTS、内核 6.18.55、GCC 13.3.0、CMake 3.28.3；实际依赖为 Boost 1.83.0、C++ SQLite 3.45.1、OpenSSL 3.0.13、LibYAML 0.2.5。测试虚拟环境 Python 3.12.14、jsonschema 4.10.3；Python sqlite3 模块为 3.53.1，与 C++ 链接版本分开记录。

恢复工作时系统缺少构建开发包，旧测试虚拟环境解释器路径也已失效；补齐仓库要求的依赖并恢复虚拟环境后验证。共享 build 中曾出现空事件对象、首部损坏的测试产物和测试 ELF 缺少执行位；逐个确认具体生成目标后移除损坏产物重编或恢复执行位，未改变源码门禁。UBSan 与 CMake 使用新的独立构建目录；没有降低警告、关闭测试或修改宿主沙箱政策。

实际完整命令在仓库根目录执行：

```sh
python3 -m venv .local/test-venv
.local/test-venv/bin/python -m pip install -r requirements-test.txt

make -j2 test PYTHON=.local/test-venv/bin/python
make -j2 ubsan BUILD_DIR=/tmp/wls-s13-validation PYTHON=.local/test-venv/bin/python

cmake -S . -B /tmp/wls-s13-cmake -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/absolute/repo/.local/test-venv/bin/python
cmake --build /tmp/wls-s13-cmake --parallel 2
ctest --test-dir /tmp/wls-s13-cmake --output-on-failure
```

Make 继续强制 `-std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`。UBSan 增加 `-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`，并设置 `halt_on_error=1:print_stacktrace=1`。

证据文件：

- [最终 Make 完整门禁](s1.3-make-test.txt)
- [最终 UBSan 完整门禁](s1.3-ubsan-test.txt)
- [独立 CMake 配置、构建和 CTest](s1.3-ctest.txt)
- [活动源码、构建、配置、契约与文档 SHA-256](s1.3-source-SHA256SUMS.txt)

最终日志将当前工作区绝对路径归一化为 `<repo>`，保留独立 `/tmp/wls-s13-*` 构建路径，并去掉行尾空白。Make 日志为稳定源码完成增量重建后的全套测试；CMake 日志从独立目录配置开始，后接 WAITING 修正后的重新构建和完整 CTest；最终结果取最后一轮。源码清单覆盖 150 个活动文件，不包含报告/日志、自身、原始交接档案和固定上游字节；后两类分别按原固定清单核对。可在仓库根目录执行 `sha256sum -c docs/reports/s1.3-source-SHA256SUMS.txt` 复核本批活动文件。

本报告记录推送前本地证据；GitHub 上同一提交的 strict 与 UBSan Actions 结果在推送后独立核验。旧报告与旧源码清单保留原提交的历史含义。

## 6. 保留边界与下一步

本批没有真实供应商调用或费用，没有部署 wlsd、套接字/API、远程认证、全局审核双容量并发调度、Asio/strand、完整 DAG 或长期后台 Worker。宿主 wait/wake 也不是完整 ask/逐级问题/ANSWER 协议。

S1.2 的强 bwrap/cgroup 入口与拒绝行为保持，正向强隔离、后代逃逸、历史残留 cgroup 恢复、真实 input 挂载及资源清理仍需要相应环境与独立证据；没有为了通过测试启用弱生产路径。

下一增量复用已落地的事件与原始反馈，接问题/回答、Worker submit 和 TeamLead 检查/返工，然后推进不可变成果、回执、经验与可恢复清理。当前 51 项产品验收、S1 整阶段、业务接受或 DELIVERED 状态不因本批用例通过而自动完成。
