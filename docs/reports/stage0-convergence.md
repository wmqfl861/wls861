# 阶段 0 收敛里程碑（进行中）

更新：2026-11-11（UTC）。本页是 [05 开发计划](../reviews/claude-2026-11-11/05-development-plan.md)与本路线点名的阶段 0 一页里程碑；当前状态 **进行中，未达出口**。阶段真正结束时回填本页，不另建新报告。

## 已发生的事实

- main 产品组合 [`ef71c09c6bf88702aaf042f8c26c9bdad6b1ee6a`](https://github.com/wmqfl861/wls861/commit/ef71c09c6bf88702aaf042f8c26c9bdad6b1ee6a)（2026-11-11）含 [#48](https://github.com/wmqfl861/wls861/pull/48) G1-2 Responses 协议兼容（普通合并 `f29d5bd`）与 [#49](https://github.com/wmqfl861/wls861/pull/49) G1-1 冻结 token 单价估算/原用量保留（普通合并 `ef71c09`）；#46、#47 已于 2026-11-08 合入。已合并能力和原范围证据见 [STATUS](../STATUS.md)、[G1-2 验证报告](g1-2-responses-compat-validation.md)与[计费验证报告](g1-billing-validation.md)。
- #49 在原分支普通 merge main 代替变基，无强推；精确 PR head `ed243d49484dbf73b526c1e9c3ac65dfc6caf512` 的核心 run [43002694200](https://github.com/wmqfl861/wls861/actions/runs/43002694200) 与 source run [43002694238](https://github.com/wmqfl861/wls861/actions/runs/43002694238) 全部 5 项检查成功。既有 `billing_baseline_e2e` 31 次调用是本机合成 TLS 验证，两次 pump 由测试显式发起，不是真实供应商/模型验收或付费调用，也不完成 F-08 产品自动续批。
- 组合 main `ef71c09` 的 source run [43004295747](https://github.com/wmqfl861/wls861/actions/runs/43004295747) 成功；完整六配置 run [43004296055](https://github.com/wmqfl861/wls861/actions/runs/43004296055) 查询时仍为 in_progress，不计为通过。此前 #48 后 main `f29d5bd` 的六配置 run [42996980600](https://github.com/wmqfl861/wls861/actions/runs/42996980600) 已完成成功，仅覆盖该旧产品组合。
- [#50](https://github.com/wmqfl861/wls861/pull/50) 已按 owner 决定关闭（由采纳路线的 F-07 最小联网切片替代），分支 `feat/g1-network-resolver-20261109` 保留。
- F-26 文档对齐以 Draft PR [#51](https://github.com/wmqfl861/wls861/pull/51) 推进（分支 `docs/f26-handoff-20261111-h6ose7wz`，普通 merge 更新后的 main）：更新原定 12 份文档。7 份审核/计划文件来自冻结提交 `7d1dadb`，除 05 的 §2 状态列和追加 §16 外保持原字节；03 仍是未批准提案，蓝图 v0.4.1、Store 410 不变。
- 本轮本地文档核验：`git diff --check`、`python3 scripts/verify_handoff.py` 与 `python3 scripts/verify_review_source.py` 均 exit 0；六份原附件逐字节一致，05 原正文仅阶段 0 状态列变化并追加 §16；既定文档的相对 Markdown 链接全部可解析。这些是文档与归档核验，不是产品测试或真实模型证据。
- Owner 决定（D-1…D-8 及 #50/#49 处置）全文见 [05 §16 执行附录](../reviews/claude-2026-11-11/05-development-plan.md)。要点：D-2/D-3 未给出，保持零真实/付费调用；D-4 元数据未知键忽略并记录、未知输出项类型拒绝；D-6 取消配对复现、独立硬审、资源采样、源码身份清单等合并前置；D-7 清理推迟至 G9；D-8 顺序为阶段 0 → G1 → G2 → G4 → G3 → G5 → G6 → G7 → G8 → G9。

## 未完成（不计为已完成）

- 阶段 0 出口逐项核对：main 含 G1-1 与 G1-2 —— 已满足；最终产品组合的 main 六配置通过 —— 待 run 43004296055 终态，不预写通过；#50 关闭保分支 —— 已满足；在制 ≤3 且开放 PR 均直接基于 main、无堆叠 —— 当前满足（仅 #51 一个）；文档与报告据实完成 —— 本 PR 尚未合并，进行中。
- G1 各项（自 G1-A 测试分层起）及 G2 以后各阶段均未开始；真实模型闭环仍未执行。

## 边界

- 本页事实来源：root 转达的 owner 决定与已查询的 GitHub PR/CI 状态；报告绑定上述具体 head 与 run，旧提交成功不代替当前组合验证，运行中不写通过。F-26 自身 CI 与合并事件独立登记；阶段 0 最终回填仍用本页。
- 本 F-26 PR 为纯文档变更；本地只运行文档核验，未新增测试或工具、未编译或运行 WLS 产品。PR CI 按既有分类另行运行，不把尚未完成的 run 写为通过；真实与付费模型调用均未发起。
