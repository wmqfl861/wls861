现在能做什么：0.12.0 已通过 G2-P；Responses 与 Chat 非流式工作流完成合成交付；普通单 Agent 可提交原始成果，评估工具支持三条件、15 个固定样本位；G4-A1/B 已合入。
还不能做什么：真实 WLS 调用仍为 0，三条件质量对照未做，普通单 Agent 的提交仍待业务验收；G4 原生验收 BLOCKED，`/input` 与环境卡片执行检查未完成，Windows 11 为 NOT_RUN。
需要 owner 做什么：在运行 agent 的 shell 中导出 `WLS_G2_API_KEY`，然后明确告知“密钥已就绪”；两项满足前不发任何真实请求。

# 当前状态

更新：2026-11-14。实现 **0.12.0**，当前蓝图 **v0.4.1**、已采纳目标蓝图 **v0.5**，Store **410**。没有数据库版本变更。

本轮：[CI 分片 #95](https://github.com/wmqfl861/wls861/pull/95)、[审核文档 #96](https://github.com/wmqfl861/wls861/pull/96)、[G4-A1/B #97](https://github.com/wmqfl861/wls861/pull/97) 已合并，历史分支与工作树已清理；本报告 PR、最终 main 完整矩阵和标签仍待收尾，见[收敛报告](reports/convergence-20261114.md)。

当前 CI：main `71de73d` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43562207559)运行中；此前 main `33c96d8` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43557663614)九作业通过，四个 acceptance 注册均 ≤90 秒。#95 按测试体量拆分，旧 acceptance 超时项关闭；用例、断言和时限保持。

## 已有能力

| 能力 | 当前边界与入口 |
| --- | --- |
| G1、G2-P | 已通过；最小工具契约、受限纠错、按次预约与失败记账、至多两次合格重试、files 默认模板、成果原名导出均已合入；见[八场景预演](reports/g2p-rehearsal.md) |
| 四角色工作流 | 原版本、授权、首次派发、UNKNOWN、原字节提交/验收/交付守卫保留；运行时自有工作区清理不等于私有行全局清理 |
| 文件与输出 | 文件工具及新 structured-v3 的 UTF-8 输出切片/前 8 KiB 预览已接通；旧格式与历史记录不回填 |
| Responses / Chat | 两协议各八场景合成交付通过；Chat 非流式已接预约、发送与结算，R3 评估仍拒绝 Chat |
| G4 | A1/B 已合并，standard 仍显式选用、默认 Strict；原生验收 BLOCKED：托管尝试与本机均未通过，托管拒绝的根因未定，见[收敛报告](reports/convergence-20261114.md) |
| G3 | plain 单 Worker 与 v4 三条件工具就绪；停在不可变 SUBMITTED、业务验收待办；质量为 not_assessed，见[用法](implementation/R3-single-agent-baseline.md#5-普通单-agent-与-v4-三条件用法) |
| G6 | #93 已将 Store 返工域单独编译；其余并行与构建提速未完成，单次测量不代表稳定性能收益 |

## 当前限制

- 合成结果不代表真实模型验收。真实/付费 WLS 调用仍为 0；密钥与明确就绪通知到位后按 [G2 计划](reviews/claude-2026-11-13/04-plan.md#3-g2-的步骤)和 [11-14 补充](reviews/claude-2026-11-14/04-next.md#2-g2步骤不变补三处)执行，每请求 ≤1 美元、G2 合计 ≤10 美元。首选 Responses / gpt-5-mini，其他 Chat 端点按 D-27。
- G4 的 `/input`、环境卡片实际执行检查、无 cgroup 启动与全树回收仍待后续条件；Windows 11 NOT_RUN。
- 六个新格式在 G2 出口冻结，此前允许原地调整；framed-v1 / structured-v2 的旧请求回归保持。
- daemon/API、私有行清理 runner、完整记忆和独立审核闭环仍未实现；D-12 取消私有证据托管要求，原始证据至少保留 30 天。

## 下一步

按 [11-14 决定](reviews/claude-2026-11-14/02-decisions.md)和[后续安排](reviews/claude-2026-11-14/04-next.md)完成收敛收尾，然后做 N1 停止提示、N2 成果核对脚本；N3 STATUS 精简随本报告完成。N1、N2 尚未实施；之后等待密钥，不开始 G7 及以后功能阶段。

历史实现、逐次运行及失败保留在 [CHANGELOG](../CHANGELOG.md)、[G1 交接](reviews/g1-main-handoff-20261112.md)、[G2-P 报告](reports/g2p-rehearsal.md)与[全代码审核](reviews/code-audit-20261027.md)；本页只记录现状与入口。
