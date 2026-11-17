现在能做什么：U1、U2 已真实交付，各有唯一确认回执和 12/12 成果检查；G3-B1 十格基线已完成，结果有通过、缺失和失败。
当前任务：G3-C 最小固定单任务切片正在实施，G3 尚未达出口；接手看最新交接。
需要 owner 做什么：当前继续已有授权任务；到合规宿主依赖或额度边界时按既定规则处理。

# 当前状态

更新：2026-11-17。实现 **0.13.1**，蓝图 **[v0.5](spec/v0.5/01_实施规格_v0.5.md)**，Store **410**；没有迁移 SQL 或已有请求。[v0.13.1 标签](https://github.com/wmqfl861/wls861/tree/v0.13.1) 已推送。

**接手入口：[最新交接](reviews/current-handoff.md)**。唯一前向计划为 [11-16 计划](reviews/claude-2026-11-16/04-plan.md)，已应用 11-17 修订。G2 实际结果只在 [G2 运行记录](reports/g2-first-real-run.md#r12出口与r11真实比较)详记；G3 十格、用量、费用未知和比较限制集中在 [B1 对照报告](reports/g3-comparison.md)；版本历史在 [CHANGELOG](../CHANGELOG.md)。

## 已完成与验证

U1、U2 均在 #143 合入后完成。E-14/E-11/E-1 已合入，R11 完整组合对照输入 token 减少 46.32%；首次未完成对照和两次全部用量保留，组合结果不分别归因，旧请求仍按冻结取值解释。R12 已由 #149 合入并完成标签。

D-50 [#150](https://github.com/wmqfl861/wls861/pull/150) 和 G3-B0 [#151](https://github.com/wmqfl861/wls861/pull/151) 已合入；B1 完成不代表同等验收性能比较或 G3 出口。当前 main 完整矩阵与 #152 待核项见[交接验证](reviews/current-handoff.md#验证与证据)，不把 CI 通过写成真实场景通过。

R6 原未达记录保留，原导出 UNKNOWN 在 R0 恢复，但 SHM-mtime 保留检查的 FAIL 不改写。六格式 `structured-v3`、`lean-v1`、`fixed-role-v1`、`append-fold-v1`、`guided-v1`、`bounded-v1` 保持冻结。

## 长期边界

G4 真实 U3/U4 仍待合规宿主，Windows 11 **NOT_RUN**。Strict 默认、Standard 显式选用；绑定、挂载传播、只读输入与环境检查已合入，托管原生检查通过，详见 [G4 记录](reports/g4-run-tool.md)。首次 scheduled native 尚未登记为已执行，托管结果不能替代真实验收。

本机 **BLOCKED**：缺受信任 bwrap 与专用 cgroup 配置，普通用户 uid_map 被拒绝；owner 已选择保持，agent 不改主机配置。未配置不等于已证实无 delegation。F-15 写入/提交扩容候选被既有捕获容量守卫拒绝并归档；G4 不出口、不升 0.14.0。

依赖/协同/并行、完整记忆、独立审核闭环、私有行清理 runner、daemon/API 尚未完成。G6 的 Store 编译切片和可选 ccache 保留；D-39 历史本机 event_runtime ASan 超时已关闭为“本机现象，托管 CI 未复现”，原诊断保留。

后续按 G3-C → E-9 → B2 推进；F-29 原 6→4 目标未达，不能提前宣称 0.15.0。每次任务结束正常同步应提交内容与最新公开交接；凭据、数据库、构建、长日志、私有证据和本机代理规则不入库。
