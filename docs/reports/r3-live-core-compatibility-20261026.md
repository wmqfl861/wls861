# R3 受控工作流核心兼容专项实测

日期：2026-10-26（America/Los_Angeles）。目标版本：
`0.10.0-r3.3c-runtime`，Store 410。开发基线：
`1c572d3aa9121e85cf22aa1e4bb330b25a34a143`。

本报告只记录模块 A 核心库开发代理实际执行的专项验证和只读审查。
测试对象是该基线之上的双模块开发树快照，不是一个已提交的最终 SHA。
新 loopback 工作流的原生调用、用量夹具、AMEND、返工及取消反例由
[live-workflow-evidence-validation.md](live-workflow-evidence-validation.md)
单独记录；最终整树和 CI 验收由整合报告记录。本文不把尚未观察的结果计作通过。

## 1. 已实现及复核范围

核心路径为不预约的 `capture_call`、原 Store 预约/唯一发送权/私有观察的
`observe_live_attempt`，以及 `accept_live_response` 的首次原结算、受信用量 hook
和原业务应用。旧 Fake 预约入口与旧完整 `execute_live_attempt` 保持兼容。
本次不增加 SQL 迁移，不改变已结算缺失反馈的拒绝重放规则。

工作流核对 hook 在进入原 `reconcile_usage` 之前，另要求证据的 attempt ID 和
settlement hash 精确匹配当前原调用；同请求另一 attempt 的有效证据也会被拒绝。
公开报告只投影标识、状态、计数和 hash，发送观察缺失保持显式未知。

## 2. 实际环境与执行

构建、测试身份为 UID/GID 65534（普通用户）。CTest 使用 `umask 0002`。
构建目录为独立临时副本，未修改生产目录、数据库、凭据或服务。

| 项目 | 实际值 |
| --- | --- |
| 编译器 | GCC 13.3.0，Ubuntu 13.3.0-6ubuntu2~24.04.1 |
| 构建 | CMake / Ninja，Release，仓库严格警告和 `-Werror` |
| Python | 3.12.14，已有兼容测试虚拟环境 |
| 本专项外部供应商调用 | 0 |
| 本专项真实付费调用 | 0 |
| 测试数据 | 自建临时 SQLite / 文件夹具 |

执行命令如下，调用前已把开发树复制到普通用户拥有的临时目录：

```sh
setpriv --reuid=65534 --regid=65534 --clear-groups \
  cmake -S /tmp/wls-live-core-validation/source \
  -B /tmp/wls-live-core-validation/build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/tmp/wls861-r33a-venv/bin/python

setpriv --reuid=65534 --regid=65534 --clear-groups \
  cmake --build /tmp/wls-live-core-validation/build-release --parallel 2 \
  --target event_runtime_tests workflow_runtime_tests acceptance_runtime_tests live_model_store_tests

setpriv --reuid=65534 --regid=65534 --clear-groups sh -c \
  'umask 0002; exec ctest --test-dir /tmp/wls-live-core-validation/build-release --output-on-failure -R "^(event_runtime|workflow_runtime|acceptance_runtime|live_model_store)$"'
```

最终一次专项运行包含当前 attempt/settlement hook 绑定修正，结果为：

| CTest 名称 | 结果 | 耗时 |
| --- | --- | ---: |
| `event_runtime` | 通过 | 4.09 秒 |
| `workflow_runtime` | 通过 | 3.71 秒 |
| `acceptance_runtime` | 通过 | 7.76 秒 |
| `live_model_store` | 通过 | 0.96 秒 |
| **合计** | **4 / 4，通过；0 失败** | **16.52 秒** |

此前同四项兼容回归也曾 4 / 4 通过（15.89 秒），但在补齐当前结算证据绑定后，
重新编译核心并重新链接四个目标，以上表的最终运行作为本报告依据；不重复计数为八项。
更早三份核心 `.cpp` 的严格语法编译曾退出 0；它不代替最终构建和运行结果。

### 未计入通过的执行尝试

- 一个较早后台构建会话在继续检查时已不可用，工具返回 `Unknown process id 94626`。
  没有最终退出状态和完整结果，因此未据此声称构建或测试成功。
- 普通受限 shell 的用户映射仅含当前 UID，直接降至 UID 65534 返回
  `setpriv: setresuid failed: Invalid argument`；直接覆写普通用户拥有的临时副本也曾
  返回权限错误。这两次未执行测试，不属于产品测试通过或失败结果。
- 随后使用已允许的本地普通用户执行方式完整刷新临时副本，配置、编译及上述 CTest
  均取得明确退出 0。没有放松运行时权限守卫，也没有把 root 运行代替普通用户回归。

## 3. 核心源码范围

最终兼容运行时，下列六个模块 A 源文件与共享开发树一致。它们用于明确本专项的源码范围；
其他模块、CLI 的后续变更和最终提交仍需要整合验证。

| 路径 | SHA-256 |
| --- | --- |
| `include/wls/offline_runtime.hpp` | `06a6bdff7d1ae0ecd24ecaaaf0aaf3392a5bb48e1a1196de4304394ef59e4ede` |
| `src/offline_runtime.cpp` | `c503a4648f4b2c6f874609da568cad8ec190e561df4e4f8b550ea8e4fedc52fe` |
| `include/wls/live_model.hpp` | `71e7959bdd0f66145632c579bbd1047185746268eaa1d92d5f0e7428a0fadb42` |
| `src/live_model.cpp` | `6eff38a9e070f76fe3305ed4a9b51931297a7d25f5c7bc92dc04354a1bbf318b` |
| `include/wls/event_runtime.hpp` | `3f7b0d98185b2aeba5a505c14895259f2be27228023b6f095f096d604dd35fb1` |
| `src/event_runtime.cpp` | `a3b95aa546af6eb593b101a9595b936d47737defaa8ba654b10a51e7791262c2` |

## 4. 宿主 CLI 只读审查

只读审查了整合代理新增的 `src/live_cli.inc` 及 `src/cli.cpp` 的 `live-status` /
`pump-live` 分支，没有修改这些文件。该检查属于代码审查，不作为 CLI 运行测试计数。

观察到的契约包括：完整授权字段及未知字段拒绝；请求、冻结配置、execution kind 和全部
实际选中模型 profile 的精确绑定；单独的 network/provider 授权；正数费用预留和有限批次；
授权预检不执行 getenv callback；选项、授权、原冻结配置和 sandbox policy 检查在写者接管前完成；
接管限定当前请求，之后再次核对；业务工作区中的授权文件被拒绝。

CLI 没有把 JSON 账目文件伪装成受信 provider 核验器。没有用量 verifier 时，原协议未知值
继续未知，并由核心返回 `needs_accounting`。读者阶段没有把全部待核对余额直接当成无效授权，
因此仍允许正确的 scoped recovery；真正预约之前，核心和 Store 继续检查原账目与预算。
公开结果只附授权资料的 hash，不附凭据值。审查中未发现阻断问题。

## 5. 结论的适用范围

本专项证明当前核心变更通过上述四组既有兼容回归，并取得严格 Release 构建结果。
它不单独证明新的完整 live 工作流、CLI、资源安装、强沙箱部署或最终提交 CI 已通过。
本模块合同见 [R3-live-workflow.md](../implementation/R3-live-workflow.md) 和
[ADR-085](../adr/085-controlled-live-workflow.md)。真实供应商质量、实际计费与同条件
单 Agent 对照仍需另行授权和验收。
