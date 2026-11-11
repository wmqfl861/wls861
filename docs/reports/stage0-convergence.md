# 阶段 0 收敛里程碑

更新：2026-11-11（UTC）。本页为 [05 开发计划](../reviews/claude-2026-11-11/05-development-plan.md)指定的唯一阶段 0 里程碑。**产品及六配置条件已满足；F-26 [#51](https://github.com/wmqfl861/wls861/pull/51) 按 D-1 合入后阶段 0 完成**，实际合并状态见该 PR，不在合并前预写完成。

- main 产品组合 [`ef71c09c6bf88702aaf042f8c26c9bdad6b1ee6a`](https://github.com/wmqfl861/wls861/commit/ef71c09c6bf88702aaf042f8c26c9bdad6b1ee6a) 含 #48 G1-2 Responses 协议兼容（普通合并 `f29d5bd`）与 #49 G1-1 冻结单价估算/原用量保留（普通合并 `ef71c09`）；#50 已关闭，分支 `feat/g1-network-resolver-20261109` 保留。已合并能力与原证据见 [STATUS](../STATUS.md)、[Responses 验证](g1-2-responses-compat-validation.md)和[计费验证](g1-billing-validation.md)。
- #49 原分支普通 merge main 替代变基，无强推；精确 head `ed243d4` 的核心 run [43002694200](https://github.com/wmqfl861/wls861/actions/runs/43002694200) 与 source run [43002694238](https://github.com/wmqfl861/wls861/actions/runs/43002694238) 全部 5 项检查成功。组合 main `ef71c09` 的 source run [43004295747](https://github.com/wmqfl861/wls861/actions/runs/43004295747) SUCCESS。
- 完整六配置来自同一自动 schedule run [43009034403](https://github.com/wmqfl861/wls861/actions/runs/43009034403)，精确 checkout 均为上述完整 SHA：**11:05:28–11:48:44 UTC，43 分 16 秒，整体 SUCCESS**；没有手动重跑或跨 run 拼接结果。

| 配置 | 实际结果 | 测试耗时 / 整个 job 耗时（秒） |
| --- | --- | --- |
| strict | CTest 78/78 | 312.19 / 533 |
| strict-umask-0002 | CTest 78/78 | 335.22 / 529 |
| Clang-18 | CTest 78/78 | 315.39 / 501 |
| UBSan | CTest 78/78 | 1496.79 / 1722 |
| ASan | CTest 78/78 | 2346.44 / 2514 |
| Make-ASan | 78 个顶层命令全部完成（46 C++ + 32 Python） | 构建与测试 2545 / 2567 |

五个 CTest 配置 failed/skipped/not-run 均为 0。Make-ASan 走原始 Make 入口，保留用户 CPPFLAGS，**不是 CTest 注册项**；数字摘要为 C++ 765、demo 3、Python unittest 393，合计 1161 子例通过，另两个完整性脚本 PASS；无失败、未见跳过，78 命令均完成，CTest not-run 不适用。不推算 CTest 注册项内部子例数。

旧 push run [43004296055](https://github.com/wmqfl861/wls861/actions/runs/43004296055) 仍为 **CANCELLED**：strict/umask/Clang/UBSan 各 78/78、failed/skipped/not-run 均 0；ASan 仅 52 项 PASS，第 53 项 `billing_baseline_e2e` 未完，Make-ASan 在 HTTPS 子例途中于 11:05:43 UTC 被取消，不算失败或全通过。既有 workflow/ref 分组与 `cancel-in-progress: true` 在同 SHA schedule 于 11:05:28 UTC 启动后取消旧 run；annotation 原文为 `Canceling since a higher priority waiting request for C++20 runtime and specification contracts-refs/heads/main exists`。原日志保留，旧 run 不改写为通过。

- F-26 #51 直接基于 main；本轮核对在制仅该 PR，≤3 且无堆叠。文档 head `8c7b705` 的三核心 run [43006279800](https://github.com/wmqfl861/wls861/actions/runs/43006279800) 与 source run [43006279560](https://github.com/wmqfl861/wls861/actions/runs/43006279560) 全 SUCCESS，只覆盖该 head；最新 head、CI 和合并事件以 #51 为准，与上述产品验证独立。#51 按 D-1 合入后阶段 0 完成，再进入 G1；本次未进入 G1。
- 后续文档 head `613ad3c` 的 PR run [43045698130](https://github.com/wmqfl861/wls861/actions/runs/43045698130) 两次 attempt 均 FAIL：各三个矩阵作业在 apt 安装报 `Temporary failure resolving 'azure.archive.ubuntu.com'`、exit 100，未到构建或产品测试（每轮 0 项）；原失败记录保留。修复 [#52](https://github.com/wmqfl861/wls861/pull/52) 仅新增五行 CI 镜像列表选择，保留 Ubuntu 签名/包校验及原 suites；精确 head `b99a6a6` 的核心 [43060702533](https://github.com/wmqfl861/wls861/actions/runs/43060702533) 与 source [43060702172](https://github.com/wmqfl861/wls861/actions/runs/43060702172) 全部五项检查 SUCCESS，于 16:51:00 UTC 普通合并为 `fcc5cfa51262654dc64631565a6dbe8d1b583257`。本分支普通 merge 该 main 后，新文档 head 独立验证三核心/source；#52 不改产品/SQL，`ef71c09` 六配置证据仍绑定原产品组合，不改写旧 run。
- 本 PR 仅原定 12 文档；六附件与冻结 `7d1dadb` 原字节一致，05 仅更新 §2 状态行及追加 §16，03 未批准，蓝图 v0.4.1 / Store 410 不变。本地 `git diff --check`、两个既有归档脚本和相对链接核验通过；这些是文档检查，不是产品测试。owner 决定与覆盖见 [05 §16](../reviews/claude-2026-11-11/05-development-plan.md#16-执行附录2026-11-11-追加非原文)。
- 计费原 31 次调用为本机合成 TLS，测试显式两次 pump，不完成 F-08 自动续批或真实模型验收。本轮未在本地构建/运行 WLS，未调用真实或付费模型；D-2/D-3 未给出，Windows 11 NOT_RUN、私有 CI 原始证据托管 BLOCKED 保持。仅维护本页，不新建报告。
