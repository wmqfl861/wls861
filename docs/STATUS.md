现在能做什么：0.12.0 已通过 G2-P；Responses 与 Chat 非流式工作流完成合成交付；普通单 Agent 可提交原始成果，评估工具支持三条件、15 个固定样本位；G4-A1/B 已合入，N1 停止提示与 N2 固定成果核对已完成。
还不能做什么：真实 WLS 调用仍为 0，三条件质量对照未做，普通单 Agent 的提交仍待业务验收；G4 原生验收 BLOCKED，`/input` 与环境卡片执行检查未完成，Windows 11 为 NOT_RUN。
需要 owner 做什么：在运行 agent 的 shell 中导出 `WLS_G2_API_KEY`，然后明确告知“密钥已就绪”；两项满足前不发任何真实请求。

# 当前状态

更新：2026-11-14。实现 **0.12.0**，当前蓝图 **v0.4.1**、已采纳目标蓝图 **v0.5**，Store **410**。没有数据库版本变更。

本轮收敛已在 [converged-20261114](https://github.com/wmqfl861/wls861/tree/converged-20261114)（`f4fbb62`）完成，七项标准通过；随后合入 [N2 #99](https://github.com/wmqfl861/wls861/pull/99) 与 [N1 #100](https://github.com/wmqfl861/wls861/pull/100)。标签保留原收敛点，见[收敛报告](reports/convergence-20261114.md)。

当前 CI：[R3 分片 #103](https://github.com/wmqfl861/wls861/pull/103)已合并，49 个原方法分为 32/12/5 三组，CTest 共 90 个注册；[PR 三核心](https://github.com/wmqfl861/wls861/actions/runs/43602255761)在实际检查输入 `50e46b2` 上各 90/90 通过。合入后 main `45e9e05` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43603725398)仍待完成，后续见[最新 main Actions](https://github.com/wmqfl861/wls861/actions/workflows/ci.yml?query=branch%3Amain)。

此前 main `f7f38e8` 的 88 注册[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43576774296)与[定时矩阵](https://github.com/wmqfl861/wls861/actions/runs/43585861193)均通过；后者 `r3_evaluation` 的 ASan 耗时 187.30/360 秒，触发此次 D-20 分片。旧 acceptance 超时项已由 [#95](https://github.com/wmqfl861/wls861/pull/95) 分片关闭。两次分片均保留原用例、断言和时限，历史失败保留。

## 已有能力

| 能力 | 当前边界与入口 |
| --- | --- |
| G1、G2-P | 已通过；最小工具契约、受限纠错、按次预约与失败记账、至多两次合格重试、files 默认模板、成果原名导出均已合入；见[八场景预演](reports/g2p-rehearsal.md) |
| N1、N2 | 停止提示区分持久限制、目标约束与可重试状态，仅附白名单失败类别；`python3 scripts/check_g2_outputs.py OUTPUT smoke`（或 `three`）核对固定任务成果并输出 JSON |
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

等待：密钥。N1、N2、N3 均已完成；按 [D-26](reviews/claude-2026-11-14/02-decisions.md#35-d-26-收敛之后密钥到来之前)，空档仅可做 G6-B 余下的构建提速、测试耗时治理和文档精简，G7 及以后功能阶段暂不开始。密钥和明确就绪通知到位、手头 PR 合并后按 G2 计划执行。

历史实现、逐次运行及失败保留在 [CHANGELOG](../CHANGELOG.md)、[G1 交接](reviews/g1-main-handoff-20261112.md)、[G2-P 报告](reports/g2p-rehearsal.md)与[全代码审核](reviews/code-audit-20261027.md)；本页只记录现状与入口。
