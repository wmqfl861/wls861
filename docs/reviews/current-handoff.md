# 最新交接

更新：2026-11-17。main 已合入 [#158](https://github.com/wmqfl861/wls861/pull/158)，提交 [59c568f](https://github.com/wmqfl861/wls861/commit/59c568f75dd7b4494daa1773f03f29689f3a47b6)；实现 0.13.1、Store 410。长期能力及限制只在 [STATUS](../STATUS.md) 维护。

现在能做什么：G2 U1/U2 已真实交付；G3-C1/C2 的显式初始计划、工单使同一合成文件任务总调用 7→6→5。E9 已有严格的双工具历史表示，生产配置和批量派发仍关闭；B1 十格的混合结果、用量和费用未知见 [G3 报告](../reports/g3-comparison.md)。

在制：E9 第二切片采用两个不同规范相对目标的顺序 fs_write；同一候选已纳入取消 token 贯通独立提交和完整回归，生产批次接线、容量证明及测试尚在实施。保留409行产品预算和测试两倍限制；不是所有文件工具的批处理。

并行：G4 的 CSV、函数与测试准备均已合入，准确 final-head core/source/native 已核验，只到 SUBMITTED/check pending，未做真实业务验收或交付。取消前置已有真实红绿和6/6 Release，最终受影响 ASan 要包含完整 live_workflow；活动 Worker SIGINT→AMEND→原命令恢复的联合候选已完成代码及本机拒绝/既有回归，正在独立工作树等待源码复核和托管原生验证，完整场景仍 NOT_PREPARED；其取消依赖须先进入main才可合并。

下一步：完成 E9 效果前容量检查、原冻结配置/Store独立重算与顺序执行，再核验最终输入和 B2。fixed-single-task 不适用于 partial_amend；旧 v4 不认证新宿主初始化，不伪造等价验收或性能 PASS。

## 验证与证据

main `67f52f0` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44172025562)及[源码](https://github.com/wmqfl861/wls861/actions/runs/44172025451)已核验：10/10 jobs、480/480 CTest（core288、ASan96、UBSan96）、Make61+25；96个 ASan 套件均低于半时限，最高30.49%。main `f31647d` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44176163480)亦已核验：490/490 CTest、Make63+25，98个ASan均低于半限（最高32.06%）；其source成功，blob核验明确复用同树PR156原件。后续 main `8018622` 和 `59c568f` 的完整矩阵单独收件，未以PR门禁代替。

E9 PR157 最终 head： [core](https://github.com/wmqfl861/wls861/actions/runs/44177730029) 294/294、[D-40](https://github.com/wmqfl861/wls861/actions/runs/44177804373) 26/26、[source](https://github.com/wmqfl861/wls861/actions/runs/44177729924) 781 blobs/modes 相符；ASan 最高32.13%、protocol_codec5.16s。产品191行、测试301行；有界源码复核通过，生产批量路径仍关闭。

CSV PR156 最终 head：[core](https://github.com/wmqfl861/wls861/actions/runs/44173217114) 294/294、[source](https://github.com/wmqfl861/wls861/actions/runs/44173216920)、[native](https://github.com/wmqfl861/wls861/actions/runs/44173270997)通过；五次 FakeModel，150B 输入拒写且原字节保持，62B 不可变 CSV 与 oracle 相同。函数 PR158 原 head：[core](https://github.com/wmqfl861/wls861/actions/runs/44179943762) 300/300、[source](https://github.com/wmqfl861/wls861/actions/runs/44179943723)、[native](https://github.com/wmqfl861/wls861/actions/runs/44180013843)通过；六次 FakeModel、原 Worker run 运行五个 unittest、两份源码提交。原四沙箱/两环境/CSV 场景均保留；最终整合 `5405d27` 的 [core](https://github.com/wmqfl861/wls861/actions/runs/44184695373) 300/300、[source](https://github.com/wmqfl861/wls861/actions/runs/44184695244) 789 blobs/modes、[native](https://github.com/wmqfl861/wls861/actions/runs/44184891518)全部通过并核验。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：main155 为 `run-intake/hosted-main155-67f52f0/resumed-ci-ozpzb9yw/verification.json`；PR156 为 `run-intake/hosted-pr156-586eaea/verification.json`；PR157 为 `run-intake/hosted-pr157-c9d2adf/verification.json`；PR158 最终输入为 `run-intake/hosted-pr158-5405d27/verification.json`；main156为 `run-intake/hosted-main156-f31647d/verification.json`；取消真实红绿及原执行记录为 `g4d-cancel-evidence/`；AMEND候选本机与检查器原件为 `g4d-amend-implementation-evidence/`，静态诊断仍独立保留。原始失败、完整日志和封存请求均保留；凭据、数据库、构建、本机规则不入 Git。
