# 最新交接

更新：2026-11-16。实现 0.13.0，Store 410；长期状态统一见 [STATUS](../STATUS.md)。

G4-N4 当前代码增量：显式冻结 `environment_checks: sandbox-run-v1`，经原持久 run 检查版本并在 Worker
预约前重新 capture；旧默认不执行。语义、opt-in 片段和原生入口见 [Worker tools](../implementation/S1-worker-tools.md#g4-n4显式冻结的环境版本检查)。
输入为 main `b49bbb1`；本机 Release 五项已通过，ASan 4/5 通过且 worker 本机 180 秒时限超时（无 ASan 错误输出），托管最终 SHA 的 D-40/native 仍待运行；[阶段记录](../reports/g4-run-tool.md#g4-n4-环境版本检查)区分实际范围，不据此声明 U3/U4。
本次私有原件在数据盘 `wls861-g2-resume-20261116-k48kol3a/g4-env-evidence/`；不提交日志、DB 或二进制。
下一步先合 N3，再普通 merge main 到 N4，解决共享文件后重跑受影响门禁；root 负责推送、CI 和合并。

现在能做什么：原文件冒烟通过；R0 以零新调用恢复原三交付物导出，成果 12/12；R1 确认流式与 low/medium/high，R3 实际创建 3 任务/3 团队。v0.5 规格和六格式范围见 [ROADMAP](../ROADMAP.md)。

卡在哪：U1/U2 未通过，R2/R3 的 Dispatcher 上游失败原件封存；R5 的末次 Worker 在无序号 `codex.response.metadata` 处被拒，未交付且 usage 未知。E1 没有完整真实改善证据，保守回退，不宣称收益或伤害；[详细运行记录](../reports/g2-first-real-run.md)区分三次失败及 R0 的 SHM mtime 保留检查 FAIL。

下一步：核对 R6 适用 CI、远端提交和公开文档，满足出口条件后由 root 创建 `v0.13.0` 注释标签；当前 CI 待核对、标签待创建。后续按[唯一计划](claude-2026-11-16/04-plan.md)推进 G4-N3/N4 和真实 U3/U4；不追加真实 R5，不做第三轮 SSE 修补。

## 当前输入与验证

R5 运行固定 main `3fa7019`，产品树等于 E1 `2bc26482`；[#133](https://github.com/wmqfl861/wls861/pull/133) 的[三核心 270/270](https://github.com/wmqfl861/wls861/actions/runs/43933418378)、[D-40 六项](https://github.com/wmqfl861/wls861/actions/runs/43933414439)和源码检查通过，本机协议 Release/ASan 各 60/60。29 份离线测量和一次失败实跑分别记录。

回退 `ef8598a7` 只反转 E1 五文件，全树等于 `c01df93`，独立审核 PASS；[#134 三核心](https://github.com/wmqfl861/wls861/actions/runs/43938846786)270/270、[D-40](https://github.com/wmqfl861/wls861/actions/runs/43938843600)六项和源码检查通过，已合入 main `6f13998366ecbd319eb844a413abe765b54d9c6a`。该 main 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43942161390)待结果。此前 `c01df93` [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43929349216)450/450 CTest、Make 60+25 成功；不改报为 R6 新提交验证。

R5 10 调用（D/TL/W 1/3/6）后 `RUNNING/needs_accounting`，完整 token/质量改善未知。预算累计、分角色用量、未知预留及实际配置身份集中在运行记录；费用未知，自动重试 0。

## 证据与接手边界

私有根 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/evidence/real-line/`：`r5-e1-validation-handoff.json`、`r5-write-summary-measurement-final/`、`r5-write-summary-prepared/`、`r5-write-summary-execution/`、`r5-last-worker-diagnosis/`、`r5-actual-worker-wire-audit/`、`r5-cumulative-accounting-check.json`、`r5-e1-exact-revert/`；摘要 SHA 在运行记录。

失败 R2/R3/R5 不取消、不重发、不回填原 usage。回退后的 profile parser 不接受 R5 的 opt-in；原 E1 ELF、35 项资源及配置继续保留，不用回退程序恢复或重放该请求。R0 原请求与备份至少保留到 2026-12-16；原 metadata FAIL 不改写。

原路线按原字节归档到 [roadmap-before-v0.5.md](../handoff/roadmap-before-v0.5.md)。历史诊断原件索引仍为 `/mnt/d/codex-workspace/wls861-g6-history-20261116-5ydkd7fq/evidence/asan-stack-followup/followup-closeout.json`。

结束前同步全部代码、测试和本页公开交接，核对远端与 CI；凭据、数据库、构建包、长日志、原始响应和本机规则只留私有位置。
