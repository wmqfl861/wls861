# 最新交接

更新：2026-11-16。

现在能做什么：0.12.0 已具备四角色文件任务闭环、Responses/Chat 合成交付、E-2 状态瘦身和 Store 五个子域独立编译；真实固定文件冒烟已经交付。owner 已采纳 [11-16 审核 D-30 至 D-45](claude-2026-11-16/02-decisions.md)，真实运行线按新停止规则恢复，G4 修复可以并行。

卡在哪：本轮 U1/U2 尚待 R2/R3 实跑。原三交付物请求经 R0 已完成导出，成果 12/12 通过，调用数仍为 29；严格 metadata 保留检查为 FAIL，差异及保留处置见 [G2 运行记录](../reports/g2-first-real-run.md)。该结果不替代新请求的 U1/U2 复验。长期状态统一在 [STATUS](../STATUS.md)。

下一步：按顺序做 R1（最多 8 次流式、思考等级和缓存探针）、A-1/A-2（旧捕获请求分析）、R2 与 R3；同时按 D-38 修复沙箱绑定来源并在托管环境验收。后续唯一计划是 [04-plan.md](claude-2026-11-16/04-plan.md)，G2-7 候选等数据表出来后再选。

## 本轮输入与执行边界

审核基线为 main `8d44256`；最近产品改动是 [#119](https://github.com/wmqfl861/wls861/pull/119) 合入的 `eed61cac17731391f225a171e95eff1e9aaf780c`。实现 **0.12.0**，Store **410**，当前蓝图 v0.4.1、已采纳目标 v0.5。本次文档接纳不扩大已有产品验证范围。

真实调用沿用 owner 指定 Responses 中转与 `gpt-6.1-sol`；D-35 授权探针确认后的分层思考等级，R2 仍全用 max。G2 余下额度 250 次/500 万输入 token，11-15 的 41 次单列历史；单次期限 180 秒，自动重试 0。费用未知，原始缺失用量不改为 0。密钥只在指定进程内存注入。

## 已有验证与证据

- main `eed61cac` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43805419520)：450 CTest + 85 条 Make-ASan 命令通过，失败/超时/跳过/未运行均为 0；[源码检查](https://github.com/wmqfl861/wls861/actions/runs/43805419511)核对 747 文件、69 目录。远端 JUnit/LastTest 未取得，原 job 日志留本机。
- 本机 #119 Release：90/90 CTest；普通 Make 10 条命令、139 个方法通过。PR merge ref `36b6ab6d` 的[三核心检查](https://github.com/wmqfl861/wls861/actions/runs/43795974365) 270 CTest 通过。各输入的证据范围保持独立。
- 真实逐次结果、原失败、离线状态瘦身及 R0–R3 后续结果集中在 [G2 运行记录](../reports/g2-first-real-run.md)。完整安全契约见 [AGENTS](../../AGENTS.md) 与[开发契约](../implementation/development-contract.md)。

本机原实跑证据根为 `/mnt/d/codex-workspace/wls861-g2-20261115-a78u4b6i/`；本轮私有证据为 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/evidence/real-line/`，R0 备份子目录 `r0-backup-v2ypjf_q`。原请求与备份至少保留到 2026-12-16。

历史诊断索引原件保留于 `/mnt/d/codex-workspace/wls861-g6-history-20261116-5ydkd7fq/evidence/asan-stack-followup/followup-closeout.json`，SHA-256 `022a2931a3b31fc8871b7c4a013e0819706d5359bd462933803a6922f1b20644`；处置状态只在 STATUS 维护。

每次任务结束同步全部应提交代码、测试和本页交接，推送后核对远端提交、文档与 CI。纯文档更新不重复全产品测试。数据库、构建二进制、凭据、长日志、原始模型响应和本机代理规则仅留本机；历史报告与私有原件不改写。
