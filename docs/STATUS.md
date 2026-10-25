# 当前实现 **0.8.1-r2.2**，目标蓝图 **v0.4.1**。基于已发布 `19ea603`，整合上轮
[拒绝成果纠正/用户升级](implementation/R2-rejected-task-resolution.md)和远端候选的
[请求限定恢复](implementation/S1-request-recovery.md)。设计取舍见 [ADR-072](adr/072-r2-integration.md)。
Dispatcher纠正与TeamLead共享冻结返工计数，新说明必须保留旧约束并经Worker重新确认；
这些动作和用户回答只恢复目标请求，不能扰动同库兄弟请求或其终态收尾租约。
原R1.3窗口/原反馈、AMEND/UNKNOWN门禁保持。当前测试及发布状态只以
[联合验证报告](reports/r2-integration-validation.md)和对应提交的CI为准。

## 能力矩阵

| 能力 | 当前实现 | 已有证据 / 缺口 |
| --- | --- | --- |
| 四角色、版本、幂等、UNKNOWN | C++ Store + FakeModel 有界流程 | 真实 SQLite/文件/跨进程回归；不证明模型决策质量 |
| 文件/进程 | fs_list/read/search/write/patch；完整强证明与前后清单可结算失败，Root专用核对；子进程固定0077 | 真实文件和监督夹具；强 bwrap/cgroup 部署正向、mkdir 和后台操作待完成 |
| 问答/候选/验收 | Worker问题、原字节提交、TL/Dispatcher共享有限返工、验收及用户升级 | 固定脚本实现拒绝→新说明/确认→重提/验收及耗尽上报；回答不追加额度。模型AMEND决策及任意任务重定义未实现 |
| 交付/清理 | 不可变包、精确回执、经验作业、固定清单受控 work 删除 | 实际发布/删除/恢复；私有行、owner收据保留；全局cleanup仍PENDING；PREPARED分配中断缺恢复出口 |
| 上下文/模型 | 冻结选择v1/v2和Worker window-v1；近期完整轮次、旧/大结果原证据引用；模型回读与持久频率限制 | FakeModel大文件多轮/重开继续、模型回读原字节；旧请求不迁移。LLM语义压缩、管理角色窗口、实际wire仍未实现；完整当前契约超限仍拒绝 |
| 事件/并发 | 持久inbox、逐任务AMEND、串行pump；冻结准入窗口/独立计数/Root检查点 | 旧快照不改写；到限可核对后续进；不清审计/不重置预算。CLI 已按请求恢复；默认全库维护仍保留。并发租约、只在确有在途时提升选中请求代次仍未实现 |
| 配置执行状态 | check/explain显示 runtime_support 和 validated_only 项 | 形状有效不代表墙钟/告警/压缩/并发/停滞检测已执行 |
| 记忆、MCP、跨功能审核 | 规格/参考契约 | 完整C++闭环及真实互通未实现 |
| 测试工具链 | 严格、UBSan、ASan、Clang、普通用户及umask矩阵 | 六路CI：Release/两种umask、UBSan、CMake ASan、Make ASan、Clang18；Make ASan默认-O0，其他优化构建保留；实际结果见报告 |

## 阶段

S1 在实施中；近期优先级已调整为真实模型闭环的接口和运行时前置条件。
S2 独立审核/DAG，S3 记忆，S4 MCP/评测，S5 按证据扩展仍为后续阶段。
单 Agent 对照框架前移 S1，不再以整套 S2 完成为前提。

历史进度保留在 CHANGELOG 与 `docs/reports/`；原 handoff 的51项产品验收记录保持原状。
本批不新增生产部署、真实模型调用、付费测试或独立第二模型评审。

## 2026-10-24 第二份审核接续

精确恢复已保存的三个提交，不把聊天中的未核验状态当成绩。设计/ADR先行；P1-3重绑与P1-1证据结算
保留原分项提交。P1-2本批接入冻结容量、独立池、预留/告警/暂停、Root CAS检查点及旧快照兼容。
原两个复现已转成回归，记录基线失败；运行状态和完整门禁见[本批报告](reports/audit24-recovery-validation.md)。

该历史增量实现为 0.7.2-s1.6，不代表三个P1的所有生产部署条件均已实测：强bwrap/cgroup正向仍缺环境。
R1容量问题现由显式新窗口部分覆盖，旧请求迁移/语义压缩仍开放；R2.2已联合接入同目标拒绝纠正/用户升级和按请求恢复；模型AMEND、R2a分配中断/fs_mkdir仍待实现。
[逐项处置](reviews/claude-2026-10-24-disposition.md)区分已覆盖、实际修复和延期方案。
