# 当前实现 **0.11.1-r3.3c-audit**，目标蓝图 **v0.4.1**，Store **410**。

本轮对 `d3da700c9c47db86abe941374bcbdac5e31462d4` 进行全代码工程审核，
覆盖内核/Store、文件与进程、模型协议与计费、上下文/工作流、CLI/配置、评测及构建检查。
已复现并修复写锁继承、并发账本误报、已核账容量不释放、文件类型/完整读取、取消清理证据、
路由过晚拒绝、评测调用来源/原输入绑定、小数历史指纹误拒及旧 Schema 目录读取边界问题。
ASan复验另定位完整上下文和历史前缀的重复处理，增加保持原字节与校验语义的函数内复用；
原60秒批次、150秒CLI子进程及900秒联合E2E期限保持，过程失败与最终结果分别登记。
原 SQL、原结算及角色/版本/UNKNOWN/一次发送权限保持；实际供应商与付费调用为0。
完整范围、未采纳的疑点及限制见[审核报告](reviews/code-audit-20261027.md)和
[ADR-090](adr/090-comprehensive-code-audit.md)，实际门禁见[本轮验证](reports/code-audit-20261027-validation.md)。

## 上一批 0.11.0 的实现与证据范围

上一批按[两个后继模块](implementation/two-module-billing-baseline.md)接续已推送的 `1b7020e`：

| 模块 | 当前新增实现 | 验收边界 |
| --- | --- | --- |
| 签名计费与连续 CLI | 冻结 opt-in challenge、Ed25519 独立收据、请求/响应原字节绑定、事务内账单唯一归属、v2 授权精确绑定政策原字节、同步与迟到核账、无写者接管的精确重放 | 项目自定义受信中转协议；临时密钥和 loopback TLS 只验工程契约，不证明实际供应商支持或真实费用 |
| 单 Agent 与 v3 基线 | 一个逻辑执行者的全部跨角色原响应/反馈、预约事务内连续性核验、显式 live/fake CLI、跨重启继续；v3 重建实际原生输入及五任务双条件证据 | 原角色仍分别限定权限；历史有界且完整，不压缩丢弃失败；真实同模型品质和成本对照仍待现场执行 |

Store 仍为410，无新增或改写已发布 SQL。原结算保持不可变，未知值、业务 UNKNOWN、
预算和一次发送规则继续生效；旧 v1/v2 评测、原角色执行及资源安装保持兼容。
全库工作区路径检查在授权、信任政策、收据内容读取之前完成；运行只使用一次捕获的字节。
实际专项、完整回归、输入身份和最终提交门禁见[本批验证](reports/billing-baseline-validation.md)。
首次发布后的ASan总任务容量与原验收运行时超时另见[CI复核](reports/billing-baseline-ci-followup.md)；
预约事务、工作区策略和管理查询只复用当前事务或调用栈中已完整验证的冻结配置；每次仍匹配
实际读取的完整配置字节/hash，保留各工具、工作区和管理视图的当前动态权限检查。
接口与限制见[签名计费](implementation/R3-signed-billing.md)、
[单 Agent 基线](implementation/R3-single-agent-baseline.md)及[评测契约](implementation/R3-evaluation-framework.md)。
本批真实外部供应商调用、付费调用和生产数据操作均为0。

## 上一批 0.10.0 的实现与证据范围

上一批按[两个大模块](implementation/two-module-continuation.md)接续已推送版本：

| 模块 | 当前新增实现 | 验收边界 |
| --- | --- | --- |
| 受控在线工作流 | 原角色event pump连接原预约/唯一发送/结算/反馈；计划、派工、工具、提交、检查、验收、经验、模型AMEND和有限返工；独立授权CLI、批次期限及五维账目停批 | 本地TLS合成模型与原SQLite/文件。实际供应商、付费调用及模型质量验收仍未执行；CLI遇未知费用停止，完整连续流程需受信独立计费核验hook |
| 迁移与安装资源 | 版本化迁移注册表、固定35项SQL/Schema/提示词清单、完整字节/hash验证、Schema一次捕获、可执行文件相对发现与prefix/DESTDIR安装 | Store保持410、旧构造器与已发布SQL兼容；安装后配置显式外置；不能据此宣称daemon/API或部署强隔离已完成 |

评测随工作流新增v2 `model-origin-v1`，把原模型响应、捕获目标、不可变反馈与业务事实关联；
v1继续只证明结构验收，不改写历史成绩。该版本的单Agent基线执行器仍待版本化身份契约，
不能把多个Store Worker重命名为同一Agent绕过现有检查。
该版本实际命令、失败修正、输入身份和最终门禁见[两模块验证](reports/two-module-validation.md)。
新接口及宿主边界见[在线工作流](implementation/R3-live-workflow.md)、
[安装资源](implementation/runtime-resources-and-migrations.md)与[评测契约](implementation/R3-evaluation-framework.md)。

此前0.9.0-r3.3b从0.8.7-r3.3a接续[五个实现节点](implementation/five-node-continuation-20261027.md)：

| 节点 | 当前新增实现 | 验收边界 |
| --- | --- | --- |
| R3.3b 预约/发送 | 原冻结profile、原attempt/预算、唯一发送权、原传输私有观察 | 临时SQLite和loopback TLS；独立宿主执行授权，默认关闭 |
| R3.3b 结算/对账 | 原结算不可变、恢复后无响应宿主观察、逐维度追加核对、预算守恒、终态与请求隔离 | 无响应不补造供应商返回，不重发、不恢复旧业务资格 |
| R3.3c 本地框架 | [固定任务与paired报告](implementation/R3-evaluation-framework.md)，原账本及交付证据重建 | 真实供应商/实际单Agent质量对照未执行，无通用单Agent调用入口 |
| R1.4 管理窗口 | 新冻结management_history、完整当前事实、历史hash/版本回读、预约前超限诊断 | 旧配置完整历史保持；模型语义使用质量待真实验收 |
| owned 容量 | 新冻结文件工具容量贯穿写入stage/库存/删除，旧大树结构化保留 | file-tools-only，不提供进程磁盘quota，不扩大清理权限 |

该历史批次的实际命令、故障修正、输入身份和门禁见[五节点验证](reports/five-node-validation-20261027.md)。
当前 CLI 的 `pump-live` 与库入口均需独立宿主授权，默认Fake路径继续兼容。
本轮测试仅使用合成凭据、本地TLS和临时数据；真实供应商及付费调用均为0。
原 [R3.3a组件](implementation/R3-https-transport.md)及[历史验证](reports/r3-https-transport-validation.md)保留其原版本范围。

前一补丁依据[独立工程审核](reviews/independent-2026-10-26.md)修复五类边界问题：
运行源及共享配置/输入文件先检查类型再读打开、doctor以实际EOF确认哈希、合法业务文件名贯通受控清理、
全CLI argv的UTF-8预检与有界错误输出、SSE默认obfuscation填充兼容。
实现规则见[ADR-079](adr/079-independent-boundary-review.md)，该补丁执行结果与原失败日志见
[验证报告](reports/independent-audit-validation.md)；不以历史成功代替当前提交门禁。

蓝图已将R3.3拆为本地TLS组件、原Store/账本集成、获准真实任务与单Agent对照三个节点。
管理角色视图与owned工作区贯通容量单列长任务前置，CLI可承担S1小任务验收，服务化后置。
R3.3a/b 已接入组件和原Store路径；R1.4与owned容量按新冻结策略实施，真实小任务与部署能力
仍按[开发计划](implementation/R3-controlled-live-plan.md)独立验收。

前一版本0.8.5-r3.2完成三个限定节点的实施：

- **R2a.3**：[有界只读部署诊断](implementation/R2a-deployment-diagnostics.md)，记录配置与实际路径来源、逐项稳定原因、共享读取预算及未检查边界。
- **R3.1**：[Responses 离线 HTTP 编解码](implementation/R3-offline-responses.md)，使用显式外部 profile，保留原 structured-v2 请求、宿主身份、原响应和可空用量。
- **R3.2**：[有界 Responses SSE 离线回放](implementation/R3-offline-responses.md)，校验分块、事件身份/次序及完整终态，支持确定性取消/deadline 输入，失败不释放部分工具。

新增独立 CLI `encode-model` / `replay-model` 只读取本地输入；不打开 Store、不发起网络请求、
不解析凭据、不生成真实 attempt 或执行工具。doctor 同样不打开 Store、不执行 launcher 或环境检查。
这三个节点不代表完整 R3 或真实模型闭环完成。设计见
[ADR-076](adr/076-doctor-deployment-diagnostics.md)、[ADR-077](adr/077-responses-offline-codec.md)
和 [ADR-078](adr/078-responses-sse-replay.md)；实际验证和限制以
[三节点报告](reports/r2a3-r3-offline-validation.md)及对应提交的 CI 为准，不从实施状态推导测试通过。

接续 R2.2 `c975be3` 的[拒绝成果纠正/用户升级](implementation/R2-rejected-task-resolution.md)和
[请求限定恢复](implementation/S1-request-recovery.md)，新增[模型逐任务 AMEND](implementation/R2-model-amendments.md)。
R2a.1接入[有界目录创建](implementation/R2a-directory-tool.md)，R2a.2接入
[分配恢复与明确弃置](implementation/R2a-allocation-recovery.md)：保存创建收据后无覆盖发布，
原代次恢复、旧407迁移、元数据弃置、历史路径保护及有界结算一起落地。
设计取舍见 [ADR-072](adr/072-r2-integration.md)、[ADR-073](adr/073-model-amendment-dispositions.md)、[ADR-074](adr/074-bounded-directory-tool.md)和[ADR-075](adr/075-owned-allocation-recovery.md)。
Dispatcher纠正与TeamLead共享冻结返工计数，新说明必须保留旧约束并经Worker重新确认；
这些动作和用户回答只恢复目标请求，不能扰动同库兄弟请求或其终态收尾租约。
原R1.3窗口/原反馈、AMEND/UNKNOWN门禁保持。先前增量的证据保留于
[R2.2联合复验](reports/r2-integration-validation.md)、[R2.3验证报告](reports/r2-model-amendment-validation.md)、[R2a.1验证报告](reports/r2a-directory-validation.md)和[R2a.2验证报告](reports/r2a-allocation-validation.md)，不替代本批提交的验证。

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
| Responses 离线协议 | 显式 provider profile、HTTP 文本/function_call 编解码、有界 SSE 回放；独立 encode-model/replay-model | 两个CLI仍零网络、零凭据解析、零工具派发。Schema/Completed 不授予权限；原wire字段不改标 |
| HTTPS / live工作流 | libcurl multi、显式CA/host验证、有界deadline/取消、原HTTP/SSE解码；原角色event pump与精确授权CLI | 自建loopback TLS，严格IPv4/显式路由，DNS名另需c-ares；零自动重试。费用未知停止，v2授权绑定独立签名计费；实际供应商与c-ares外部DNS验收未执行 |
| 签名计费 / 迟到核账 | 独立Ed25519收据、原challenge/字节身份、五维nullable账目、跨调用唯一归属、追加投影与只读重放 | 错签、错绑、重放、缺值、超预留和错误响应各按原账本限制；自定义中转契约需实际服务另行接入 |
| 运行资源 / 迁移 | 编译认可清单、35项固定资源、版本注册表及CMake安装 | 从无关cwd与删除安装来源后运行；prefix/DESTDIR、资源篡改/缺失/特殊文件拒绝、旧API/迁移兼容；配置和凭据外置 |
| 单 Agent / 评测证据 | 显式冻结共享原生对话与原权限代理；v1结构验收、v2原模型来源、v3独立验证单逻辑Agent连续性 | 五任务双条件合成工程链、跨重启与错标/缺反馈反例；真实供应商质量、成本及长对话语义效果未验收 |
| 事件/并发 | 持久inbox、逐任务AMEND、串行pump；冻结准入窗口/独立计数/Root检查点 | 旧快照不改写；到限可核对后续进；不清审计/不重置预算。CLI 已按请求恢复；默认全库维护仍保留。并发租约、只在确有在途时提升选中请求代次仍未实现 |
| 配置执行状态 | check/explain显示 runtime_support 和 validated_only 项 | 形状有效不代表墙钟/告警/压缩/并发/停滞检测已执行 |
| 记忆、MCP、跨功能审核 | 规格/参考契约 | 完整C++闭环及真实互通未实现 |
| 测试工具链 | 严格、UBSan、ASan、Clang、普通用户及umask矩阵 | 已配置六路CI：Release/两种umask、UBSan、CMake ASan、Make ASan、Clang18；Make ASan默认-O0，其他优化构建保留；本批结果须按报告和具体 CI 提交核对 |

## 阶段

S1 在实施中；近期优先级已调整为真实模型闭环的接口和运行时前置条件。
S2 独立审核/DAG，S3 记忆，S4 MCP/评测，S5 按证据扩展仍为后续阶段。
单 Agent 对照框架前移 S1，不再以整套 S2 完成为前提。
R2a.2已按[ADR-075](adr/075-owned-allocation-recovery.md)实现迁移、创建收据、精确恢复、弃置与清理联动。
R2a.3、R3.1、R3.2 的上述限定接口已实施；真实强沙箱部署正向验收保持独立。
R3.3a/b、两项长任务前置及受控event pump/CLI已按限定范围实施。迁移注册表与可安装资源
已补齐服务化之前置，但daemon/API继续后置。本批补齐版本化签名计费适配和单Agent逻辑身份执行器。
下一步按[R3.3c计划](implementation/R3-controlled-live-plan.md)，核验实际中转签发与所选端点能力；
在凭据、预算和明确授权具备后进行真实小任务与同条件对照，保留全部失败和未知样本。
离线codec profile继续固定real_calls_enabled=false；外层宿主live配置另有显式开关和独立执行授权，
默认关闭，max_paid_retries=0；CLI只有完整宿主授权后的受控入口，本次未使用真实供应商。
并发租约和选中请求按需提升代次继续开放，不因模型 AMEND 接入而视为完成。

历史进度保留在 CHANGELOG 与 `docs/reports/`；原 handoff 的51项产品验收记录保持原状。
本批不新增生产部署、真实模型调用、付费测试或独立第二模型评审。

## 2026-10-24 第二份审核接续

精确恢复已保存的三个提交，不把聊天中的未核验状态当成绩。设计/ADR先行；P1-3重绑与P1-1证据结算
保留原分项提交。P1-2本批接入冻结容量、独立池、预留/告警/暂停、Root CAS检查点及旧快照兼容。
原两个复现已转成回归，记录基线失败；运行状态和完整门禁见[本批报告](reports/audit24-recovery-validation.md)。

该历史增量实现为 0.7.2-s1.6，不代表三个P1的所有生产部署条件均已实测：强bwrap/cgroup正向仍缺环境。
R1容量问题现由显式新窗口部分覆盖，旧请求迁移/语义压缩仍开放；R2.2已联合接入同目标拒绝纠正/用户升级和按请求恢复；R2.3接入模型AMEND，R2a.1接入fs_mkdir，R2a.2补齐PREPARED分配恢复和明确弃置。
[逐项处置](reviews/claude-2026-10-24-disposition.md)区分已覆盖、实际修复和延期方案。
