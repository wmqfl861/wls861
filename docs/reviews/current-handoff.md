# 最新交接

更新：2026-11-17。实现 0.13.0，Store 410；长期状态见 [STATUS](../STATUS.md)。本轮授权见 [D-46 至 D-57](claude-2026-11-17/03-decisions.md)，唯一前向计划已应用 [11-17 修订](claude-2026-11-17/04-plan-revisions.md)。

现在能做什么：D-46 观察模式账目就绪 [#140](https://github.com/wmqfl861/wls861/pull/140) 和 D-48 有界重试 [#141](https://github.com/wmqfl861/wls861/pull/141) 已合入，当前产品 main `93b8968`。失败调用的原始 null 用量保持，合格 ERROR 以原预留参与就绪；新请求按冻结 runtime 上限最多重试两次，每次另行预约。版本仍为 0.13.0，尚未发布 0.13.1。

实际验证：两个 PR 各有核心 270/270、源码检查及独立审核 PASS，D-40 分别 15/15、16/16。D-46 合入 main 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43998851346)450/450 CTest 与 Make 两 shard 通过；D-48 合入 main 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44002919509)最终结果待独立核对。本机合成范围及 CI 输入身份集中在[运行记录](../reports/g2-first-real-run.md#r7r10账目恢复重试与真实复验)。

真实进度：R8 完整备份后，用当前 main 程序和原 argv/config 路径运行一次，D-46 账目已就绪，但原冻结期限已过，停于 `wall_clock_exceeded` 后封存，新预约/发送均 0。R9 首次和一次原命令续跑、R10 首次共三次失败已核实为响应媒体类型不匹配：冻结及实际发送 `stream=false`，HTTP 200 却声明 `Content-Type: text/event-stream`，头部检查提前拒绝并报 `invalid_http_header`。响应体捕获为 0，无法判断模型完成、终态或实际用量；账目均 ready，自动重试均 0，无交付，U1/U2 为 NOT_PASSED。

下一步按修订后的 [计划 §3](claude-2026-11-16/04-plan.md#3-g2-余下的步骤) 顺序执行：

1. 响应媒体类型的窄范围修正正在制定计划，尚未交付；完成实施与独立核对后再按授权复验。现有新请求为非流式、180 秒，只有 `system.model_calls.max_paid_retries: 2` 控制运行重试，provider codec 保持零重试；超时不重试。R2-stream-check、R3-stream-check、R5 均已封存。
2. R11 的后续顺序仍是 E-14、E-11、E-1，分别给出离线字节与一次完整真实比较，再按 D-57 办理 0.13.1。当前优化复验、版本出口和 G3 均未完成。
3. G2 出口后转 G3。G4 的真实 U3/U4 等合规宿主，之前不出口、不升 0.14.0；D-50 SSE 扩展事件留到 R12 之后或 G3 期间。

长期限制、宿主状态及 owner 的既有决定集中在 [STATUS](../STATUS.md)。

N3/N4 的实际行为、CI 链接、失败和超时原件集中在 [G4 阶段记录](../reports/g4-run-tool.md)；环境 opt-in 用法见 [Worker tools](../implementation/S1-worker-tools.md#g4-n4显式冻结的环境版本检查)。源码检查、合成测试、托管 native 和真实模型验收分别登记。

实际用量、未知 usage、R0 原文件以及各次失败原因只在 [G2 运行记录](../reports/g2-first-real-run.md)详记。D-31 的原导出重放已完成，不再重复；未知用量不回填，预留不当作实际 token。

本轮私有任务根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：D-46/D-48 本机证据在 `d46-evidence/`、`d48-evidence/`，托管原件及审核在 `run-intake/`，R8 在 `r8-execution/`，新请求在 `real-prep-3gueo4lx/`。旧 N3/N4 与实跑证据仍在各阶段报告的原位置；凭据、DB、构建包、长日志、原始响应和本机规则不入库。
