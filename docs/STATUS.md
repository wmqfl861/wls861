# 当前实现 **0.8.4-r2a.2**，目标蓝图 **v0.4.1**。

接续 R2.2 `c975be3` 的[拒绝成果纠正/用户升级](implementation/R2-rejected-task-resolution.md)和
[请求限定恢复](implementation/S1-request-recovery.md)，新增[模型逐任务 AMEND](implementation/R2-model-amendments.md)。
R2a.1接入[有界目录创建](implementation/R2a-directory-tool.md)，R2a.2接入
[分配恢复与明确弃置](implementation/R2a-allocation-recovery.md)：保存创建收据后无覆盖发布，
原代次恢复、旧407迁移、元数据弃置、历史路径保护及有界结算一起落地。
设计取舍见 [ADR-072](adr/072-r2-integration.md)、[ADR-073](adr/073-model-amendment-dispositions.md)、[ADR-074](adr/074-bounded-directory-tool.md)和[ADR-075](adr/075-owned-allocation-recovery.md)。
Dispatcher纠正与TeamLead共享冻结返工计数，新说明必须保留旧约束并经Worker重新确认；
这些动作和用户回答只恢复目标请求，不能扰动同库兄弟请求或其终态收尾租约。
原R1.3窗口/原反馈、AMEND/UNKNOWN门禁保持。当前测试及发布状态只以
[R2.2联合复验](reports/r2-integration-validation.md)、[R2.3验证报告](reports/r2-model-amendment-validation.md)、[R2a.1验证报告](reports/r2a-directory-validation.md)、[R2a.2验证报告](reports/r2a-allocation-validation.md)和对应提交的CI为准。

## 能力矩阵

| 能力 | 当前实现 | 已有证据 / 缺口 |
| --- | --- | --- |
| 四角色、版本、幂等、UNKNOWN | C++ Store + FakeModel 有界流程 | 真实 SQLite/文件/跨进程回归；不证明模型决策质量 |
| 文件/进程 | fs_list/read/search/mkdir/write/patch；完整强证明与前后清单可结算失败，Root专用核对；子进程固定0077 | 真实目录/文件和监督夹具；mkdir有投影配额、首次派发及UNKNOWN故障回归；强 bwrap/cgroup 部署正向和后台操作待完成 |
| 问答/候选/验收 | Worker问题、原字节提交、TL/Dispatcher共享有限返工、验收及用户升级 | 固定脚本实现拒绝→新说明/确认→重提/验收及耗尽上报；回答不追加额度。通用任务重定义仍未实现 |
| AMEND | Dispatcher模型提交新计划及逐节点update/keep/cancel；遗漏节点按同计划续办 | 捕获请求/任务CAS和团队代次，遗漏保留待办、keep须新说明/ack；双格式/重开/预算/兄弟隔离回归。真实模型判断质量尚未验收 |
| 交付/清理 | 不可变包、精确回执、经验作业、固定清单受控 work 删除 | 实际发布/删除/恢复；私有行、owner收据保留；全局cleanup仍PENDING；PREPARED有原收据恢复/显式弃置；弃置残留保留、不可重用，doctor现场诊断待接入 |
| 上下文/模型 | 冻结选择v1/v2和Worker window-v1；近期完整轮次、旧/大结果原证据引用；模型回读与持久频率限制 | FakeModel大文件多轮/重开继续、模型回读原字节；旧请求不迁移。LLM语义压缩、管理角色窗口、实际wire仍未实现；完整当前契约超限仍拒绝 |
| 事件/并发 | 持久inbox、逐任务AMEND、串行pump；冻结准入窗口/独立计数/Root检查点 | 旧快照不改写；到限可核对后续进；不清审计/不重置预算。CLI 已按请求恢复；默认全库维护仍保留。并发租约、只在确有在途时提升选中请求代次仍未实现 |
| 配置执行状态 | check/explain显示 runtime_support 和 validated_only 项 | 形状有效不代表墙钟/告警/压缩/并发/停滞检测已执行 |
| 记忆、MCP、跨功能审核 | 规格/参考契约 | 完整C++闭环及真实互通未实现 |
| 测试工具链 | 严格、UBSan、ASan、Clang、普通用户及umask矩阵 | 六路CI：Release/两种umask、UBSan、CMake ASan、Make ASan、Clang18；Make ASan默认-O0，其他优化构建保留；实际结果见报告 |

## 阶段

S1 在实施中；近期优先级已调整为真实模型闭环的接口和运行时前置条件。
S2 独立审核/DAG，S3 记忆，S4 MCP/评测，S5 按证据扩展仍为后续阶段。
单 Agent 对照框架前移 S1，不再以整套 S2 完成为前提。
R2a.2已按[ADR-075](adr/075-owned-allocation-recovery.md)实现迁移、创建收据、精确恢复、弃置与清理联动。
下一聚焦增量为R2a.3 doctor实际部署路径/具体阻塞说明，然后接R3单协议离线回放。
[ADR-076](adr/076-doctor-deployment-diagnostics.md)已写成设计草案；新增诊断字段、路径检查和验收尚未实现。
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
