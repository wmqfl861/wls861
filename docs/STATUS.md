现在能做什么：0.12.0 已通过 G2-P；Responses 与 Chat Completions 非流式工作流的合成预演均完成交付。
还不能做什么：真实 WLS 调用仍为 0；强沙箱部署与 `/input` 尚未完成，环境卡片的实际执行检查未接通，Windows 11 为 NOT_RUN。
需要 owner 做什么：在运行 agent 的 shell 中导出 `WLS_G2_API_KEY`，然后明确告知“密钥已就绪”；两项满足前不发任何真实请求。

# 当前状态

当前实现 **0.12.0**，当前蓝图 **v0.4.1**、已采纳目标蓝图 **v0.5**，Store **410**（无 013 迁移、异步协调器或持久事件扩展）。

更新：2026-11-13。G1 已按 owner 的 D-10 验收通过；G2-P 的完整实现与八场景结果见[预演报告](reports/g2p-rehearsal.md)，执行顺序见[当前计划](reviews/claude-2026-11-13/04-plan.md)。版本 0.12.0 不改变 Store 410，六个新格式在 G2 出口之前允许原地调整，旧 framed-v1 / structured-v2 回归保持。

G2-P：最小自包含工具参数与宿主补全、具体字段错误、受限纠错、失败调用记账、按次预留和至多两次合格重试、files 默认模板、可读摘要和原名导出、已知输出项未知元数据兼容均已合入。原模型响应、未知值、精确授权、预算预约、业务版本及事务守卫保持。独立 `g2_rehearsal_e2e` 与原 G1 本地首跑通过（22.32 秒、22.88 秒）；新注册项加入后总计 84 项。[P7 #81](https://github.com/wmqfl861/wls861/pull/81) 的三核心各 84/84 项通过，整体门禁 12:12，已合并。

近期增量：[配置复用 #85](https://github.com/wmqfl861/wls861/pull/85) 已复用相邻调用的规范配置字节，本地 acceptance 仍 TIMEOUT，超时根因尚未修复；[输出切片 #86](https://github.com/wmqfl861/wls861/pull/86) 已支持新 `structured-v3` 的 UTF-8 `op_output` 与 `run` 前 8 KiB 预览，旧格式与旧记录不回填。

[纯 Chat 编解码 #87](https://github.com/wmqfl861/wls861/pull/87) 与 [Chat 运行时 #88](https://github.com/wmqfl861/wls861/pull/88) 已合入。非流式 Chat 接入原预留、唯一发送、观察与结算流程；本地 Release 12/12 个 CTest 首轮通过，同一预演的 Responses 和 Chat 各完成 35 次合成调用、8 个场景，均在断开后以同一命令恢复到 DELIVERED。R3 评估尚未接入 Chat。

当前 CI：main `170619b`（#88 合并）的新完整矩阵待完成；上一 main `5599d44` 的[矩阵 43434154232](https://github.com/wmqfl861/wls861/actions/runs/43434154232) 已有 7 个构建测试作业成功，ASan 1/2 与 Make-ASan 2/2 仍待完成。此前 `82ae41e` 的[完整矩阵 43428028557](https://github.com/wmqfl861/wls861/actions/runs/43428028557) 九个构建测试作业为 8 PASS、ASan 1/2 FAIL，`acceptance_runtime` 在前 14 个内部用例 PASS 后于 180.18 秒 TIMEOUT。`23965af` 的九作业全 PASS 绑定旧输入。原失败记录保留；D-11 的 sanitizer CTest 时限倍数仍为 2，Release 和业务期限不变。

## 尚缺的真实能力（当前边界）

- 真实 WLS 供应商模型调用与验收：0 次 / NOT_RUN。D-2/D-3 已授权 G2 仅用 OpenAI Responses / `gpt-5-mini`，每请求上限 1 美元、G2 合计上限 10 美元，符合决定 §3.3 的重试最多 2 次；当前仍等待密钥和 owner 明确“密钥已就绪”。配置外置，密钥不写入文件、日志、PR 或文档。合成结果不代表真实模型验收。
- G4-A1 [#84](https://github.com/wmqfl861/wls861/pull/84) 的 native 验证仍 BLOCKED，G4-B 候选尚未纳入 main；`/input` 尚未实现，环境卡片的实际执行检查未接通。
- Windows 11 原生运行时：NOT_RUN。D-12 已取消私有 CI 原始证据托管要求，公开 CI 日志和仓库报告足够；本机原始证据保留 30 天后才可清理。
- daemon/API、私有行清理 runner、完整记忆与独立审核闭环、获准真实小任务与同条件对照仍未实现。
- 0.11.1 全代码审核（ADR-090）已修复项、未采纳疑点与限制见[审核报告](reviews/code-audit-20261027.md)与[验证报告](reports/code-audit-20261027-validation.md)。

## 当前 G1 与历史阶段入口

- G1-A–M 的产品与 CI 增量已合并并获 owner 验收。M 包含合成验收及 H 的两项实际产品修复，原始差异与结果见[交接 §3、§6](reviews/g1-main-handoff-20261112.md)。D-10 将 `structured-v3`、`lean-v1`、`fixed-role-v1`、`append-fold-v1`、`guided-v1`、`bounded-v1` 的冻结推迟到 G2 出口；此前可原地调整，`framed-v1`、`structured-v2` 的旧请求回归保留。
- M 新 head `c2aa644` 的本机合成场景独立实测完成一条 `wls run`、一次确认、三个文件任务与 32 次合成调用；Worker 每任务第三次调用起最低模拟缓存读取 72.0093408%，估算 $0.234326，原 cache-write/provider-cost 保持 NULL。整体模拟缓存62.1915%与逐任务最低值分开；不作真实供应商费用或缓存证据。
- 阶段 0 已于 F-26 [#51](https://github.com/wmqfl861/wls861/pull/51) 合并为 `ff2c899` 时完成，原计数、取消与失败保留在[阶段 0 里程碑](reports/stage0-convergence.md)。旧 A/B/C/I 组合 `951c95d` 的六配置成功也仅绑定当时提交，不替代当前 main。

## 能力矩阵

| 能力 | 当前实现 | 已有证据 / 缺口 |
| --- | --- | --- |
| 四角色、版本、幂等、UNKNOWN | C++ Store + FakeModel及显式授权live有界流程 | 真实 SQLite/文件/跨进程和loopback原模型反馈回归；不证明实际供应商决策质量 |
| 文件/进程 | fs_list/read/search/mkdir/write/patch；新 structured-v3 的 UTF-8 op_output 与 run 前 8 KiB 预览，旧格式与旧记录不回填；完整强证明与前后清单可结算失败，Root专用核对；子进程固定0077 | 真实目录/文件和监督夹具；mkdir有投影配额、首次派发及UNKNOWN故障回归；强 bwrap/cgroup 部署正向和后台操作待完成 |
| 部署诊断 | doctor 的输入来源、实际路径、typed reasons、逐项文件/cgroup/proc 观察与预算/截断 | 只读，不开 Store；未来 operation 控制为 not_checked。只读前提、固定 cgroup 分支和拒绝用例不等于强隔离部署正向 |
| 问答/候选/验收 | Worker问题、原字节提交、TL/Dispatcher共享有限返工、验收及用户升级 | 固定脚本实现拒绝→新说明/确认→重提/验收及耗尽上报；回答不追加额度。通用任务重定义仍未实现 |
| AMEND | Dispatcher模型提交新计划及逐节点update/keep/cancel；遗漏节点按同计划续办 | 捕获请求/任务CAS和团队代次，遗漏保留待办、keep须新说明/ack；双格式/重开/预算/兄弟隔离回归。真实模型判断质量尚未验收 |
| 交付/清理 | 不可变包、精确回执、经验作业、固定清单受控 work 删除 | 实际发布/删除/恢复；私有行、owner收据保留；全局cleanup仍PENDING；PREPARED有原收据恢复/显式弃置；弃置残留保留、不可重用。部署 doctor 不扫描或采用分配残留 |
| 上下文/模型 | 冻结选择v1/v2/v3、Worker window-v1、固定角色工具/允许列表、lean-v1与追加折叠；原文回读与持久频率限制 | FakeModel大文件多轮/重开继续、模型回读原字节；旧请求不迁移。已接管理角色冻结窗口和原Store网络attempt；LLM语义压缩/真实模型质量待验收，完整当前契约超限在预约前拒绝 |
| Responses 离线协议 | 显式 provider profile、HTTP 文本/function_call 编解码、有界 SSE 回放；独立 encode-model/replay-model；#48 起 opt-in 扩展保留加密 reasoning、refusal 不派发、显式别名映射，原生历史按回合配对 call/result 身份并跨 Store 重开保持 | 两个CLI仍零网络、零凭据解析、零工具派发。Schema/Completed 不授予权限；原wire字段不改标；旧 profile 与 Store 410 兼容保持 |
| Chat 非流式协议 | 纯编解码、独立外部 profile、原 live 预留与结算流程 | 双协议各 35 次合成调用、8 个场景通过；Chat 流式、Responses 兼容扩展与签名挑战保持拒绝；真实供应商未验收 |
| HTTPS / live工作流 | libcurl multi、显式CA/host验证、有界deadline/取消、原HTTP/SSE解码；原角色event pump与精确授权CLI | 自建loopback TLS；G1-I加入冻结DNS/显式代理策略，旧配置保留原解释；冻结 bounded-v1 可对明确未发送连接失败或完整的指定 HTTP 状态最多重试两次，每次单独预留/结算。原 nullable 用量保留；冻结 pricing 下完整用量估算，缺值将原预留转为已花费；签名 v2 为独立可选路径，选用后不得回退估算。实际供应商、外部DNS/代理验收未执行 |
| 估算与签名计费 / 迟到核账 | #49 增加冻结 token 单价估算、最低费用预留、原响应与结算用量投影及可信迟到实际费用差额调整；签名路径仍独立，原五维 nullable 账目不覆写 | 原31次合成计费验证保留；G1-J/L已接通有界续批与wls run，M的32次合成场景按各精确head单独登记；实际供应商/模型验收和付费调用仍未执行，自定义中转契约待实际服务接入。签名收据错绑、重放、缺值、超预留和错误响应依原账本限制 |
| 运行资源 / 迁移 | 编译认可清单、35项固定资源、版本注册表及CMake安装 | 从无关cwd与删除安装来源后运行；prefix/DESTDIR、资源篡改/缺失/特殊文件拒绝、旧API/迁移兼容；配置和凭据外置 |
| 单 Agent / 评测证据 | 显式冻结共享原生对话与原权限代理；v1结构验收、v2原模型来源、v3独立验证单逻辑Agent连续性 | 五任务双条件合成工程链、跨重启与错标/缺反馈反例；R3 评估尚未接入 Chat；真实供应商质量、成本及长对话语义效果未验收 |
| 事件/并发 | 持久inbox、逐任务AMEND、串行pump；冻结准入窗口/独立计数/Root检查点 | 旧快照不改写；到限可核对后续进；不清审计/不重置预算。CLI 已按请求恢复；默认全库维护仍保留。并发租约、只在确有在途时提升选中请求代次仍未实现 |
| 配置执行状态 | check/explain按冻结配置披露runtime_support；G1-J的bounded-v1已接通请求墙钟期限、预算预警、输出上限/输入估算、同错停滞限制与有界续批 | 本地合成回归覆盖已接通路径；真实供应商未验收。未选bounded-v1时墙钟/预警/同错限制仍validated_only；全局并发/审核槽位、stall_minutes、snapshot_decision_log_entries及周期vacuum仍未执行；compaction_at_fraction仅append-fold-v1接通 |
| 记忆、MCP、跨功能审核 | 规格/参考契约 | 完整C++闭环及真实互通未实现 |
| 测试工具链 | 严格、UBSan、ASan、Clang、普通用户及umask矩阵 | PR按改动分层，核心含strict/umask 0002/Clang18；main push/nightly/手动完整矩阵为六配置、九个构建测试作业（UBSan、CMake ASan与Make ASan各两片）；Make ASan默认-O0；Release原期限不变，ASan/UBSan CTest期限按D-11乘2；静态注册、定向子集与完整运行分别记录 |

## 阶段

S1 在实施中；近期优先级已调整为真实模型闭环的接口和运行时前置条件。
S2 独立审核/DAG，S3 记忆，S4 MCP/评测，S5 按证据扩展仍为后续阶段。
单 Agent 对照框架前移 S1，不再以整套 S2 完成为前提。
R2a.2已按[ADR-075](adr/075-owned-allocation-recovery.md)实现迁移、创建收据、精确恢复、弃置与清理联动。
R2a.3、R3.1、R3.2 的上述限定接口已实施；真实强沙箱部署正向验收保持独立。
R3.3a/b、两项长任务前置及受控event pump/CLI已按限定范围实施。迁移注册表与可安装资源
已补齐服务化之前置，但daemon/API继续后置。签名计费适配和单Agent逻辑身份执行器已并入。
下一步按 owner 2026-11-13 采纳的[当前计划](reviews/claude-2026-11-13/04-plan.md)推进：阶段 0 收敛 → G1 → G2-P → G2 → G4 → G3 → G5 → G6 → G7 → G8 → G9 → G10。阶段 0、G1 和 G2-P 已完成；等待密钥期间已合入 G4-C 输出切片与 G5-A/B，G4-A1 native 仍 BLOCKED，G4-B 候选尚未纳入、`/input` 未做、环境卡片的实际执行检查未接通。standard 的部署和无 cgroup 的启动与全树回收保持独立验收。密钥与明确就绪通知均到位后优先转回 G2；真实运行保留全部失败和未知样本。
离线codec profile继续固定real_calls_enabled=false；外层宿主live配置另有显式开关和独立执行授权，
默认关闭，max_paid_retries=0；CLI只有完整宿主授权后的受控入口，本次未使用真实供应商。
并发租约和选中请求按需提升代次继续开放，不因模型 AMEND 接入而视为完成。

## 历史

各批次实现、验收边界、失败原件与限定按版本见 [CHANGELOG](../CHANGELOG.md) 对应条目（0.11.1 全代码审核、0.11.0 签名计费/单 Agent、0.10.0 受控工作流/安装资源、0.9.0 五节点、0.8.x R2a/R3 各节点、0.7.x S1 各批次与 audit24 恢复）及 `docs/reports/` 逐批验证报告；audit24 逐项处置见[处置表](reviews/claude-2026-10-24-disposition.md)，2026-11 外审收敛见[处置表](reviews/claude-2026-11-06-disposition.md)与[收敛表](reviews/claude-2026-11-06-pr-convergence.md)。原 handoff 的 51 项产品验收记录保持原状。本批不新增生产部署、真实模型调用、付费测试或独立第二模型评审。
