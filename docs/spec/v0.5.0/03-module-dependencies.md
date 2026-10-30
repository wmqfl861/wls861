# v0.5.0 模块依赖与批准门禁

以下为 **PLANNED / 待审依赖图**，新增实现/验收为 **NOT_IMPLEMENTED / NOT_RUN**。实线表示必需技术依赖，虚线表示可先做的准备或条件性依赖；菱形表示需绑定具体方案的批准。图中的批准不是本稿已作出的决定。

## 1. 近期任务依赖

```mermaid
flowchart TD
    B[已验证 R2.4 分支 55dc989e] --> P0[P0 正式 PR 与独立审核]
    P0 --> A0{协调者采纳与获准合并}
    A0 --> M[R2.4 集成基线]
    B -.独立设计可先做.-> P1[P1 写者生命周期与并发风险设计]
    B -.离线准备可先做.-> P2[P2 固定真实小任务与对照材料]
    B -.构建调查可先做.-> P3[P3 Windows 原生构建与抽象]
    P1 --> A1{协调者批准具体并发方案}
    A1 --> CW[另立并发实现与故障测试卡]
    M --> CW
    P2 --> A2{端点 模型 计费来源 预算 调用授权}
    M --> LIVE[另行获准的真实小任务执行]
    A2 --> LIVE
    P3 --> A3{协调者确定 Windows 支持集合与设计}
    A3 --> P4[P4 原生文件安全与进程监督]
    P4 --> P5[P5 原生沙箱与网络安全]
    P4 --> P6[P6 双平台业务与证据一致性]
    P5 --> P6
    CW -.接入并发后扩展矩阵.-> P6
    P5 -.仅 Windows run 样本需要.-> LIVE
    LIVE --> S1[S1 产品验收结论]
    S1 --> P7[P7 按 S2 至 S5 拆分后继卡]
    CW --> S2[S2 公平并发与审核双容量]
    P7 --> S2
    P6 --> WP[Windows 完整支持结论]
```

P0 本轮只创建/完善正式 PR 和交接，不执行合并。P1 是风险设计卡，批准后仍需独立实现卡；其未完成不阻塞 P2 的离线材料或 P3 的构建调查。真实执行根据所用平台/工具补齐安全证据；Linux 文件任务不等待 Windows 沙箱，任何平台的 `run` 都不能借图中虚线省掉自身强隔离条件。

## 2. 运行模块的接入方向

```mermaid
flowchart LR
    CFG[外部配置与编译认可资源] --> FR[请求冻结快照]
    FR --> ST[Store 原事务与身份守卫]
    IN[CLI 或未来 Root 适配器] --> ST
    ST --> ER[有限事件运行器]
    ER --> CC[完整当前契约与历史窗口]
    CC --> MA[Fake 或获准模型适配器]
    MA --> OBS[原响应与传输观察]
    OBS --> LED[原结算与追加用量投影]
    LED --> ST
    ER --> WT[WorkerTools]
    WT --> ST
    ST --> CLAIM[唯一首次派发权]
    CLAIM --> OS[Linux 或待实现 Windows 执行边界]
    OS --> FACT[原结果 退出与工作区证据]
    FACT --> ST
    ST --> DEL[原字节交付与精确回执]
    DEL --> JOB[独立经验与清理待办]
    JOB --> OWN[原所有权 原库存 当前清理领取]
```

图示业务数据流，不表示所有调用是同步或一个跨存储事务。未来调度、MCP、A2A 和服务入口只能接入原权威边界；原副作用结算失败仍保留 UNKNOWN 和执行围栏。

## 3. 源码接入点与责任

下表是后续任务的影响范围，不是本轮修改清单；新增 API 名称和文件布局在实施卡中确定。

| 模块 | 当前源码/契约入口 | 新工作依赖与不得破坏的边界 |
| --- | --- | --- |
| 事务/恢复/租约 | `src/store.cpp`、`src/store_*.inc`、`include/wls/store.hpp` | P1 先审连接、锁、事务与 Actor 生命周期；保留原快照、CAS、scope、会话、epoch、SQL 与账目 |
| 事件/角色/公平 | `src/event_runtime.cpp`、`src/offline_runtime.cpp`、`src/scheduler.cpp` | 原 inbox/claim 与捕获目标；整合 S2 双容量和取消，不将 M1 回调配额直接称为 Agent 生命周期配额 |
| 配置/上下文/记忆 | `src/config_loader.cpp`、`src/context.cpp`、`src/structured_*`、`src/worker_history.cpp`、`src/management_context.cpp` | 请求冻结、原完整历史、版本化回读；S3 新 MemoryEngine 不由现有 Schema/经验表自动得到 |
| 模型/费用/对照 | `src/provider_profile.cpp`、`src/protocol_codec.cpp`、`src/sse_replay.cpp`、`src/https_transport.cpp`、`src/live_model.cpp`、`src/billing.cpp`、`src/single_agent.cpp` | P2 复用原链；S4 增第二适配器需冻结能力与计费语义；不改原 unknown/null 或历史 v1/v2/v3 身份 |
| 文件/进程/清理 | `src/workspace_posix.cpp`、`src/sandbox_posix.cpp`、`src/process_evidence.cpp`、`src/cleanup_posix.cpp`、`src/cleanup_runtime.cpp`、`src/worker_tools.cpp` | P3–P6 设计真正平台边界；对象身份、授权、退出和清理权分别验证 |
| 交付/持久/资源 | `src/delivery.cpp`、`src/journal_posix.cpp`、`src/runtime_resources.cpp`、`src/migrations.cpp`、`cmake/runtime_resources.cmake` | Windows 原生锁/同步/安装/发现；保持 Journal 与 SQL 原格式，平台证据另行版本化 |
| 构建/验收 | `CMakeLists.txt`、`Makefile`、`.github/workflows/ci.yml`、`tests/`、`scripts/evaluate_r3.py` | 新增 Windows 原生检查；Linux 六路与源码快照继续；固定期限和安全断言不降低 |

现有 `Workspace::dup_root_fd()` 和 `ProcessRunner(int, ...)` 直接体现 Linux 描述符边界，CMake 当前明确拒绝非 Linux。P3 需要先设计可承载平台资源身份的受信接口；将 HANDLE 强转为 int 或删除 CMake 拒绝条件不能完成适配。Linux `device/inode`、Windows 卷/文件 ID、进程退出信号的原含义不能互相伪造。

## 4. S2–S5 的设计依赖

```mermaid
flowchart TD
    S1[已验收 S1 所用闭环] --> S2[S2 DAG 独立审核 公平与双容量]
    EXP[已有原版本经验作业与证据] --> MD[S3 记忆设计与离线夹具准备]
    S2 --> MR[S3 记忆运行时与效果验收]
    MD --> MR
    ROOT[原 Root 身份与回执边界] --> IOD[S4 MCP 双向与第二适配器设计]
    MR --> IO[S4 互通产品验收]
    IOD --> IO
    IO --> S5[S5 A2A 服务化 语义检索 高级治理]
    CON[批准的并发与外部身份设计] --> S5
    WIN[P3 至 P6 原生平台证据] --> S5
```

阶段完成顺序不禁止提前设计、协议调查和离线夹具。服务化需要身份发行、跨进程控制、单写者协调和迁移维护等专项；不能仅把 CLI 放到 HTTP handler 中就开放给远程调用方。记忆与互通的上线政策、真实模型支出和最终发布都按 [09](09-release-governance.md)留批准与证据记录。
