现在能做什么：U1、U2 已真实交付，各有唯一确认回执和 12/12 成果检查；G3-B1 十格已完成，结果有通过、缺失和失败。
当前任务：G3-C1/C2 与 G4-D CSV 准备已合入；E9 历史表示候选验证与 G4-D 函数场景开发并行，G3 尚未达出口。
需要 owner 做什么：当前继续已有授权任务；真实 G4 验收仍等合规宿主。

# 当前状态

更新：2026-11-17。实现 **0.13.1**，蓝图 **[v0.5](spec/v0.5/01_实施规格_v0.5.md)**，Store **410**；没有迁移 SQL 或已有请求。[v0.13.1 标签](https://github.com/wmqfl861/wls861/tree/v0.13.1) 已推送。

**接手入口：[最新交接](reviews/current-handoff.md)**。唯一前向计划为 [11-16 计划](reviews/claude-2026-11-16/04-plan.md)，已应用 11-17 修订。G2 结果在 [G2 运行记录](reports/g2-first-real-run.md#r12出口与r11真实比较)，G3 对照、用量、费用未知和限制在 [G3 报告](reports/g3-comparison.md)，版本历史在 [CHANGELOG](../CHANGELOG.md)。

## 已完成与验证

U1、U2 均在 #143 合入后完成。E-14/E-11/E-1 已合入，R11 完整组合对照输入 token 减少 46.32%；首次未完成对照和两次全部用量保留，组合结果不分别归因，旧请求仍按冻结取值解释。R12 已由 #149 合入并完成标签。

D-50 [#150](https://github.com/wmqfl861/wls861/pull/150)、G3-B0 [#151](https://github.com/wmqfl861/wls861/pull/151)、测试分片 [#152](https://github.com/wmqfl861/wls861/pull/152) 及 G3-C1/C2 [#154](https://github.com/wmqfl861/wls861/pull/154)/[#155](https://github.com/wmqfl861/wls861/pull/155) 已合入。显式冻结初始计划/工单复用原事务及 Worker 确认；同一合成文件任务总调用 7→6→5、固定开销 6→5→4。检查、验收、验收后经验仍独立，原 F-29 合并机制未实施。实际门禁见[交接验证](reviews/current-handoff.md#验证与证据)。

R6 原未达记录保留，原导出 UNKNOWN 在 R0 恢复，但 SHM-mtime 保留检查的 FAIL 不改写。六格式 `structured-v3`、`lean-v1`、`fixed-role-v1`、`append-fold-v1`、`guided-v1`、`bounded-v1` 保持冻结。

## 长期边界

G4 真实 U3/U4 仍待合规宿主，Windows 11 **NOT_RUN**。Strict 默认、Standard 显式选用；绑定、挂载传播、只读输入与环境检查已合入。CSV 准备 [#156](https://github.com/wmqfl861/wls861/pull/156) 的最终输入已通过核心、源码及托管实际 Worker run/不可变提交核验并合入。这里只到 SUBMITTED，不能替代真实验收；函数+测试和中断/AMEND 两项准备尚未完成，详见 [G4 记录](reports/g4-run-tool.md)。

本机 **BLOCKED**：缺受信任 bwrap 与专用 cgroup 配置，普通用户 uid_map 被拒绝；owner 已选择保持，agent 不改主机配置。未配置不等于已证实无 delegation。F-15 写入/提交扩容候选被既有捕获容量守卫拒绝并归档；G4 不出口、不升 0.14.0。

依赖/协同/并行、完整记忆、独立审核闭环、私有行清理 runner、daemon/API 尚未完成。G6 的 Store 编译切片和可选 ccache 保留；D-39 历史本机 event_runtime ASan 超时已关闭为“本机现象，托管 CI 未复现”，原诊断保留。

E9 首切片只增加显式主机门禁的历史表示，生产批执行和新配置值仍关闭；后续按最终门禁 → 第二个执行切片 → B2 推进；B1 不同终态不能作同等验收性能比较，真实 G3 出口及 0.15.0 仍未达。每次任务结束同步应提交内容与最新公开交接；凭据、数据库、构建、长日志、私有证据和本机代理规则不入库。
