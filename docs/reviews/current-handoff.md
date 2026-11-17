# 最新交接

更新：2026-11-17。实现 0.13.0，Store 410。长期状态见 [STATUS](../STATUS.md)，唯一前向计划是 [11-16 计划](claude-2026-11-16/04-plan.md)，已应用 [11-17 修订](claude-2026-11-17/04-plan-revisions.md)。

现在能做什么：D-46 [#140](https://github.com/wmqfl861/wls861/pull/140)、D-48 [#141](https://github.com/wmqfl861/wls861/pull/141) 与媒体诊断/兼容 [#142](https://github.com/wmqfl861/wls861/pull/142)、[#143](https://github.com/wmqfl861/wls861/pull/143) 已合入。当前 main `5a96f40`；新请求可冻结 `responses_media_type_policy: json-under-sse-v1`，只对完整严格 JSON 选择原 HTTP 解码器。旧配置、实际 SSE、原用量及原请求均不改写。

当前状态：修复后的新 U1 正在运行，U1/U2 尚未达成。#143 本机六套、核心 270/270、源码及受影响 ASan 16/16 通过；合入后的 main 程序与受审候选 ELF 一致。测试分片候选将 live_workflow_cli 的原 24 方法分为三组，所有方法内容保持，本机 3/3 通过；托管检查待执行。

真实调用额度与每次结果只在 [G2 运行记录](../reports/g2-first-real-run.md#r7r10账目恢复重试与真实复验)详记。到这次诊断请求结束，G2 为 64/250 次、986,531/5,000,000 输入保守占用；原 null 用量及失败目录保持，费用未知。密钥已有授权，只向目标运行进程临时提供，不写入文件。

下一步：

1. 完成当前新 U1 的交付/成果核验，再做 U2；测试分片并行完成托管检查并正常合入。原 R2/R3/R5、前次 R9/R10 和媒体诊断请求不改写。
2. U1/U2 后依次 R11 E-14、E-11、E-1，完成离线字节与真实 token 比较，再办理 R12 `v0.13.1`。这些步骤尚未执行，不宣称 G2 验收通过。
3. G2 出口后做 G3；G4 的 U3/U4 等合规宿主，不出口、不升 0.14.0。D-50 留到 R12 后或 G3。

CI、局部合成和真实运行分别登记；#142 main 完整矩阵已核对通过，#143 main 完整矩阵正在运行。本机宿主和 Windows 未运行项集中在 [STATUS](../STATUS.md)，原 G4/N3/N4 证据见 [阶段记录](../reports/g4-run-tool.md)。

私有根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：`run-intake/` 保存 CI 原日志与核验，`media-real-prep-lcylnhl4/r9-media-check/execution/` 保存最新真实失败，`media-json-offline-ig_ga46p/` 保存精确原文离线回放，`json-media-real-prep-4gybr21u/` 保存当前新 U1 配置和运行；`d20-shards-evidence/` 保存分片验证。凭据、数据库、原始模型响应、构建和长日志不入库。
