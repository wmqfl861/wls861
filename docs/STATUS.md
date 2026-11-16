现在能做什么：0.12.0 已通过 G2-P，E-2 与 Responses/Chat 合成交付已落地；11-16 审核 D-30 至 D-45 已采纳，G2 真实运行与 G4 修复可以并行。
还不能做什么：完整 G2 验收未通过；R1 探针完成；R2/U1 的 profile 规范化缺陷已修复；新请求首次发送收到完整 SSE，但协议拒绝，1 次用量未知；R3/U2 待复验。原导出 UNKNOWN 已由 R0 恢复；严格 metadata 保留检查仅因 SHM mtime 变化而 FAIL，原检查结果保留。
需要 owner 做什么：当前无需操作；既有密钥授权有效，费用未知保持未知；阶段额度触顶等 D-32/D-34 指定情况才升级处理。

# 当前状态

更新：2026-11-16。实现 **0.12.0**，当前蓝图 **v0.4.1**、已采纳目标 **v0.5**，Store **410**；没有数据库版本变更。

**稳定接手入口：[最新交接](reviews/current-handoff.md)**。后续工作唯一计划是 [11-16 计划](reviews/claude-2026-11-16/04-plan.md)；真实 G2 逐次事实保留在[运行记录](reports/g2-first-real-run.md)。长期状态仅在本页维护。

最近合入 [#121](https://github.com/wmqfl861/wls861/pull/121) 沙箱绑定来源、[#122](https://github.com/wmqfl861/wls861/pull/122) 审核资料、[#123](https://github.com/wmqfl861/wls861/pull/123) 导出提示和 [#124](https://github.com/wmqfl861/wls861/pull/124) 三团队夹具。main `2cf036f` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43878414585)已成功；各 PR 的适用检查均通过。#121 另有分支 [asan-affected](https://github.com/wmqfl861/wls861/actions/runs/43864212503) 通过。

## 当前边界

D-31 允许先备份、再以当前 main 和原命令恢复原导出；D-32 只停止受影响请求或工作线，不再以该请求的 UNKNOWN 暂停全部真实调用。四角色的版本、授权、首次派发和原字节交付守卫保留。

G4 保持 Strict 默认、standard 显式选用。[#127](https://github.com/wmqfl861/wls861/pull/127) 修复子进程挂载传播后，[托管原生运行](https://github.com/wmqfl861/wls861/actions/runs/43895693147)四种操作通过，包含输出、挂载/命名空间和完整回收；Standard 保持 `strong_isolation=false`。每夜调度在 [#128](https://github.com/wmqfl861/wls861/pull/128)。本机仍 BLOCKED：缺受信任 bwrap 和专用 cgroup 配置，普通用户实测 `uid_map: Operation not permitted`；未改主机设置，未将未配置推断为无 delegation；owner 已决定继续托管验收，本机保持 BLOCKED。Windows 11 NOT_RUN。完整记忆、独立审核闭环、私有行清理 runner、daemon/API 未完成，后续按 11-16 计划的阶段依赖推进。

## G6 与下一步

[#117](https://github.com/wmqfl861/wls861/pull/117)、[#118](https://github.com/wmqfl861/wls861/pull/118)、[#119](https://github.com/wmqfl861/wls861/pull/119) 完成 cleanup/live/history 构建切片；可选 ccache 及此前拆分保留。单组本机测量不证明普遍提速，G6 仍仅部分完成，详细测量见 [CHANGELOG](../CHANGELOG.md)。

按 D-39，历史本机 ASan `event_runtime` 180 秒超时项已关闭，标为“本机现象，托管 CI 未复现”，不再投入诊断。原始超时与派生诊断原件保留，未改报为同一次门禁通过；证据索引见[最新交接](reviews/current-handoff.md)。

每次任务结束同步全部应提交代码与最新可公开交接，推送后核对远端提交、文档和 CI；私有原始证据与本机规则留本机，历史记录不改写。维护要求集中在[最新交接](reviews/current-handoff.md)。
