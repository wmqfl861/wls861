# 2026-11-14 收敛报告

清理快照：Step 4–7 完成后、报告 PR 创建前，`main@71de73d`。依据 [D-19 至 D-29](../reviews/claude-2026-11-14/02-decisions.md)；报告自身合并、删分支、最终 main 验证和标记仍待收尾。

| 完成标准 | 该快照的结果 |
| --- | --- |
| 1. 远端只留 main | 是；历史分支清理完成 |
| 2. 无打开的 PR | 是；#95、#96、#97 已合并，#84 已由 #97 替代并关闭 |
| 3. 本地仅 main、一个工作树、无其他克隆 | 是；本地与远端一致、受跟踪文件干净；扫描范围内未发现独立克隆，边界见附表 |
| 4. 最终 main 九作业通过、acceptance 各组 ≤90 秒 | 否；报告合并后的最终提交尚未产生，已有运行见下表 |
| 5. 所有被删非祖先提交有远端归档标签 | 是；137 条记录对应的 110 个归档标签均核对通过 |
| 6. 仓库外保留全部引用保险包 | 是；bundle 验证及归档标签恢复抽查通过 |
| 7. main 含报告且最终提交有收敛标签 | 否；待本报告合并及 `converged-20261114` 标记 |

历史批量删除远端 **97** 条（祖先 50、归档 47）、本地 **139** 条分支，移除 **146** 个实际工作树并清理 **1** 个失效登记；#95、#96、#97 的远端/本地分支和工作树另计各 **3** 个。8 个归档提交保留 19 份源码；原 41 个标签全留，新增 70 个，现有 111 个，未计未来收敛标签。报告自身清理不计入已完成数字。

完整 [238 行处置附表](../reviews/convergence-20261114-branches.md) 保留原引用、完整提交与归档标签。与审核的 96 条远端清单相比，仅新增 `docs/claude-review-20261114`，其余末端完全相同；62 条本地独有分支、保存源码的提交和 2 个 detached 末端均列入附表。

| 完整矩阵 | 九作业 | CMake ASan acceptance 四组（秒） |
| --- | --- | --- |
| [#95 分支 43545587169](https://github.com/wmqfl861/wls861/actions/runs/43545587169) | 通过 | 38.60 / 28.54 / 38.54 / 53.01 |
| [main ef0e460 43549145117](https://github.com/wmqfl861/wls861/actions/runs/43549145117) | 通过 | 37.74 / 35.96 / 37.24 / 60.88 |
| [main 33c96d8 43557663614](https://github.com/wmqfl861/wls861/actions/runs/43557663614) | 通过 | 38.02 / 28.82 / 37.59 / 51.96 |
| [#97 分支 43558843290](https://github.com/wmqfl861/wls861/actions/runs/43558843290) | 通过 | 38.14 / 35.45 / 37.76 / 62.61 |
| [main 71de73d 43562207559](https://github.com/wmqfl861/wls861/actions/runs/43562207559) | 运行中 | 待原始日志核对 |

四组顺序为 `acceptance_runtime`、`_experience`、`_memory`、`_guidance`；原用例、断言和 180 秒 sanitizer 时限保持。#95 按套件体量拆分，关闭旧 acceptance 超时项；#97 全部 ASan 注册最大时限占比 38.79%，无超过一半者。历史失败不改写。

G4-A1/B 已由 [#97](https://github.com/wmqfl861/wls861/pull/97) 合入，两个原末端保留在 `archive/g4/a1-standard-cgroup-20261113` 和 `archive/codex/g4-b-xqrck4ht`。托管 [native 43558975976](https://github.com/wmqfl861/wls861/actions/runs/43558975976) 为 Actions **failure**、程序 **BLOCKED（77）**；3 项前置测试通过，bwrap 读取 `/proc/<pid>/fd/5` 被拒绝，业务命令未放行且子树全部退出。根因未定，本机原生验收仍 BLOCKED。

保险包 `wls861-all-refs.bundle`（31,030,689 bytes，SHA-256 见附表）及外移的 127 个原始证据文件（6,480,676 bytes，哈希已核对）至少保留至 **2026-12-14**。main 已禁止强推和删除，合并后自动删分支已开启。

收尾后以 [converged-20261114](https://github.com/wmqfl861/wls861/tree/converged-20261114) 和该提交的 [main 完整矩阵](https://github.com/wmqfl861/wls861/actions/workflows/ci.yml?query=branch%3Amain) 查询最终结果；本页保留上述时点，不为回写自身再开 PR。N3 随本报告落实；N1、N2 尚未实施，之后等待密钥。
