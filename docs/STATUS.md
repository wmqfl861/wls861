# 当前实现与证据

当前补丁 **0.7.2-s1.6**，目标蓝图 **v0.4.1**。本批为 Claude 审核后的可靠性修复，
本批来源为 main e7fa64b、审核材料合并012bed74及恢复的分项提交673a6df。
发布与验证分开记录：本地完成不等于GitHub main已更新，准确边界见[第二份审核报告](reports/audit24-recovery-validation.md)。
原始审核与采纳范围见[逐项决定](reviews/claude-disposition.md)，首次结果见[修复验证](reports/claude-review-fixes-validation.md)，接续发布依据见[本轮复验](reports/claude-review-resume-validation.md)。

## 能力矩阵

| 能力 | 当前实现 | 已有证据 / 缺口 |
| --- | --- | --- |
| 四角色、版本、幂等、UNKNOWN | C++ Store + FakeModel 有界流程 | 真实 SQLite/文件/跨进程回归；不证明模型决策质量 |
| 文件/进程 | fs_list/read/search/write/patch；完整强证明与前后清单可结算失败，Root专用核对；子进程固定0077 | 真实文件和监督夹具；强 bwrap/cgroup 部署正向、mkdir 和后台操作待完成 |
| 问答/候选/验收 | Worker问题、原字节提交、TL有限返工、Dispatcher验收 | 固定脚本闭环；否定验收/返工耗尽的受控后继尚缺 |
| 交付/清理 | 不可变包、精确回执、经验作业、固定清单受控 work 删除 | 实际发布/删除/恢复；私有行、owner收据保留；全局cleanup仍PENDING；PREPARED分配中断缺恢复出口 |
| 上下文/模型 | 自定义帧、完整历史、FakeModel；容量错误为quota | 没有压缩或真实适配器；大结果多轮仍会到限。Native工具消息与wire指纹待接入 |
| 事件/并发 | 持久inbox、逐任务AMEND、串行pump；冻结准入窗口/独立计数/Root检查点 | 旧快照不改写；到限可核对后续进；不清审计/不重置预算。全库恢复范围和全局并发仍待完成 |
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

当前实现为 0.7.2-s1.6，不代表三个P1的所有生产部署条件均已实测：强bwrap/cgroup正向仍缺环境。
R1上下文容量、R2否定验收/全库恢复范围、R2a分配中断/fs_mkdir等仍开放；新存储检查点不解决这些问题。
[逐项处置](reviews/claude-2026-10-24-disposition.md)区分已覆盖、实际修复和延期方案。
