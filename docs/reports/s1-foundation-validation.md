# S1 基础增量（S1.1）实际验证报告

日期：2026-10-17（Asia/Taipei）。目标蓝图：**v0.4.1**；新库／CLI：**0.2.0-s1.1**；原 M1 入口继续为 **0.1.1-m1**。
开发基线：`e78cbfb4acf45af73e9edc1ddb014d1bba7a909d`。

**本批已经把 C++ SQLite Store、严格配置与 Schema、FakeModel、ContextCompiler 和本地 CLI 接成可持久化的单任务离线流程。最终严格构建与 UBSan 门禁均通过新增的 72 项 C++ 和 8 项实际 CLI 检查；原 55 项 M1 内核／CLI 检查、36 项 Python 契约参考及两套来源完整性检查也全部通过。CMake Release／CTest 为 15 / 15 组通过。**

这些结果属于 S1 基础层的模块与离线集成证据。完整 S1 的真实工具、沙箱、供应商、守护进程、业务验收／交付和可恢复清理尚未完成。原 51 项产品验收保持历史“未执行”，不同构建模式、内部子断言和 CTest 分组不重复计为新产品用例。

## 1. 本批交付的实际行为

| 模块 | 已接入行为 | 对应实现 |
| --- | --- | --- |
| 基础类型与 Schema | 严格 JSON、内部规范序列化、SHA-256、typed ULID、完整执行绑定；Draft-07、本地封闭引用、完整记录与模型输入分别校验 | [runtime_types.cpp](../../src/runtime_types.cpp)、[schema.cpp](../../src/schema.cpp) |
| SQLite Store | 真正的 C++ WAL/FULL/FK、短事务、CAS、状态与事件原子提交、独占写者和只读模式；请求原文、任务、说明、AMEND、操作与预算持久化 | [store.cpp](../../src/store.cpp)、[402 迁移](../../db/migrations/004_s1_store.sql) |
| 配置与模板 | LibYAML 严格解析；Root 与角色选择、全部提示词／模板依赖及原始内容一起冻结；快照恢复和字段来源解释 | [config_loader.cpp](../../src/config_loader.cpp) |
| 模型与上下文 | 有界、可取消的 FakeModel；长度分帧的实际输入及缓存身份；完整 ModelRequest 与原始绑定先写账本，每次 attempt 独立预留和结算 | [model.cpp](../../src/model.cpp)、[context.cpp](../../src/context.cpp) |
| 离线运行器与 CLI | 一次最多 Dispatcher、TeamLead、Worker 各一次；当前说明确认后执行持久 note；支持提交、查询、AMEND 显式处置、重开和取消 | [offline_runtime.cpp](../../src/offline_runtime.cpp)、[cli.cpp](../../src/cli.cpp) |

新请求先冻结配置，再提交计划、任务和 TeamLead 说明；Worker 必须应用当前说明才能执行 note。模型原文、上下文、工具参数、调用／尝试 ID、原始十字段 binding、响应和 usage 均可追溯。note 成功后仍保持运行状态，输出 `business_delivered=false`，不伪造业务完成。

Store 是受信宿主 API，CLI 是本机数据库所有者的管理入口。模型参数不构造宿主身份。配置准入、模型预约和操作派发分别核对已冻结模型／模板、角色和工具授权；非法模型输出即使已经消耗用量，也不能直接成为计划、说明或操作。

完整行为和限制见 [S1 基础实现契约](../implementation/S1-foundation.md)，后续拆分见 [S1 任务卡](../spec/v0.4.1/07_S1任务卡.md)。

## 2. 最终实际检查结果

### 新增 S1 C++ 与实际 CLI

| 检查组 | 结果 | 关键证据 |
| --- | ---: | --- |
| Runtime types | 4 / 4 | 解码后重复 JSON 键、非有限值与容量限制；规范字节／SHA；typed ULID 与绑定类型 |
| SQLite Store | 17 / 17 | 真正 SQLite 配置、401→402 空库迁移、未知库保留、写租约、CAS 竞争、事务回滚、完整模型输入、AMEND、UNKNOWN、重开与 SIGKILL |
| Config loader | 15 / 15 | 重复／未知键、歧义值、能力和政策、冻结 Root 继承、快照篡改、路径及符号链接、依赖修改后原快照稳定、环境 check 不执行 |
| Model / Context | 15 / 15 | 原始绑定、角色／序号、延迟／取消／中断、nullable usage、稳定前缀和缓存依赖、完整帧恢复、重新计算 hash 也不能伪造角色／工具元数据 |
| Schema | 12 / 12 | 13 份完整示例、模型禁止宿主字段、嵌套业务字段与条件、TaskBrief、本地 JSON Pointer、恶意／未访问引用提前拒绝、100 条 findings |
| Offline runtime | 9 / 9 | 三角色持久流程、非法约束不发布、伪造宿主字段、AMEND 迟到结果和 keep、未知预算与取消、只读不围栏、计划发布后重开 rebind、显式 cancel 处置、输入路径 |
| **新增 C++ 合计** | **72 / 72** | 普通严格构建和 UBSan 构建均通过 |
| **实际 `wls` CLI** | **8 / 8** | Python 通过 subprocess 启动 C++ 可执行文件；覆盖配置／Schema、幂等、只读、实际 SQLite 记录、AMEND、配置源移除后恢复、预算耗尽仍能取消、参数错误 |

CLI 测试是 C++ 程序与真实临时 SQLite 文件的集成驱动，与下面的 Python 参考实现分开统计。测试中的 Fake 响应、note 和 usage 都是显式夹具；没有真实模型供应商或文件／进程业务工具调用。

### 既有回归与构建入口

| 检查组 | 结果 | 范围 |
| --- | ---: | --- |
| 原 M1 Journal / Config / Scheduler / TaskEngine | 10 + 5 + 9 + 28 = **52 / 52** | 原持久化、身份、版本、UNKNOWN、有界调度、候选审核、回执与资源状态回归 |
| 原 `wls861` CLI | **3 / 3** | 原演示、版本／帮助、错误处理；与原 C++ 合计 55 项 |
| Python Schema／语义参考 | **13 / 13** | 原 12 项保留，增加宿主字段投影作用域回归 |
| Python SQLite 事务参考 | **23 / 23** | 原 CAS、AMEND、撤权、版本化记忆及持久待办参考；不冒充 C++ 对应运行器 |
| Python 契约合计 | **36 / 36** | 普通与 UBSan 门禁使用同一 Python；Python 本身未做 UBSan 插桩 |
| v0.3 原始包 | 通过 | 33 个原文件、固定清单和 SHA-256；原验收状态保留 |
| v0.4 审核原始包 | 通过 | 49 个原文件逐一匹配固定 SHA-256 清单 |
| 固定上游依赖 | **29 / 29** | LibYAML／Valijson 导入文件的字节数与 Git blob SHA-1 均匹配固定上游原件 |
| 本地 Make 严格门禁 | 通过，退出码 0 | C++20、主要警告及 `-Werror`，运行上述全部测试 |
| 本地 Make UBSan 门禁 | 通过，退出码 0 | `-fsanitize=undefined -fno-sanitize-recover=undefined`，同组全部测试 |
| 本地 CMake Release／CTest | **15 / 15 组** | 10 个 C++ 组、两个 CLI、Python 参考、两个来源校验组 |

Store 的崩溃检查实际启动子进程并发送 `SIGKILL`，验证已提交 SQLite 数据保留和未提交事务回滚。它没有模拟整机断电。副作用 UNKNOWN 测试使用受信宿主提交的合成操作事实，不能据此宣称真实文件工具已经通过恢复验收。

## 3. 交叉审查修复及有效回归

- **持久输入不能被简化成摘要。** Store 预约必须有完整 ModelRequest，原始绑定、配置 SHA、call/attempt、角色、冻结 model/reasoning/adapter/protocol 和预算必须一致。Worker 工具必须落在冻结模板、请求授权和当前政策的交集。缺少输入或越权时，事务不增加预算或事件。
- **非法模型计划不应先发布再失败。** 宿主先做模型输入 Schema、单任务范围和全量显式约束检查；Store 提交处再次验证。非法文本的调用照常记账，但不会留下半发布计划。
- **模型输入投影不能削弱嵌套业务规则。** C++ 与 Python 统一按实例对象作用域处理 `x-runtime-field`；同名普通业务字段仍为必填，业务条件与 `const` 数据不被误删。被移除宿主字段仍明确禁止模型提交。
- **恢复的上下文不能信任可改元数据。** 逐帧有界解析，再复用 Compiler 重建并比对实际字节；清空工具列表、改变角色并重新计算 hash、截断、追加或溢出长度均被拒绝。
- **未知费用和迟到结果保持原事实。** 重开将 PENDING 调用记为 UNKNOWN，保留未知维度预约，不自动退款或重试。旧需求／旧代次响应仍记账，但不能写回当前业务状态。
- **查询和控制不会冒充恢复完成。** ReadOnly 命令不迁移、接管或推进代次；AMEND 遗漏任务保持暂停，keep 也要新说明与 ack；预算耗尽仍可走有界取消，确认不会消除 UNKNOWN。
- **文件来源与版本明确。** 配置和 CLI 输入沿路径逐级拒绝符号链接及特殊文件；未知 SQLite 文件在持久 journal-mode 改动前拒绝。adapter_version 和 protocol 是必填冻结字段，当前 Fake profile 的 streaming 启用明确拒绝。

独立文档复核还修正了活动蓝图中关于“只有 M1 CLI”“配置加载器尚未实现”和“全部 C++ 无第三方”的旧表述。README、路线图、任务卡、验收计划、需求追踪与新契约统一采用 S1.1 的实际范围，历史报告与两套原件没有被重写。

## 4. 环境、命令和可复核文件

| 项目 | 本批实际环境 |
| --- | --- |
| 平台 | Linux 6.18.55，x86_64，glibc 2.39 |
| 编译器／构建 | GCC 13.3.0；GNU Make 4.3；CMake／CTest 3.28.3 |
| C++ 依赖 | Boost 1.83.0；SQLite 3.45.1；OpenSSL 3.0.13；LibYAML 0.2.5；固定 Valijson v1.0.6 |
| Python 参考驱动 | `/usr/bin/python3` 3.12.3；jsonschema 4.10.3；SQLite 3.45.1 |
| 数据与模型 | 临时目录／数据库、显式 Fake 响应、固定文案和合成用量；真实供应商调用 0 次 |

本次实际命令：

```sh
make -j2 test PYTHON=/usr/bin/python3
make -j2 ubsan PYTHON=/usr/bin/python3
cmake -S . -B build-cmake-s1 -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build-cmake-s1 --parallel 2
ctest --test-dir build-cmake-s1 --output-on-failure
```

UBSan 使用 `halt_on_error=1:print_stacktrace=1`。一次普通构建的预检遇到本地 `build/wls` 为 `0644` 导致 CLI 无法启动；重新链接生成 `0755` 后，完整 Make 门禁退出 0，CMake 和 UBSan 也分别通过。没有通过跳过 CLI、降低警告或改动测试预期来通过门禁。

最终门禁输出及源文件指纹：

- [Make 严格测试日志](s1.1-make-test.txt)
- [Make UBSan 测试日志](s1.1-ubsan-test.txt)
- [CMake Release／CTest 日志](s1.1-ctest.txt)
- [活动源码、构建、配置、契约与文档 SHA-256](s1.1-source-SHA256SUMS.txt)
- [固定依赖来源、许可证与字节清单](../../third_party/README.md)

日志仅将工作区绝对路径归一化为 `<repo>`。源码清单不包含自身和报告／日志；两套历史原件由各自固定清单独立校验。可以在仓库根目录执行 `sha256sum -c docs/reports/s1.1-source-SHA256SUMS.txt` 复核本批源码内容。日志包含当次增量构建和实际测试结果，不能据测试耗时推断产品性能。

[GitHub Actions 工作流](../../.github/workflows/ci.yml) 对 Ubuntu 24.04 的 strict Release 与 UBSan 两种 CMake 构建运行相同 15 组 CTest。本报告记录推送前实际本地成绩；远端结果以 [Actions 中对应提交](https://github.com/wmqfl861/wls861/actions) 为准，不预填尚未执行的 CI 成绩。

## 5. 下一增量与保留边界

优先沿 **S1-06/07** 接入真实受控文件与进程工具、输出／超时、路径与环境边界，以及 bubblewrap/cgroup 的实际能力检查；每次副作用继续经过现有 Store 准备、派发、完成、UNKNOWN 和核对关口。随后再接持久事件消费和角色循环、交付／回执／清理、wlsd/API。

当前只运行一个独立任务，不执行完整 DAG、审核返工、跨能力调用或自动循环。一次 CLI 写者持有独占数据库租约，没有第二个进程通过 socket 取消在途 Fake 调用的服务；已测同进程取消与重开后的控制不能替代该验收。

`call_attempts.accepted` 表示原绑定在结算时仍有资格且传输完成，不表示 Schema、工具或业务验收通过；实际采用要检查随后对应的计划、说明、操作事件和状态。约束目录是入口显式提供的当前约束集合，逐字核对不构成对任意自然语言需求的完整语义证明。

本批没有测试真实供应商、真实 shell/fs 工具、OS 沙箱隔离、整机断电、ASan、非空旧数据迁移、生产部署、版本化业务交付、记忆执行或真实清理。开发和 push 授权不被解释成模型费用或部署授权；这些能力按后续任务卡提供独立证据。
