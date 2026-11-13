# G2-P P3：可纠正失败范围验证

2026-11-13。对应 [P3 修改清单](../reviews/claude-2026-11-13/03-fix-list.md#p3-把可纠正的范围补齐)。模型参数的纯校验/事务拒绝、经过授权的只读失败、具有完整无效果证据的写入拒绝，以及完整 HTTP 200 的输出上限截断，现在返回原始原因并允许有界纠正。管理角色跨 `wls run` 保留上次纠错。`allowed-tools-v1` 使用 `mode: required`，不支持允许列表的端点可显式选 `required-v1`；示例 `schema_retry_max` 为 3。

安全边界：权限/工作区检查与频限先于只读纠错，历史读取的全部 `Quota` 保守保留为停止。新增写入纠错只采用原持久化的 FAILED/无效果证据；UNKNOWN、写权限、预算、取消、期限、存储和不变量错误继续停止。截断响应保持 INTERRUPTED 和 `accepted=0`，不派发部分工具，原用量照常结算；同一语义目标连续两次截断后，跨重启也不再发送，新的 AMEND 目标可继续。评估器从原始 attempt、反馈、传输摘要及哈希验证纠错重试来源，保留 P1a 宿主补全与 P5 路径规范化校验。

普通用户 Ubuntu、数据盘私有临时目录、Release 构建。在 P3 与已合入 P1a/CI 修复的 main 组合上，以下 18 个定向 CTest 套件的最终结果全部通过，共 391 个用例：

| 验证范围 | 套件数 / 用例数 | 结果 |
| --- | --- | --- |
| offline/event/workflow runtime、sandbox、worker_tools、r1_structured、lean、r1_management | 8 / 145 | 通过 |
| protocol_codec、sse_replay（含 profile、required、截断分类） | 2 / 84 | 通过 |
| s14、r1_window、r2_rework、r2_amend CLI | 4 / 41 | 通过 |
| r3_evaluation 完整套件 | 1 / 43 | 通过，121.19 秒 |
| HTTPS、live_workflow、run_cli_e2e 完整套件 | 3 / 78 | 通过；run CLI 332.70 秒 |

随后普通合入 P5a（[#77](https://github.com/wmqfl861/wls861/pull/77)），保留路径规范化参数副本与 P3 纠错边界。组合增量构建通过；最终复验 `offline_runtime`、`r1_structured`、`r1_structured_lean` 共 54 例通过（52.91 秒），三个 live workflow 关键用例通过（19.39 秒），一个 v3 原交付评估用例通过（7.31 秒）。后四例分别核对纠错重试/只读原证据、宿主结算异常不转纠错、未知工具纠正，以及 `/work/` 原响应与规范化入库事实的对应。没有把前一组合的 18 套件宣称为最新组合的全套运行。

原失败均保留：初次 protocol 有 1 项错误的未知字段分类预期；r1_window 有 2 项旧的立即停批预期；s14 有缺文件分类预期失败且达到 90 秒时限；r3 在完成 42 项后达到 180 秒时限。修正对应预期并串行复验，保持原 Release 时限，s14 为 65.30 秒、r1_window 为 59.59 秒。新 workflow 回归还暴露一个空 Completed 的 FakeModel 夹具错误；补上文字使它进入工具选择纠错后，25 项完整通过（44.30 秒）。最终选例脚本因遗漏测试目录导入路径，曾未启动 R3；补齐导入路径后仅补跑该例。原失败与构建交接时的瞬时资源重叠记录均保留，旧日志不改写。

本次 P3 差异：`src/` + `include/` 增删 219 行（上限 250），测试 343 行（小于产品的两倍 438），评估脚本单列 53 行。Store 410、已发布 SQL 不变。所有请求为 FakeModel 或自建 loopback TLS 合成请求；未读取或发送真实密钥，无真实/付费调用。本机未重跑全部 83 项或 sanitizer 全矩阵，交由 PR 核心 CI；Windows 11 仍为 NOT_RUN。
