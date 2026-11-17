# 最新交接

更新：2026-11-17。实现 0.13.0，Store 410。长期状态见 [STATUS](../STATUS.md)，唯一前向计划是 [11-16 计划](claude-2026-11-16/04-plan.md)，已应用 [11-17 修订](claude-2026-11-17/04-plan-revisions.md)。

现在能做什么：D-46 [#140](https://github.com/wmqfl861/wls861/pull/140)、D-48 [#141](https://github.com/wmqfl861/wls861/pull/141) 与媒体诊断 [#142](https://github.com/wmqfl861/wls861/pull/142) 已合入。当前已合入输入是 main `4b10ee1`；原未知用量不回填，重试另行预约、发送和结算。#142 的核心、源码和受影响 ASan 均通过。

当前阻塞：U1/U2 尚未达成。#142 后的一次新 R9 捕获了完整 JSON 响应，但 HTTP 头声明 `text/event-stream`；按旧策略拒绝，无交付。原 profile、原请求、原 body 的产品离线 JSON 回放通过，证明可用 JSON 被标错类型；回放没有网络、Store 或工具效果。本机六套定向回归（220 个报告用例）已通过，正在完成托管门禁的新请求选项为显式冻结的 `responses_media_type_policy: json-under-sse-v1`；缺键的旧策略不变，不把真正的 SSE 当作非流式 JSON。

真实调用额度与每次结果只在 [G2 运行记录](../reports/g2-first-real-run.md#r7r10账目恢复重试与真实复验)详记。到这次诊断请求结束，G2 为 64/250 次、986,531/5,000,000 输入保守占用；原 null 用量及失败目录保持，费用未知。密钥已有授权，只向目标运行进程临时提供，不写入文件。

下一步：

1. 完成显式媒体兼容选项的 PR 核心和受影响 ASan；正常合入后，用新配置、新请求继续非流式 U1，再 U2。原 R2/R3/R5、前次 R9/R10 和媒体诊断请求不改写。
2. U1/U2 后依次 R11 E-14、E-11、E-1，完成离线字节与真实 token 比较，再办理 R12 `v0.13.1`。这些步骤尚未执行，不宣称 G2 验收通过。
3. G2 出口后做 G3；G4 的 U3/U4 等合规宿主，不出口、不升 0.14.0。D-50 留到 R12 后或 G3。

CI、局部合成和真实运行分别登记；#142 main 完整矩阵正在核对。本机宿主和 Windows 未运行项集中在 [STATUS](../STATUS.md)，原 G4/N3/N4 证据见 [阶段记录](../reports/g4-run-tool.md)。

私有根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：`run-intake/` 保存 CI 原日志与核验，`media-real-prep-lcylnhl4/r9-media-check/execution/` 保存最新真实失败，`media-json-offline-ig_ga46p/` 保存精确原文离线回放，`json-media-real-prep-4gybr21u/` 为尚未执行的新请求准备。凭据、数据库、原始模型响应、构建和长日志不入库。
