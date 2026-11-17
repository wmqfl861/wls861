# 最新交接

更新：2026-11-17。当前 main 为 [67f52f0](https://github.com/wmqfl861/wls861/commit/67f52f018e2b031839503e105adab0b49b23b80b)，[#155](https://github.com/wmqfl861/wls861/pull/155) 已合入；实现 0.13.1、Store 410。长期边界只在 [STATUS](../STATUS.md) 维护。

现在能做什么：G2 U1/U2 已真实交付；G3-B1 十格已封存，结果和费用未知见 [G3 报告](../reports/g3-comparison.md)。冻结初始计划与初始 TeamLead 工单均已合入，同一合成文件任务总调用 7→6→5，Worker 写入、提交及各项审核仍保留。

在制：[#156](https://github.com/wmqfl861/wls861/pull/156) 增加 CSV 原 CLI→Worker run→不可变提交的完整准备切片；旧 head 核心、源码、托管 native 和独立源码复核通过。已正常整合当前 main，最终整合提交的检查待执行，旧 head 绿色不替代新输入。该场景止于 SUBMITTED，检查、业务验收及交付未完成。

并行：E9 第一个小 PR 开发严格的文件批操作历史表示，生产调用路径和配置 opt-in 保持关闭；第二个 PR 才接入可信冻结配置与执行。剩余两个 G4-D 场景已形成小切片计划，等待前置合入。唯一前向计划为 [11-16 §15](claude-2026-11-16/04-plan.md#15-修订记录)。

下一步：收齐 #156 整合输入的核心、源码、native 原件后按条件合并；E9 两 PR 依次验证、合入，再做 B2。B2 的 fixed-single-task 不适用于 partial_amend；旧 v4 不认证新宿主初始化，按真实适用性、原始 oracle、工作流和用量分列，不伪造 v4 PASS。

## 验证与证据

main `2213c27` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44160566010)和[源码](https://github.com/wmqfl861/wls861/actions/runs/44160566058)已核验：475/475 CTest（核心 285、ASan 95、UBSan 95），Make 61+25 条命令及原 26 个 worker 用例通过。全部 95 个 ASan 套件低于各自半时限，最高 acceptance 71.48/180 秒（39.71%）。

PR155 head `dc32c74` 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44166272729) 288/288、[源码](https://github.com/wmqfl861/wls861/actions/runs/44166272804)及[受影响 ASan](https://github.com/wmqfl861/wls861/actions/runs/44166266566) 26/26 均核验通过；全部 ASan 低于半时限，initialization 20.39/180 秒。当前 main `67f52f0` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44172025562)和[源码](https://github.com/wmqfl861/wls861/actions/runs/44172025451)另行收件，不与 PR 结果混同。

CSV 旧 head `2a28951` 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44168598933) 291/291、[源码](https://github.com/wmqfl861/wls861/actions/runs/44168599111)及[native](https://github.com/wmqfl861/wls861/actions/runs/44168595942)通过。native 五次 FakeModel、150B 输入写入以 EROFS 拒绝且原字节不变、62B CSV 不可变提交、原 hash/binding/isolation/cleanup 核验通过，原四个沙箱与两个环境场景保留。新整合输入预期为 98 个 CTest 注册，实际结果待新 CI 核验。

CSV 准确实现 `0ea6ca9` 的全新 Release 构建已通过 2/2 聚焦 CTest（0.23/1.22 秒）。首次误用旧 B1 ELF 的检查单独保留并更正，不计作准确候选结果。没有本机 native 或真实模型调用。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：main2213 为 `run-intake/hosted-main154-2213c27/verification.json`；PR155 为 `run-intake/hosted-pr155-dc32c74/verification.json`；CSV 原 head 为 `run-intake/hosted-pr156-2a28951/`（core-source 与 native 分列），本机为 `g4d-csv-evidence/`。完整原件和失败保留；数据库、凭据、构建及本机规则不入 Git。
