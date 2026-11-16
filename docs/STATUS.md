现在能做什么：0.12.0 已通过 G2-P，E-2 与 Responses/Chat 合成交付已落地；G6-B 的 Store cleanup/live/history 三域独立编译已合并。
还不能做什么：完整 G2 验收未通过，原文件发布 UNKNOWN；G2-5 NOT_RUN，G4 原生验收 BLOCKED，Windows 11 NOT_RUN，G6 仍 PARTIAL。
需要 owner 做什么：无需重复提供已授权密钥或补价格表；真实调用因 UNKNOWN 暂停，先保留原请求与证据，费用未知保持未知。

# 当前状态

更新：2026-11-16。实现 **0.12.0**，当前蓝图 **v0.4.1**、已采纳目标 **v0.5**，Store **410**；没有数据库版本变更。

**稳定接手入口：[最新交接](reviews/current-handoff.md)**。能力、验证输入、历史诊断、源码发现、限制、证据与下一步集中维护在该页；真实 G2 逐次事实保留在[运行记录](reports/g2-first-real-run.md)。

最近完整验证的产品基线是 [#119](https://github.com/wmqfl861/wls861/pull/119) 合并后的 main `eed61cac17731391f225a171e95eff1e9aaf780c`。[main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43805419520)通过 **450 CTest + 85 条 Make-ASan 命令**，失败/超时/跳过/未运行均为 0；[源码检查](https://github.com/wmqfl861/wls861/actions/runs/43805419511)核对 747 文件、69 目录。远端 JUnit/LastTest 未取得，原 job 日志留本机；后续文档提交不扩大该验证范围。

## 当前边界

G2 原发布 UNKNOWN 不变：不重跑原请求、不重试 export、不改或删除原数据库/WAL/SHM/包；新真实调用继续暂停。四角色的版本、授权、首次派发和原字节交付守卫保留，固定文件冒烟与私有副本审计不等于完整真实验收。

G4 保持 Strict 默认、standard 显式选用，原生验收 BLOCKED；Windows 11 NOT_RUN。完整记忆、独立审核闭环、私有行清理 runner、daemon/API 未完成，G7 及以后暂不开始。

## G6 与下一步

[#117](https://github.com/wmqfl861/wls861/pull/117)、[#118](https://github.com/wmqfl861/wls861/pull/118)、[#119](https://github.com/wmqfl861/wls861/pull/119) 完成 cleanup/live/history 构建切片；可选 ccache 及此前拆分保留。单组本机测量不证明普遍提速，G6 仍仅部分完成，详细测量见 [CHANGELOG](../CHANGELOG.md)。

历史 ASan 原始 180 秒门禁 **OPEN**。旧输入的第二次派生诊断 16/16 方法通过、88.567 秒，不替代原始超时，也不归入新 main 验证；源码发现及下一步受限核对见[最新交接](reviews/current-handoff.md)。当前未安排新的原生重跑。

每次任务结束同步全部应提交代码与最新可公开交接，推送后核对远端提交、文档和 CI；私有原始证据与本机规则留本机，历史记录不改写。维护要求集中在[最新交接](reviews/current-handoff.md)。
