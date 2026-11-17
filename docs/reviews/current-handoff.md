# 最新交接

更新：2026-11-17。G3-C1 [#154](https://github.com/wmqfl861/wls861/pull/154) 已合入 [main 2213c27](https://github.com/wmqfl861/wls861/commit/2213c27fd673c3a4c298ba713324da12d1513228)，这是 C2 的精确基线；[v0.13.1](https://github.com/wmqfl861/wls861/tree/v0.13.1) 已推送。长期状态和宿主边界见 [STATUS](../STATUS.md)。

现在能做什么：G2 U1/U2 已真实交付，R11 完整组合对照达成，详见 [G2 记录](../reports/g2-first-real-run.md#r12出口与r11真实比较)。G3-B1 十格已封存，结果混合；调用、文件字节、完整 oracle、工作流、费用未知和 C1/C2 合成口径集中在 [G3 报告](../reports/g3-comparison.md)。

当前候选：`feat/g3-fixed-initial-work-order-20261117` 在上述 main 上增加严格的冻结首次工单 opt-in，复用 TeamLead 事务及 Worker 确认。产品 114 行，C1+C2 合计 208/250；测试 173 行，另有 CTest/CI 维护 11 行。合成总调用 7→6→5、固定开销 6→5→4，检查、显式验收、验收后经验仍独立；原 F-29 检查/经验合并机制未实施。

卡在哪：C2 本机定向验证通过，尚待推送、最终 head CI/D-40 和父线程审核；没有以 PR154 的 PASS 代替 C2。B1 不同终态仍不能作同等验收性能比较，G3 未达真实出口。

下一步：父线程正常推送/建 PR，在最终 head 跑核心、源码和 26 组受影响 ASan，复核实际 D-20 计时后按既定条件合并。然后继续 E-9 两 PR 与新冻结格式，B2 本轮不执行；`partial_amend` 与固定单任务的适用性仍需在 B2 前解决。唯一前向计划为 [11-16 §15](claude-2026-11-16/04-plan.md#15-修订记录)。

## 验证与证据

PR154 head `bc3ae070973686b4f7051e0855962c51edb74a63` 的[核心 44153210046](https://github.com/wmqfl861/wls861/actions/runs/44153210046) 285/285、[源码 44153209908](https://github.com/wmqfl861/wls861/actions/runs/44153209908) 与[受影响 ASan 44153239418](https://github.com/wmqfl861/wls861/actions/runs/44153239418) 25/25 独立核验 PASS。核心/源码实际 checkout 为 merge-ref `1bf6d36888243875de99cd1c650d061a3115ebf3`（与 head 同树），ASan 为精确 head；不混同新的 main 完整矩阵。四个 acceptance ASan 组为 71.38/42.36/38.13/76.10 秒，均低于 180 秒期限的 50%。

C2 本机普通用户 UID 1000、GCC 13.3 Release：config_loader 0.57 秒；acceptance 12/12（13.53 秒）、initialization 3/3（6.01 秒）、experience 10.11 秒、memory 6.77 秒、guidance 13.14 秒通过；38 项契约与源码完整性通过。新增分组 Release×10 为 60.1 秒，小于 90 秒；这不是实际 ASan PASS。旧 acceptance 保留原 12 用例和托管计时依据；新旧分组均保留 90 秒基础期限及 sanitizer 倍率。

CTest 实际注册 96 项，ASan 选择器完整匹配 26 组，含全部五个 acceptance 分组；Make 无参数入口仍运行全部 28 用例。没有放宽断言/期限、运行完整本机 sanitizer 或调用真实模型。首轮出现测试夹具段错误；临时对象生命周期修正曾与编译重叠，完整重编译后通过。两次负例造状态先后命中 inbox 一致性和 task state_version 约束，已按原约束修正，原失败未删除。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：PR154 为 `run-intake/hosted-pr154-bc3ae07/verification.json`；C2 命令、原始日志、字节诊断和交接为 `g3-c2-evidence/`，构建为 `g3-c2-release/`。旧 scheduled Standard native 的核验补记在 [G4 记录](../reports/g4-run-tool.md)，不代表当前 main 或 U3/U4。数据库、凭据、长日志、构建和本机规则不入 Git。
