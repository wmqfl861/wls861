# 最新交接

更新：2026-11-17。main 已合入 [#157](https://github.com/wmqfl861/wls861/pull/157)，提交 [8018622](https://github.com/wmqfl861/wls861/commit/801862281cc22ebb65e9d4ed395a8dad777a0b49)；实现 0.13.1、Store 410。长期能力及限制只在 [STATUS](../STATUS.md) 维护。

现在能做什么：G2 U1/U2 已真实交付；G3-C1/C2 的显式初始计划、工单使同一合成文件任务总调用 7→6→5。E9 已有严格的双工具历史表示，生产配置和批量派发仍关闭；B1 十格的混合结果、用量和费用未知见 [G3 报告](../reports/g3-comparison.md)。

在制：[#158](https://github.com/wmqfl861/wls861/pull/158) 函数与测试场景原 head 核心、源码、native 全通过，现为解决一处公开报告冲突正常整合最新 main；实现文件保留，整合提交的门禁另行核验。CSV #156 已合入，两项准备均只到 SUBMITTED/check pending，不能当作业务验收或交付。

下一步：取消 token 在 live response 应用链路丢失，已有三文件最小补丁和完整确定性回归草案，动态验证尚未执行；与 E9 顺序文件批次接入同一候选，独立提交，完整预算及断言保留。G4 父 SIGINT→AMEND→原命令恢复场景仍 NOT_PREPARED。E9 合入后再做 B2；fixed-single-task 不适用于 partial_amend，旧 v4 不认证新宿主初始化，不伪造等价验收或性能 PASS。

## 验证与证据

main `67f52f0` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44172025562)及[源码](https://github.com/wmqfl861/wls861/actions/runs/44172025451)已核验：10/10 jobs、480/480 CTest（core288、ASan96、UBSan96）、Make61+25；96个 ASan 套件均低于半时限，最高30.49%。后续 main `f31647d` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44176163480)与当前 main `8018622` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44183281321)单独登记，尚未完成最终原件核验，不与 PR 门禁混同。

E9 PR157 最终 head： [core](https://github.com/wmqfl861/wls861/actions/runs/44177730029) 294/294、[D-40](https://github.com/wmqfl861/wls861/actions/runs/44177804373) 26/26、[source](https://github.com/wmqfl861/wls861/actions/runs/44177729924) 781 blobs/modes 相符；ASan 最高32.13%、protocol_codec5.16s。产品191行、测试301行；有界源码复核通过，生产批量路径仍关闭。

CSV PR156 最终 head：[core](https://github.com/wmqfl861/wls861/actions/runs/44173217114) 294/294、[source](https://github.com/wmqfl861/wls861/actions/runs/44173216920)、[native](https://github.com/wmqfl861/wls861/actions/runs/44173270997)通过；五次 FakeModel，150B 输入拒写且原字节保持，62B 不可变 CSV 与 oracle 相同。函数 PR158 原 head：[core](https://github.com/wmqfl861/wls861/actions/runs/44179943762) 300/300、[source](https://github.com/wmqfl861/wls861/actions/runs/44179943723)、[native](https://github.com/wmqfl861/wls861/actions/runs/44180013843)通过；六次 FakeModel、原 Worker run 运行五个 unittest、两份源码提交。原四沙箱/两环境/CSV 场景均保留；当前整合输入结果待核验。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：main155 为 `run-intake/hosted-main155-67f52f0/resumed-ci-ozpzb9yw/verification.json`；PR156 为 `run-intake/hosted-pr156-586eaea/verification.json`；PR157 为 `run-intake/hosted-pr157-c9d2adf/verification.json`；PR158 原 head 为 `run-intake/hosted-pr158-69c4b9b/verification.json`；取消静态诊断为 `g4d-amend-evidence/`，不是动态 RED。原始失败、完整日志和封存请求均保留；凭据、数据库、构建、本机规则不入 Git。
