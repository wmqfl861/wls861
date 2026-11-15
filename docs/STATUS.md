现在能做什么：0.12.0 已通过 G2-P，E-2 状态段半减已完成；真实 Responses 协议探针成功，第二次 WLS 冒烟固定三行文件检查通过；Responses/Chat 合成交付均可用。
还不能做什么：G2-4 首次运行超时、第二次成果发布阻断，完整验收未通过；G2-5 NOT_RUN，尚无三条件质量结论；G4 原生验收 BLOCKED，Windows 11 NOT_RUN。
需要 owner 做什么：测试密钥与 observe-only 授权已到位，无需重复提供或补价格表；因文件 UNKNOWN 已暂停后续真实调用，费用保持未知。

# 当前状态

更新：2026-11-15。实现 **0.12.0**，当前蓝图 **v0.4.1**、已采纳目标蓝图 **v0.5**，Store **410**。没有数据库版本变更。

owner 指定 Responses 中转、`gpt-6.1-sol` / `max` 并取消金额门禁。新请求显式采用 `observe-only-v1`；旧冻结请求保留原门禁。凭据与配置留在外部，不授权公网上线或换端点兜底。实跑表、用量、审计及验证证据集中在[首次真实运行记录](reports/g2-first-real-run.md)。

E-8 #116 合并后的 main `2327836` [完整 CI](https://github.com/wmqfl861/wls861/actions/runs/43766234662)通过：450/450 CTest、Make 60+25 全部 PASS，失败/跳过/未运行均为 0；[源码验证](https://github.com/wmqfl861/wls861/actions/runs/43766234740)确认 744 文件一致。完整 CI 未提供 JUnit/LastTest artifacts，原日志已保留；旧轮次失败见[已有运行记录](reports/g2-first-real-run.md)。本机 doctor 布局失败保留，受支持布局复验通过；历史 ASan `event_runtime` 问题仍 OPEN。

## 当前阻塞

G2-4 保留 Store `DELIVERED` / `output_blocked`、Root `unknown_outcome` 的原状态。最终包审计及私有副本的固定成果检查通过，原 export 仍未确认，三 team 分工也未验证。[发布修复 #111](https://github.com/wmqfl861/wls861/pull/111) 已合并到 main `9b7f700`，本机回归、独立静态审核、PR 检查及合并后完整矩阵均已通过；原真实请求未重试。

## 已有能力与限制

[E-2 #113](https://github.com/wmqfl861/wls861/pull/113) 已完成三个角色的状态项减半，两个旧版本的 wire 兼容验证通过；测量与回归见[同一运行记录](reports/g2-first-real-run.md)。真实 G2 验收结论不变。

G1/G2-P、N1/N2/N3 已完成；[八场景预演](reports/g2p-rehearsal.md)只证明合成流程。四角色工作流保留版本、授权、首次派发、UNKNOWN、原字节提交/验收/交付守卫。普通单 Agent 可提交不可变成果，三条件评测与真实业务验收仍未完成。

G4-A1/B 已合并，standard 显式选用、Strict 为默认；原生验收仍 BLOCKED，`/input`、环境卡片实际执行检查、无 cgroup 启动与全树回收仍待后续条件；Windows 11 NOT_RUN。[收敛报告](reports/convergence-20261114.md)保留此前失败。

G6 已完成 Store 返工、capacity 子域拆分与可选 ccache，具体构建切片范围和测量见 [CHANGELOG](../CHANGELOG.md)，整体仍只完成部分工作。daemon/API、私有行清理 runner、完整记忆和独立审核闭环仍未实现。六个新格式在 G2 出口冻结；旧请求回归保持。D-12 取消私有证据托管要求，原始证据至少保留 30 天。

## 下一步

保留原 UNKNOWN 与原件；原真实请求未重跑、export 未重试。新的真实复验和 G2-5 角色对照尚未恢复，当前 G2-5 NOT_RUN。单任务冒烟 PASS 的范围不扩大；G7 及以后功能阶段暂不开始。

历史实现与逐次失败保留在 [CHANGELOG](../CHANGELOG.md)、[G1 交接](reviews/g1-main-handoff-20261112.md)、[G2-P 报告](reports/g2p-rehearsal.md)和[全代码审核](reviews/code-audit-20261027.md)；本页只记录现状与入口。
