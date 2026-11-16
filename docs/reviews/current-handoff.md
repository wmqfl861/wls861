# 最新交接

更新：2026-11-16。实现 0.13.0，Store 410；长期状态见 [STATUS](../STATUS.md)。

现在能做什么：G4-N3 冻结只读 `/input` 与总字节限制已由 [#136](https://github.com/wmqfl861/wls861/pull/136) 合入 `adb89e46`；核心 270/270、D-40 11/11 及扩展 native 四场景通过。R6 `b49bbb1` 完整矩阵 450/450 CTest、Make 60+25 通过，注释标签 [`v0.13.0`](https://github.com/wmqfl861/wls861/tree/v0.13.0) 已推送，无 Release。

卡在哪：N4 [#137](https://github.com/wmqfl861/wls861/pull/137) 已实现冻结环境检查、原 BLOB 版本卡及 Worker 预约前重新 capture，已普通 merge N3，本机 Release 10/10 通过；最终提交的托管门禁未完成。F-15 写入/提交扩容和真实 U3/U4 尚未完成。G2 的 U1/U2 未通过；R2/R3 上游失败、R5 无序号扩展事件拒绝均保留原件，E1 已保守回退。

下一步：root 推送普通 merge 提交，在同一最终输入核对 PR 必需检查、14 套 D-40 和保留 N3 输入扩展的 native 四场景加同请求输入/版本组合环境入口；通过后按授权合 N4。继续 [唯一计划](claude-2026-11-16/04-plan.md) 的 F-15 剩余切片与真实验收；不重放失败 R2/R3/R5，不做第三轮 SSE 修补。

N4 本机合入前 Release 五套通过，ASan 四套通过、worker 180 秒本机超时；不改期限或重复诊断。首次托管核心/D-40 因 mkdir phase 宏签名遗漏而编译失败，已最小修复且该套本机 Release/ASan 通过。历史 `f1150e27` native 四场景和环境两项通过；最终合并提交的结果须另核。合 N3 后 Release 10/10 通过，158.71 秒；选取 ASan 七套，六套通过，event_runtime 180.79 秒本机超时，无 ASan 错误输出。新旧两次超时分开保留，按 D-39 不加期限或重复诊断。

N3/N4 的实际行为、已知限制、失败记录、逐项 CI 链接及最终本机证据集中在 [G4 阶段记录](../reports/g4-run-tool.md)；环境 opt-in 用法见 [Worker tools](../implementation/S1-worker-tools.md#g4-n4显式冻结的环境版本检查)。源码检查、合成测试、托管原生和真实模型验收分别登记；本切片真实调用 0。

R6 的 [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43946016276) 绑定 `b49bbb1`。N3 的 [核心](https://github.com/wmqfl861/wls861/actions/runs/43954620405) synthetic `19d1dfb5` 与 head `9355be19` 同树，[D-40](https://github.com/wmqfl861/wls861/actions/runs/43954617821) 和 [native](https://github.com/wmqfl861/wls861/actions/runs/43954623427) 均精确 head；N3 main 新矩阵单独核对。

G2 原文件冒烟通过，R0 零新调用恢复原三交付物、成果 12/12；R1 确认流式及 low/medium/high，R3 创建 3 任务/3 团队。R5 十次调用后 `RUNNING/needs_accounting`，完整 token/质量改善及费用未知，自动重试 0。实际用量、三个失败请求、E1 回退和 R0 的 SHM mtime 保留检查 FAIL 见 [G2 运行记录](../reports/g2-first-real-run.md)。

私有任务根 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/`：N3 原件 `g4-input-validation/evidence/`，N4 `g4-env-evidence/`，托管原件 `evidence/n3-pr136-ci/`、`evidence/n4-pr137-ci/`；原 G2 输入与运行在 `evidence/real-line/`，文件级索引和摘要 SHA 在相应阶段记录。

失败请求不取消、不重发、不回填 usage。回退后的 parser 不接受 R5 opt-in；保留原 E1 ELF、35 项资源及配置，不用回退程序恢复或重放。R0 原请求与备份至少保留到 2026-12-16，原 metadata FAIL 不改写；原路线在 [归档](../handoff/roadmap-before-v0.5.md)。

任务结束同步代码、测试及本页，推送后核对远端提交、文档及 CI。凭据、DB、构建包、长日志、原始响应和本机规则仅留私有位置。当前无需 owner 操作；后续按已授权范围与有限额度推进。
