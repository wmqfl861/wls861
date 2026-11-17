# 最新交接

更新：2026-11-17。[#150](https://github.com/wmqfl861/wls861/pull/150) D-50 与 [#151](https://github.com/wmqfl861/wls861/pull/151) G3-B0 已合入，[v0.13.1](https://github.com/wmqfl861/wls861/tree/v0.13.1) 已推送；当前产品基线为 [main f9da424](https://github.com/wmqfl861/wls861/commit/f9da424)。长期状态和宿主边界只在 [STATUS](../STATUS.md) 维护。

现在能做什么：G2 U1/U2 已真实交付，R11 完整组合对照达成，详见 [G2 记录](../reports/g2-first-real-run.md#r12出口与r11真实比较)。G3-B1 五任务×两拓扑均已运行、封存和分析；文件字节、完整 oracle、工作流、场景与费用分别列在 [B1 对照报告](../reports/g3-comparison.md)。

卡在哪：B1 有两格确认交付，其余保留验收待定、AMEND 未完成或场景未触发，不能拿不同终态作性能优劣结论。F-29 原 6→4 目标仍未达；[#152](https://github.com/wmqfl861/wls861/pull/152) 测试分片尚未合入，状态见下。

下一步：独立推进 G3-C 固定单任务模板 opt-in，仅计划省首次 Dispatcher 规划；测试尚待执行，目标口径与验收后经验捕获限制见 [B1 报告](../reports/g3-comparison.md)，决定见[前向计划 §15](claude-2026-11-16/04-plan.md#15-修订记录)。之后 E-9 两 PR、新冻结格式，再做 B2；不改变数据库或现有默认值，不提前登记阶段出口。

## 验证与证据

基线 main f9da424 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44081983219)与[源码快照](https://github.com/wmqfl861/wls861/actions/runs/44081983151)已独立核验 PASS：465/465 CTest（核心 279、ASan 93、UBSan 93），Make 两组 61+25 条命令通过，口径分别保留。ASan worker_tools 为 90.92/180 秒（50.51%），触发 D-20 后续分片要求，原 main PASS 不改写。

[#152](https://github.com/wmqfl861/wls861/pull/152) 当前 head [8c47b5f](https://github.com/wmqfl861/wls861/commit/8c47b5f)：[核心](https://github.com/wmqfl861/wls861/actions/runs/44145394773)与[源码检查](https://github.com/wmqfl861/wls861/actions/runs/44145394706)通过；[受影响 ASan](https://github.com/wmqfl861/wls861/actions/runs/44145437310)仍待独立核验，不登记为已合并或全部门禁完成。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：main CI 核验在 `run-intake/hosted-main151-f9da424/verification.json`；B1 原件定位和汇总哈希见 [B1 报告](../reports/g3-comparison.md)。本次仅据已完成分析更新文档，没有读取原始 live DB、调用模型或重跑产品测试；原始证据留数据盘。
