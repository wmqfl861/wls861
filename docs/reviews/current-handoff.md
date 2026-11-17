# 最新交接

更新：2026-11-17。[#150](https://github.com/wmqfl861/wls861/pull/150) D-50、[#151](https://github.com/wmqfl861/wls861/pull/151) G3-B0 与 [#152](https://github.com/wmqfl861/wls861/pull/152) 测试分片已合入；当前产品基线为 [main de7f5b5](https://github.com/wmqfl861/wls861/commit/de7f5b5)，[v0.13.1](https://github.com/wmqfl861/wls861/tree/v0.13.1) 已推送。长期状态和宿主边界只在 [STATUS](../STATUS.md) 维护。

现在能做什么：G2 U1/U2 已真实交付，R11 完整组合对照达成，详见 [G2 记录](../reports/g2-first-real-run.md#r12出口与r11真实比较)。G3-B1 五任务×两拓扑均已运行、封存和分析；文件字节、完整 oracle、工作流、场景与费用分别列在 [B1 对照报告](../reports/g3-comparison.md)。

卡在哪：B1 有两格确认交付，其余保留验收待定、AMEND 未完成或场景未触发，不能拿不同终态作性能优劣结论。G3-C 候选定向测试通过，但 CI/D-40 尚待完成，F-29 原 6→4 目标仍未达。

当前候选：`feat/g3-fixed-single-task-20261117` 已正常导入 main，改动 94 产品行、171 测试行。固定单任务 opt-in 只省首次 Dispatcher 规划，合成同任务实测总调用 7→6、管理调用 5→4、Worker 始终两次；完整 brief、成果字节及保留的角色顺序一致。这不是 B1 真实对照或阶段出口，详细口径见 [G3 报告](../reports/g3-comparison.md)。

下一步：推送候选并在最终 head 完成 CI/D-40，实际核查受影响 ASan 的耗时，必要时保持全部用例和期限做分组。选择器已补全为 25 组，含全部四个 acceptance_runtime 分组及 #152 新增两组；之后按[前向计划 §15](claude-2026-11-16/04-plan.md#15-修订记录)推进 E-9 两 PR、新冻结格式与 B2，不改变数据库或现有默认值。

## 验证与证据

基线 main f9da424 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44081983219)与[源码快照](https://github.com/wmqfl861/wls861/actions/runs/44081983151)已独立核验 PASS：465/465 CTest（核心 279、ASan 93、UBSan 93），Make 两组 61+25 条命令通过，口径分别保留。ASan worker_tools 为 90.92/180 秒（50.51%），触发 D-20 后续分片要求，原 main PASS 不改写。

[#152](https://github.com/wmqfl861/wls861/pull/152) head [8c47b5f](https://github.com/wmqfl861/wls861/commit/8c47b5f) 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44145394773) 285/285、[受影响 ASan](https://github.com/wmqfl861/wls861/actions/runs/44145437310) 21/21 和[源码检查](https://github.com/wmqfl861/wls861/actions/runs/44145394706)均 PASS。Worker runtime/output/environment 三组托管 ASan 为 29.63/36.72/21.44 秒，均低于既有 180 秒期限的 50%；已合入 `de7f5b5`。新 main 的[完整矩阵 44152280898](https://github.com/wmqfl861/wls861/actions/runs/44152280898)仍 pending，不能沿用 PR PASS 代替该运行结果。

G3-C 本机普通用户 Release：config_loader 0.44 秒、acceptance_runtime 12/12（13.36 秒）、experience 10.12 秒；38 项契约和源码完整性检查通过。Release×10 为验收 133.6 秒、经验 101.2 秒，仅为相对既有 sanitizer 180 秒期限的估算，不是实际 ASan PASS；候选未做真实调用或本机完整矩阵。CI 清单 YAML/95 项 CTest 注册/25 组选择一致性检查通过，导入与文档更新未重复产品测试。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：旧 main 核验在 `run-intake/hosted-main151-f9da424/verification.json`；B1 原件定位和汇总哈希见 [G3 报告](../reports/g3-comparison.md)；候选结果、初始失败和修正记录在 `g3-c-evidence/result.md`，两次选择器检查原件同目录。原始日志、数据库、凭据和私有控制器不入 Git。
