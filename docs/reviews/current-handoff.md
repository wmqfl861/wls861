# 最新交接

更新：2026-11-17。实现 0.13.0，Store 410；长期状态见 [STATUS](../STATUS.md)。本轮授权见 [D-46 至 D-57](claude-2026-11-17/03-decisions.md)，唯一前向计划已应用 [11-17 修订](claude-2026-11-17/04-plan-revisions.md)。

现在能做什么：N3 的冻结只读 `/input` 与总字节限制已由 [#136](https://github.com/wmqfl861/wls861/pull/136) 合入；N4 的冻结环境版本检查、原 BLOB 结果卡与 Worker 预约前重新 capture 已由 [#137](https://github.com/wmqfl861/wls861/pull/137) 合入 `87c31473`。最终 d365 核心 270/270、源码检查、D-40 14/14 和同请求 input/version 组合 native 独立复核均通过。R6 [`v0.13.0`](https://github.com/wmqfl861/wls861/tree/v0.13.0) 标签已推送，无 Release。

卡在哪：W128 候选止于 TL check 的 1 MiB capture guard，已归档且未合 main；F-15 写入/提交扩容仍未实现，G4 未达阶段出口。main `87c31473` [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43971296505) 10/10 jobs、450/450 CTest、Make-ASan shard 60+25 命令通过，测试失败/超时/跳过/未运行均 0；长期环境和真实验收状态见 [STATUS](../STATUS.md)。

下一步按修订后的 [计划 §3](claude-2026-11-16/04-plan.md#3-g2-余下的步骤) 顺序执行：

1. 先分别合入 D-46 观察模式账目就绪、D-48 重试资格；此前不发新的真实请求。本次只接入文档，两个产品修复尚未完成。
2. R3-stream-check 完整备份并核对清单后，按 D-49 用当前 main、原命令与原配置接着跑一次，新调用不超过 20；保留原冻结流式与零重试，守卫拒绝就封存。R2-stream-check、R5 继续封存。
3. 新 U1、必要时的新 U2 和 G2-7 复验用非流式、180 秒、冻结 `max_paid_retries: 2`；超时不重试。G2-7 依次做 E-14、E-11、E-1；通过后按 D-57 出 0.13.1。
4. G2 出口后转 G3。G4 的真实 U3/U4 等合规宿主，之前不出口、不升 0.14.0；D-50 SSE 扩展事件留到 R12 之后或 G3 期间。

验证边界：合 N3 后 Release 10/10 通过；本机 ASan 选取七套，六套通过，event_runtime 180.79 秒超时，无 ASan 错误输出；此前 worker 超时单独保留，按 D-39 未重跑。native 入口已编译，本机未运行；真实模型调用 0。W128 的正式失败与一次不同 ELF 的私人计数诊断分别登记，归档标签已推送，构建与原始证据保留。

N3/N4 的实际行为、CI 链接、失败和超时原件集中在 [G4 阶段记录](../reports/g4-run-tool.md)；环境 opt-in 用法见 [Worker tools](../implementation/S1-worker-tools.md#g4-n4显式冻结的环境版本检查)。源码检查、合成测试、托管 native 和真实模型验收分别登记。

G2 的 U1/U2 未通过；R2/R3 上游失败、R5 无序号扩展事件拒绝均保留原件，E1 已回退，D-51 的重新合入待执行。实际用量、未知 usage 和 R0 原文件见 [G2 运行记录](../reports/g2-first-real-run.md)；D-31 的原导出重放已完成，不再重复，未知用量不回填。

私有任务根 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/`：N3 原件 `g4-input-validation/evidence/`，N4 `g4-env-evidence/`，托管原件 `evidence/n3-pr136-ci/`、`evidence/n4-pr137-ci/`。凭据、DB、构建包、长日志、原始响应和本机规则仅留私有位置。
