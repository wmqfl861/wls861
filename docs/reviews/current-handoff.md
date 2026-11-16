# 最新交接

更新：2026-11-16。

现在能做什么：0.12.0 的四角色文件闭环已完成固定冒烟；R0 恢复原三交付物导出，12/12 成果检查通过且没有新调用。R1 确认流式和 low/medium/high 可用，缓存探针不支持继续缓存优化。A-1/A-2 已按原捕获输入完成，详见 [G2 运行记录](../reports/g2-first-real-run.md)。

卡在哪：#126/#130/#131 修复已合入；其后 R2 与 R3 都因中转上游失败流缺少当前 Dispatcher 验收或交付，U1/U2 未通过。R2 17 次、R3 21 次，R3 建立 3 个任务/团队；失败 usage 保持未知，请求原件封存。长期状态统一见 [STATUS](../STATUS.md)。

下一步：按 A1/A2 只选择成功写入回执发送投影（E-1），先离线测量、回归，再按 R5 做一次新目录的同条件 R2；不继续第三轮 SSE 修补。G4 托管 native 及 nightly 调度已合入，本机保持 BLOCKED，按 owner 决定继续托管验收。后续按[唯一计划](claude-2026-11-16/04-plan.md)推进，v1.0 报告后等 owner 回复。

## 输入与已推送成果

[#121](https://github.com/wmqfl861/wls861/pull/121) 至 [#124](https://github.com/wmqfl861/wls861/pull/124) 已合入 main：绑定来源、审核资料、导出重放提示、三团队固定任务。main `2cf036f` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43878414585)成功。R2 首次启动固定在 main `9aa9254`；修复后的实跑为 `bacf68bd`。配置修复 [#126](https://github.com/wmqfl861/wls861/pull/126) 与沙箱传播修复 [#127](https://github.com/wmqfl861/wls861/pull/127) 也已合入；[托管 native](https://github.com/wmqfl861/wls861/actions/runs/43895693147)实际四种操作通过，手动 ASan 与各 PR 检查通过。旧失败不改报。Responses [#130](https://github.com/wmqfl861/wls861/pull/130) 已合入，新运行冻结 main `322c173`；其 PR 核心、源码检查和 [D-40](https://github.com/wmqfl861/wls861/actions/runs/43909409408) 6/6 通过，本机 codec/SSE ASan 2/2 通过。第二轮 [#131](https://github.com/wmqfl861/wls861/pull/131) 已合入 `55bf304`；strict、umask-0002、Clang-18、源码和 [D-40](https://github.com/wmqfl861/wls861/actions/runs/43916415645) 6/6 全通过，本机 Release/ASan SSE 各 42/42，先后两份原始 SSE 离线全程解析通过。main `322c173` 完整矩阵 10/10 成功；`55bf304` [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43919634047)10/10 成功。

真实调用沿用获准 Responses 中转与 `gpt-6.1-sol`，自动重试 0。G2 本轮累计 48 次，已知 350,071 输入/13,505 输出，另 4 次未知输入预留共 283,558；阶段保守占用 633,629/500 万、48/250 次。历史 41 次另列并计入 through-v1；费用未知，凭据只在指定进程内存注入。

## 证据与保留

逐次结果、R0 metadata 检查 FAIL、R1 探针、A-1/A-2 和 R2 各次失败集中在 [G2 运行记录](../reports/g2-first-real-run.md)，不在本页复制细节。完整安全契约见 [AGENTS](../../AGENTS.md) 与[开发契约](../implementation/development-contract.md)。

本机原实跑根为 `/mnt/d/codex-workspace/wls861-g2-20261115-a78u4b6i/`；本轮根为 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/`。`evidence/real-line/` 保存 R0 备份及 R1/R2/A1/A2；`evidence/g4-n2/` 保存失败与通过的原生原件及 `owner-local/`；`evidence/main-5452-ci/` 保存前一产品输入完整 CI 原日志。原请求与 R0 备份至少保留到 2026-12-16。

历史诊断索引原件为 `/mnt/d/codex-workspace/wls861-g6-history-20261116-5ydkd7fq/evidence/asan-stack-followup/followup-closeout.json`，处置状态仅在 STATUS 维护。

每次任务结束同步全部应提交代码、测试和本页交接，正常推送并核对 GitHub。私有数据库、构建、凭据、长日志、原始模型响应和本机规则不进仓库；原失败保留。
