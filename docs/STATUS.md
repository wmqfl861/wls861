现在能做什么：U1 单团队、U2 三任务三团队已真实交付，各有唯一确认回执和 12/12 成果检查；R11 完成的组合对照输入 token 减少 46.32%。
还不能做什么：G3 及后续阶段尚未完成；G4 真实 U3/U4 等合规宿主，Windows 11 未执行。
需要 owner 做什么：当前继续已有授权任务；本机 BLOCKED 的决定保持。到宿主依赖或额度边界时再按规则处理。

# 当前状态

更新：2026-11-17。实现 **0.13.1**，蓝图 **[v0.5](spec/v0.5/01_实施规格_v0.5.md)**，Store **410**；没有迁移 SQL 或已有请求。

**接手入口：[最新交接](reviews/current-handoff.md)**。唯一前向计划为 [11-16 计划](reviews/claude-2026-11-16/04-plan.md)，已应用 11-17 修订。真实结果、用量及证据只在 [G2 运行记录](reports/g2-first-real-run.md#r12出口与r11真实比较)详记；版本历史在 [CHANGELOG](../CHANGELOG.md)。

## 已完成与验证

U1、U2 均在 #143 合入后的程序上完成。后续 E-14 [#146](https://github.com/wmqfl861/wls861/pull/146)、E-11 [#147](https://github.com/wmqfl861/wls861/pull/147)、E-1 [#148](https://github.com/wmqfl861/wls861/pull/148) 已合入；旧请求仍按原冻结取值解释，优化显式选择。三项离线组合请求体减少 27.57%；真实比较先有一次待答问题，再有一次完整交付，两份原件和全部用量均保留。

#148 本机协议 63/63、配置 24/24；PR 核心 276/276、受影响 ASan 19/19、源码检查通过。合入后的 [main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44060667435) 460/460 CTest、Make 两组通过；全部 92 套 ASan 无超半项，最大 38.17%。R12 本机版本一致性、增量构建、CLI 14/14、[PR #149](https://github.com/wmqfl861/wls861/pull/149) 核心 276/276 和源码检查通过并合入；[v0.13.1](https://github.com/wmqfl861/wls861/tree/v0.13.1) 已推送。[新 main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44070987714) 460/460 CTest、Make 两组通过，92 套 ASan 无超半项。

R6 的原未达结论保留，原导出 UNKNOWN 在 R0 恢复，但 SHM-mtime 保留检查的 FAIL 不改写。六格式 `structured-v3`、`lean-v1`、`fixed-role-v1`、`append-fold-v1`、`guided-v1`、`bounded-v1` 已冻结。R11 的组合改善不分别归因于单项，也不代表整个项目或普遍性能通过。

## 当前边界与下一步

G3 已开始补五任务的共同成果核对，再运行同条件对照、实现快路径和新版多工具应答。现有单 Agent 比较器的未知费用、重试及 AMEND 场景兼容需要先验证，不能用缺失结果补成成功。D-50 SSE 扩展事件可在 G3 期间处理。

G4 Strict 仍默认，Standard 显式选用。绑定、挂载传播、只读输入和环境执行检查已合入，托管原生检查通过，详见 [G4 记录](reports/g4-run-tool.md)。首次 scheduled native 仍未登记为已执行；托管结果不能替代真实 U3/U4。

本机仍 BLOCKED：缺受信任 bwrap 和专用 cgroup 配置，普通用户 uid_map 被拒绝；owner 已选择保持，agent 不改主机配置。未配置不等于已证实无 delegation。F-15 写入/提交扩容候选因现有捕获容量守卫被拒绝并归档，G4 不出口、不升 0.14.0。

依赖/协同/并行、完整记忆、独立审核闭环、私有行清理 runner、daemon/API 尚未完成。G6 的 Store 编译切片和可选 ccache 保留；D-39 的历史本机 event_runtime ASan 超时已关闭为“本机现象，托管 CI 未复现”，原诊断保留。

每次任务结束正常推送全部代码与最新公开交接，并核对远端和 CI。凭据、数据库、构建、长日志、私有证据和本机代理规则不入库。
