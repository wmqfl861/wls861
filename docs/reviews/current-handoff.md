# 最新交接

更新：2026-11-17。当前 main 为 [2213c27](https://github.com/wmqfl861/wls861/commit/2213c27fd673c3a4c298ba713324da12d1513228)，[#154](https://github.com/wmqfl861/wls861/pull/154) 已合入；实现 0.13.1、Store 410。长期边界只在 [STATUS](../STATUS.md) 维护。

现在能做什么：G2 U1/U2 已真实交付；G3-B1 十格已封存，结果与费用未知见 [G3 报告](../reports/g3-comparison.md)。main 的固定初始计划 opt-in 在相同合成文件任务中实测总调用 7→6，Worker 写入与提交保留。

在制：[#155](https://github.com/wmqfl861/wls861/pull/155) 冻结首次 TeamLead 工单，合成总调用 7→5、固定开销 6→4；产品 114 行，C1+C2 共 208/250，本机定向检查和独立源码复核通过，核心 CI 与 26 组受影响 ASan 待终态核验。该能力尚未合入本分支基线。

并行候选：`test/g4d-csv-workflow-20261117` 准备 CSV 的原 CLI→Worker run→不可变提交路径，准确候选本机解析/oracle 与预启动拒绝检查通过；托管正向 native 待执行。范围、原始二进制身份更正及剩余两场景详见 [G4 记录](../reports/g4-run-tool.md#g4-d-csv-准备候选2026-11-17)。

下一步：各候选按自己的实际 CI/native/D-40 结果收口；正常合入后删除已备份的清洁分支和工作树。G3 随后推进 E9 两个顺序 PR、新冻结策略和 B2；旧 v4 不认证宿主初始化或新重试，B2 按原始 oracle、工作流、用量和适用性分列，G3 尚未出口。

## 验证与证据

main `de7f5b5` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44152280898)与[源码](https://github.com/wmqfl861/wls861/actions/runs/44152281143)已核验：475/475 CTest（核心 285、ASan 95、UBSan 95），Make 61+25 条命令及 26 个 worker 用例通过；全部 95 个 ASan 套件低于各自半时限。它是早于当前 main 的独立输入。

PR154 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44153210046) 285/285、[源码](https://github.com/wmqfl861/wls861/actions/runs/44153209908)和[受影响 ASan](https://github.com/wmqfl861/wls861/actions/runs/44153239418) 25/25 已核验通过；当前 main 2213c27 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44160566010)仍待终态核验，不能用前述结果替代。

CSV 准确候选 `0ea6ca9` 在新目录编译；2/2 CTest 通过（0.23/1.22 秒）。首次旧 B1 ELF 检查单独保留并更正，不计作候选结果。未运行本机 native、未改变主机配置；托管正向结果不提前登记。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：主线核验 `run-intake/hosted-main152-de7f5b5/resumed-ci-l4fgaoj7/verification.json`，PR154 核验 `run-intake/hosted-pr154-bc3ae07/verification.json`，PR155 本机与复核 `g3-c2-evidence/`、`g3-c2-review.json`，CSV 为 `g4d-csv-evidence/`。原始失败和完整日志保留；数据库、凭据、构建和本机规则不入 Git。
