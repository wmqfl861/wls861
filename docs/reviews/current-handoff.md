# 最新交接

更新：2026-11-16。实现 0.13.0，Store 410；长期状态见 [STATUS](../STATUS.md)。

现在能做什么：N3 的冻结只读 `/input` 与总字节限制已由 [#136](https://github.com/wmqfl861/wls861/pull/136) 合入；N4 的冻结环境版本检查、原 BLOB 结果卡与 Worker 预约前重新 capture 已由 [#137](https://github.com/wmqfl861/wls861/pull/137) 合入 `87c31473`。最终 d365 核心 270/270、源码检查、D-40 14/14 和同请求 input/version 组合 native 独立复核均通过。R6 [`v0.13.0`](https://github.com/wmqfl861/wls861/tree/v0.13.0) 标签已推送，无 Release。

卡在哪：W128 候选止于 TL check 的 1 MiB capture guard，已归档且未合 main；F-15 写入/提交扩容仍未实现，G4 未达阶段出口。新 main `87c31473` 完整矩阵尚未完成，不能沿用 N3 main 的成功；长期环境和真实验收状态见 [STATUS](../STATUS.md)。

下一步：先为大成果上下文重复设计引用/分页或减少重复的方案，再重测 F-15；本轮不继续扩大容量，保持 0.13.0。G4-D 真实 run 等合规原生宿主可用后启动，CI 不调用真实模型。W128 已按 D-23 保险归档、推送标签并复核后删除清洁分支/工作树，标签与原件见 [阶段记录](../reports/g4-run-tool.md)。不重放失败 R2/R3/R5，不做第三轮 SSE 修补。

验证边界：合 N3 后 Release 10/10 通过；本机 ASan 选取七套，六套通过，event_runtime 180.79 秒超时，无 ASan 错误输出；此前 worker 超时单独保留，按 D-39 未重跑。native 入口已编译，本机未运行；真实模型调用 0。W128 的正式失败与一次不同 ELF 的私人计数诊断分别登记，归档标签已推送，构建与原始证据保留。

N3/N4 的实际行为、CI 链接、失败和超时原件集中在 [G4 阶段记录](../reports/g4-run-tool.md)；环境 opt-in 用法见 [Worker tools](../implementation/S1-worker-tools.md#g4-n4显式冻结的环境版本检查)。源码检查、合成测试、托管 native 和真实模型验收分别登记。

G2 的 U1/U2 未通过；R2/R3 上游失败、R5 无序号扩展事件拒绝均保留原件，E1 已保守回退。实际用量、未知 usage 和 R0 原文件见 [G2 运行记录](../reports/g2-first-real-run.md)，失败请求不取消、不重发、不回填。

私有任务根 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/`：N3 原件 `g4-input-validation/evidence/`，N4 `g4-env-evidence/`，托管原件 `evidence/n3-pr136-ci/`、`evidence/n4-pr137-ci/`。凭据、DB、构建包、长日志、原始响应和本机规则仅留私有位置。
