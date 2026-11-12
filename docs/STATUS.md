# 当前状态

当前实现 **0.11.1-r3.3c-audit**，目标蓝图 **v0.4.1**，Store **410**（无 013 迁移、异步协调器或持久事件扩展）。

更新：2026-11-12（UTC；CI快照23:11:47）。**G1-A–M已普通合并，产品合并基线 main@[`b09848e787e307a002ab97282c1b3d95901a397f`](https://github.com/wmqfl861/wls861/commit/b09848e787e307a002ab97282c1b3d95901a397f)**（[#66](https://github.com/wmqfl861/wls861/pull/66)，23:09:34 UTC）；本页属于随后文档提交。完整架构、模块/PR/merge SHA、构建步骤、证据及接手清单集中在[完整交接](reviews/g1-main-handoff-20261112.md)；[G1简报](reports/g1-exit.md)汇总结果与限制。

M head `c2aa6440dec010b9161527c1c80d6b75ccf2081a` 的 [core 43324561502](https://github.com/wmqfl861/wls861/actions/runs/43324561502) 三路各83/83 PASS、[source 43324561578](https://github.com/wmqfl861/wls861/actions/runs/43324561578) SUCCESS。本机新Release构建后，注册 `g1_exit_e2e` 单次1/1 PASS（19.80秒，原300秒期限）；83项注册、Make59+24及CMake ASan42+41的静态核对不等于本机全部执行。

产品合并基线 `b09848e7` 的 [full 43326432461](https://github.com/wmqfl861/wls861/actions/runs/43326432461) PENDING、[source 43326432580](https://github.com/wmqfl861/wls861/actions/runs/43326432580) SUCCESS；上一main `b267ae5b` 的 [full 43323428497](https://github.com/wmqfl861/wls861/actions/runs/43323428497) 仍IN_PROGRESS。先前 `fba19b5` 完整ASan两个单项超时、job中断及本机capture 90.17秒超时仍OPEN。**owner本次特定合并要求不代表全矩阵通过、正式PR评审或G1正式验收，也不扩大未来合并权限。** D-2/D-3未给出，真实供应商/付费调用为0。

## 尚缺的真实能力（当前边界）

- 真实 WLS 供应商模型调用与验收：0 次 / NOT_RUN；付费调用默认关闭，端点、能力与预算未配置。
- Windows 11 原生运行时：NOT_RUN（G2 前冻结）；私有 CI 原始证据托管：BLOCKED，完整原件先保留本机。此托管状态不表述产品私有部署能力。
- daemon/API、私有行清理 runner、完整记忆与独立审核闭环、获准真实小任务与同条件对照仍未实现。
- 0.11.1 全代码审核（ADR-090）已修复项、未采纳疑点与限制见[审核报告](reviews/code-audit-20261027.md)与[验证报告](reports/code-audit-20261027-validation.md)。

## 当前 G1 与历史阶段入口

- G1-A–M的产品与CI增量已合并，D/J在制、E待D的旧进度已被取代。M包含合成验收及H的两项实际产品修复，完整差异与当前精确结果见[交接 §3、§6](reviews/g1-main-handoff-20261112.md)。`structured-v3` 在 G1 总门禁正式通过前仍视为未发布。
- M 新 head `c2aa644` 的本机合成场景独立实测完成一条 `wls run`、一次确认、三个文件任务与 32 次合成调用；Worker 每任务第三次调用起最低模拟缓存读取 72.0093408%，估算 $0.234326，原 cache-write/provider-cost 保持 NULL。整体模拟缓存62.1915%与逐任务最低值分开；不作真实供应商费用或缓存证据。
- 阶段 0 已于 F-26 [#51](https://github.com/wmqfl861/wls861/pull/51) 合并为 `ff2c899` 时完成，原计数、取消与失败保留在[阶段 0 里程碑](reports/stage0-convergence.md)。旧 A/B/C/I 组合 `951c95d` 的六配置成功也仅绑定当时提交，不替代当前 main。

## 能力矩阵

| 能力 | 当前实现 | 已有证据 / 缺口 |
| --- | --- | --- |
| 四角色、版本、幂等、UNKNOWN | C++ Store + FakeModel及显式授权live有界流程 | 真实 SQLite/文件/跨进程和loopback原模型反馈回归；不证明实际供应商决策质量 |
| 文件/进程 | fs_list/read/search/mkdir/write/patch；完整强证明与前后清单可结算失败，Root专用核对；子进程固定0077 | 真实目录/文件和监督夹具；mkdir有投影配额、首次派发及UNKNOWN故障回归；强 bwrap/cgroup 部署正向和后台操作待完成 |
| 部署诊断 | doctor 的输入来源、实际路径、typed reasons、逐项文件/cgroup/proc 观察与预算/截断 | 只读，不开 Store；未来 operation 控制为 not_checked。只读前提、固定 cgroup 分支和拒绝用例不等于强隔离部署正向 |
| 问答/候选/验收 | Worker问题、原字节提交、TL/Dispatcher共享有限返工、验收及用户升级 | 固定脚本实现拒绝→新说明/确认→重提/验收及耗尽上报；回答不追加额度。通用任务重定义仍未实现 |
| AMEND | Dispatcher模型提交新计划及逐节点update/keep/cancel；遗漏节点按同计划续办 | 捕获请求/任务CAS和团队代次，遗漏保留待办、keep须新说明/ack；双格式/重开/预算/兄弟隔离回归。真实模型判断质量尚未验收 |
| 交付/清理 | 不可变包、精确回执、经验作业、固定清单受控 work 删除 | 实际发布/删除/恢复；私有行、owner收据保留；全局cleanup仍PENDING；PREPARED有原收据恢复/显式弃置；弃置残留保留、不可重用。部署 doctor 不扫描或采用分配残留 |
| 上下文/模型 | 冻结选择v1/v2/v3、Worker window-v1、固定角色工具/允许列表、lean-v1与追加折叠；原文回读与持久频率限制 | FakeModel大文件多轮/重开继续、模型回读原字节；旧请求不迁移。已接管理角色冻结窗口和原Store网络attempt；LLM语义压缩/真实模型质量待验收，完整当前契约超限在预约前拒绝 |
| Responses 离线协议 | 显式 provider profile、HTTP 文本/function_call 编解码、有界 SSE 回放；独立 encode-model/replay-model；#48 起 opt-in 扩展保留加密 reasoning、refusal 不派发、显式别名映射，原生历史按回合配对 call/result 身份并跨 Store 重开保持 | 两个CLI仍零网络、零凭据解析、零工具派发。Schema/Completed 不授予权限；原wire字段不改标；旧 profile 与 Store 410 兼容保持 |
| HTTPS / live工作流 | libcurl multi、显式CA/host验证、有界deadline/取消、原HTTP/SSE解码；原角色event pump与精确授权CLI | 自建loopback TLS；G1-I加入冻结DNS/显式代理策略，旧配置保留原解释；零付费自动重试。原 nullable 用量保留；显式冻结 pricing 可形成有效估算账目，未能有效结算仍停止新业务；签名 v2 为独立可选路径，选用后不得回退估算。实际供应商、外部DNS/代理验收未执行 |
| 估算与签名计费 / 迟到核账 | #49 增加冻结 token 单价估算、最低费用预留、原响应与结算用量投影及可信迟到实际费用差额调整；签名路径仍独立，原五维 nullable 账目不覆写 | 原31次合成计费验证保留；G1-J/L已接通有界续批与wls run，M的32次合成场景按各精确head单独登记；实际供应商/模型验收和付费调用仍未执行，自定义中转契约待实际服务接入。签名收据错绑、重放、缺值、超预留和错误响应依原账本限制 |
| 运行资源 / 迁移 | 编译认可清单、35项固定资源、版本注册表及CMake安装 | 从无关cwd与删除安装来源后运行；prefix/DESTDIR、资源篡改/缺失/特殊文件拒绝、旧API/迁移兼容；配置和凭据外置 |
| 单 Agent / 评测证据 | 显式冻结共享原生对话与原权限代理；v1结构验收、v2原模型来源、v3独立验证单逻辑Agent连续性 | 五任务双条件合成工程链、跨重启与错标/缺反馈反例；真实供应商质量、成本及长对话语义效果未验收 |
| 事件/并发 | 持久inbox、逐任务AMEND、串行pump；冻结准入窗口/独立计数/Root检查点 | 旧快照不改写；到限可核对后续进；不清审计/不重置预算。CLI 已按请求恢复；默认全库维护仍保留。并发租约、只在确有在途时提升选中请求代次仍未实现 |
| 配置执行状态 | check/explain按冻结配置披露runtime_support；G1-J的bounded-v1已接通请求墙钟期限、预算预警、输出上限/输入估算、同错停滞限制与有界续批 | 本地合成回归覆盖已接通路径；真实供应商未验收。未选bounded-v1时墙钟/预警/同错限制仍validated_only；全局并发/审核槽位、stall_minutes、snapshot_decision_log_entries及周期vacuum仍未执行；compaction_at_fraction仅append-fold-v1接通 |
| 记忆、MCP、跨功能审核 | 规格/参考契约 | 完整C++闭环及真实互通未实现 |
| 测试工具链 | 严格、UBSan、ASan、Clang、普通用户及umask矩阵 | PR按改动分层，核心含strict/umask 0002/Clang18；main push/nightly/手动完整矩阵为六配置、八个构建测试jobs（CMake与Make ASan各两片）；Make ASan默认-O0，原期限/flags保留；静态注册、定向子集与完整运行分别记录 |

## 阶段

S1 在实施中；近期优先级已调整为真实模型闭环的接口和运行时前置条件。
S2 独立审核/DAG，S3 记忆，S4 MCP/评测，S5 按证据扩展仍为后续阶段。
单 Agent 对照框架前移 S1，不再以整套 S2 完成为前提。
R2a.2已按[ADR-075](adr/075-owned-allocation-recovery.md)实现迁移、创建收据、精确恢复、弃置与清理联动。
R2a.3、R3.1、R3.2 的上述限定接口已实施；真实强沙箱部署正向验收保持独立。
R3.3a/b、两项长任务前置及受控event pump/CLI已按限定范围实施。迁移注册表与可安装资源
已补齐服务化之前置，但daemon/API继续后置。签名计费适配和单Agent逻辑身份执行器已并入。
下一步按 owner 2026-11-11 采纳的开发路线（见[ROADMAP](ROADMAP.md)「当前执行路线」：阶段 0 收敛 → G1 → G2 → G4 → G3 → G5 → G6 → G7 → G8 → G9 → G10）推进：阶段0完成、G1-A–M已合，当前main完整矩阵仍在途；新G1单次验证已完成，优先登记剩余CI结果并有界诊断已知超时，不为交接重复已完成测试。G2须等待D-2/D-3及明确真实执行授权，保留全部失败和未知样本。
离线codec profile继续固定real_calls_enabled=false；外层宿主live配置另有显式开关和独立执行授权，
默认关闭，max_paid_retries=0；CLI只有完整宿主授权后的受控入口，本次未使用真实供应商。
并发租约和选中请求按需提升代次继续开放，不因模型 AMEND 接入而视为完成。

## 历史

各批次实现、验收边界、失败原件与限定按版本见 [CHANGELOG](../CHANGELOG.md) 对应条目（0.11.1 全代码审核、0.11.0 签名计费/单 Agent、0.10.0 受控工作流/安装资源、0.9.0 五节点、0.8.x R2a/R3 各节点、0.7.x S1 各批次与 audit24 恢复）及 `docs/reports/` 逐批验证报告；audit24 逐项处置见[处置表](reviews/claude-2026-10-24-disposition.md)，2026-11 外审收敛见[处置表](reviews/claude-2026-11-06-disposition.md)与[收敛表](reviews/claude-2026-11-06-pr-convergence.md)。原 handoff 的 51 项产品验收记录保持原状。本批不新增生产部署、真实模型调用、付费测试或独立第二模型评审。
