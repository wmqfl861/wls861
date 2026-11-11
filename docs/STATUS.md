# 当前状态

当前实现 **0.11.1-r3.3c-audit**，目标蓝图 **v0.4.1**，Store **410**（无 013 迁移、异步协调器或持久事件扩展）。

main 基线 [`f29d5bd`](https://github.com/wmqfl861/wls861/commit/f29d5bd658bd5b050a889d6033b32520212504da)（2026-11-11）。已合并：G0 工程 [#43](https://github.com/wmqfl861/wls861/pull/43)/[#44](https://github.com/wmqfl861/wls861/pull/44)/[#45](https://github.com/wmqfl861/wls861/pull/45)（2026-11-07）、[#47](https://github.com/wmqfl861/wls861/pull/47) 文档收口与完整契约迁移（`f7760e5`，2026-11-08）、[#46](https://github.com/wmqfl861/wls861/pull/46) disposition 校验共用（`4987f2c`，2026-11-08）、[#48](https://github.com/wmqfl861/wls861/pull/48) G1-2 Responses 协议兼容（`f29d5bd`，2026-11-11）：opt-in profile 扩展保留加密 reasoning、refusal 不派发工具、显式别名映射与纯文本指令，原生历史按回合配对工具 call/result 身份且两轮原生历史跨 Store 重开保持；产品增删 795 行，Store 410 与旧 profile 兼容，证据见[验证报告](reports/g1-2-responses-compat-validation.md)。#48 精确 head `01c330d`：PR core run [42488178636](https://github.com/wmqfl861/wls861/actions/runs/42488178636)、显式全量六配置 run [42488303902](https://github.com/wmqfl861/wls861/actions/runs/42488303902)（各 CTest 配置 78/78，含原 Make-ASan 路径）与 source run [42488306828](https://github.com/wmqfl861/wls861/actions/runs/42488306828) 通过。合并后 main 推送：source run [42996980508](https://github.com/wmqfl861/wls861/actions/runs/42996980508) 成功；完整六配置 run [42996980600](https://github.com/wmqfl861/wls861/actions/runs/42996980600) 截至本次更新仍在运行，不计为通过。[#50](https://github.com/wmqfl861/wls861/pull/50) 已按 owner 决定关闭（由采纳路线的最小联网切片替代，分支保留）。2026-11-07 G0 合并的组合验证、最终 main 门禁与中间取消 run 的历史证据见 [CHANGELOG](../CHANGELOG.md) 对应条目与原报告。

## 尚缺的真实能力（当前边界）

- 真实 WLS 供应商模型调用与验收：0 次 / NOT_RUN；付费调用默认关闭，端点、能力与预算未配置。
- Windows 11 原生运行时：NOT_RUN（G2 前冻结）；私有 CI 原始证据托管：BLOCKED，完整原件先保留本机。此托管状态不表述产品私有部署能力。
- daemon/API、私有行清理 runner、完整记忆与独立审核闭环、获准真实小任务与同条件对照仍未实现。
- 0.11.1 全代码审核（ADR-090）已修复项、未采纳疑点与限制见[审核报告](reviews/code-audit-20261027.md)与[验证报告](reports/code-audit-20261027-validation.md)。

## 进行中（未合并，不计为已完成）

- PR [#49](https://github.com/wmqfl861/wls861/pull/49)（G1-1 按冻结单价估算费用并保留原用量，OPEN，head `834efcd`）：分支 core 三配置检查当前均为 SUCCESS；与 #48 同改 `provider_profile`，按 owner 待决方案在原分支普通 merge 新 main 解决整合冲突（不强推），CI 全绿后转 Ready 合并；未合并，不计入能力，其既有 `billing_baseline_e2e` 31 次调用验证保留。
- F-26 阶段 0 文档收敛（Draft PR，分支 `docs/f26-handoff-20261111-h6ose7wz`，直接基于 main）：对齐 STATUS/CHANGELOG/ROADMAP/需求追踪至 2026-11-11 owner 采纳路线，7 份审核/计划文件自冻结提交 `7d1dadb` 原字节入库存档（仅 05 追加执行附录），新增一页[阶段 0 里程碑](reports/stage0-convergence.md)（进行中，未达出口）。纯文档变更，未合并，不计为能力；#49 整合后更新事实再按 D-1 合并。

## 能力矩阵

| 能力 | 当前实现 | 已有证据 / 缺口 |
| --- | --- | --- |
| 四角色、版本、幂等、UNKNOWN | C++ Store + FakeModel及显式授权live有界流程 | 真实 SQLite/文件/跨进程和loopback原模型反馈回归；不证明实际供应商决策质量 |
| 文件/进程 | fs_list/read/search/mkdir/write/patch；完整强证明与前后清单可结算失败，Root专用核对；子进程固定0077 | 真实目录/文件和监督夹具；mkdir有投影配额、首次派发及UNKNOWN故障回归；强 bwrap/cgroup 部署正向和后台操作待完成 |
| 部署诊断 | doctor 的输入来源、实际路径、typed reasons、逐项文件/cgroup/proc 观察与预算/截断 | 只读，不开 Store；未来 operation 控制为 not_checked。只读前提、固定 cgroup 分支和拒绝用例不等于强隔离部署正向 |
| 问答/候选/验收 | Worker问题、原字节提交、TL/Dispatcher共享有限返工、验收及用户升级 | 固定脚本实现拒绝→新说明/确认→重提/验收及耗尽上报；回答不追加额度。通用任务重定义仍未实现 |
| AMEND | Dispatcher模型提交新计划及逐节点update/keep/cancel；遗漏节点按同计划续办 | 捕获请求/任务CAS和团队代次，遗漏保留待办、keep须新说明/ack；双格式/重开/预算/兄弟隔离回归。真实模型判断质量尚未验收 |
| 交付/清理 | 不可变包、精确回执、经验作业、固定清单受控 work 删除 | 实际发布/删除/恢复；私有行、owner收据保留；全局cleanup仍PENDING；PREPARED有原收据恢复/显式弃置；弃置残留保留、不可重用。部署 doctor 不扫描或采用分配残留 |
| 上下文/模型 | 冻结选择v1/v2和Worker window-v1；近期完整轮次、旧/大结果原证据引用；模型回读与持久频率限制 | FakeModel大文件多轮/重开继续、模型回读原字节；旧请求不迁移。已接管理角色冻结窗口和原Store网络attempt；LLM语义压缩/真实模型质量待验收，完整当前契约超限在预约前拒绝 |
| Responses 离线协议 | 显式 provider profile、HTTP 文本/function_call 编解码、有界 SSE 回放；独立 encode-model/replay-model；#48 起 opt-in 扩展保留加密 reasoning、refusal 不派发、显式别名映射，原生历史按回合配对 call/result 身份并跨 Store 重开保持 | 两个CLI仍零网络、零凭据解析、零工具派发。Schema/Completed 不授予权限；原wire字段不改标；旧 profile 与 Store 410 兼容保持 |
| HTTPS / live工作流 | libcurl multi、显式CA/host验证、有界deadline/取消、原HTTP/SSE解码；原角色event pump与精确授权CLI | 自建loopback TLS，严格IPv4/显式路由，DNS名另需c-ares；零自动重试。费用未知停止，v2授权绑定独立签名计费；实际供应商与c-ares外部DNS验收未执行 |
| 签名计费 / 迟到核账 | 独立Ed25519收据、原challenge/字节身份、五维nullable账目、跨调用唯一归属、追加投影与只读重放 | 错签、错绑、重放、缺值、超预留和错误响应各按原账本限制；自定义中转契约需实际服务另行接入 |
| 运行资源 / 迁移 | 编译认可清单、35项固定资源、版本注册表及CMake安装 | 从无关cwd与删除安装来源后运行；prefix/DESTDIR、资源篡改/缺失/特殊文件拒绝、旧API/迁移兼容；配置和凭据外置 |
| 单 Agent / 评测证据 | 显式冻结共享原生对话与原权限代理；v1结构验收、v2原模型来源、v3独立验证单逻辑Agent连续性 | 五任务双条件合成工程链、跨重启与错标/缺反馈反例；真实供应商质量、成本及长对话语义效果未验收 |
| 事件/并发 | 持久inbox、逐任务AMEND、串行pump；冻结准入窗口/独立计数/Root检查点 | 旧快照不改写；到限可核对后续进；不清审计/不重置预算。CLI 已按请求恢复；默认全库维护仍保留。并发租约、只在确有在途时提升选中请求代次仍未实现 |
| 配置执行状态 | check/explain显示 runtime_support 和 validated_only 项 | 形状有效不代表墙钟/告警/压缩/并发/停滞检测已执行 |
| 记忆、MCP、跨功能审核 | 规格/参考契约 | 完整C++闭环及真实互通未实现 |
| 测试工具链 | 严格、UBSan、ASan、Clang、普通用户及umask矩阵 | #44 后 PR 走一套 Linux 核心门禁（strict/umask 0002/Clang18），ASan/UBSan/Make-ASan 在 main push、nightly 与手动全量（Release/两种umask、UBSan、CMake ASan、Make ASan、Clang18；Make ASan默认-O0，其他优化构建保留）；结果须按报告和具体 CI 提交核对 |

## 阶段

S1 在实施中；近期优先级已调整为真实模型闭环的接口和运行时前置条件。
S2 独立审核/DAG，S3 记忆，S4 MCP/评测，S5 按证据扩展仍为后续阶段。
单 Agent 对照框架前移 S1，不再以整套 S2 完成为前提。
R2a.2已按[ADR-075](adr/075-owned-allocation-recovery.md)实现迁移、创建收据、精确恢复、弃置与清理联动。
R2a.3、R3.1、R3.2 的上述限定接口已实施；真实强沙箱部署正向验收保持独立。
R3.3a/b、两项长任务前置及受控event pump/CLI已按限定范围实施。迁移注册表与可安装资源
已补齐服务化之前置，但daemon/API继续后置。签名计费适配和单Agent逻辑身份执行器已并入。
下一步按 owner 2026-11-11 采纳的开发路线（见[ROADMAP](ROADMAP.md)「当前执行路线」：阶段 0 收敛 → G1 → G2 → G4 → G3 → G5 → G6 → G7 → G8 → G9 → G10）推进：阶段 0 完成 #49 整合与文档对齐后进入 G1；核验实际中转签发与所选端点能力，在凭据、预算和明确授权具备后进行真实小任务与同条件对照，保留全部失败和未知样本。
离线codec profile继续固定real_calls_enabled=false；外层宿主live配置另有显式开关和独立执行授权，
默认关闭，max_paid_retries=0；CLI只有完整宿主授权后的受控入口，本次未使用真实供应商。
并发租约和选中请求按需提升代次继续开放，不因模型 AMEND 接入而视为完成。

## 历史

各批次实现、验收边界、失败原件与限定按版本见 [CHANGELOG](../CHANGELOG.md) 对应条目（0.11.1 全代码审核、0.11.0 签名计费/单 Agent、0.10.0 受控工作流/安装资源、0.9.0 五节点、0.8.x R2a/R3 各节点、0.7.x S1 各批次与 audit24 恢复）及 `docs/reports/` 逐批验证报告；audit24 逐项处置见[处置表](reviews/claude-2026-10-24-disposition.md)，2026-11 外审收敛见[处置表](reviews/claude-2026-11-06-disposition.md)与[收敛表](reviews/claude-2026-11-06-pr-convergence.md)。原 handoff 的 51 项产品验收记录保持原状。本批不新增生产部署、真实模型调用、付费测试或独立第二模型评审。
