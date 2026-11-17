# G3 对照报告：B1 真实基线与 G3-C 合成候选

**B1 十格已完成并封存：完整 oracle 为 4 PASS、4 MISSING、2 FAIL，仅两格有原始确认交付。** 每个任务、每种条件各运行一次（n=1）；终态不同，不能据调用数或 token 总量宣称普遍性能改善或同等验收比较，也不构成 v4 PASS、G3 出口或 0.15.0 发布。

基线 [main f9da424](https://github.com/wmqfl861/wls861/commit/f9da424)，实现 0.13.1；[#150](https://github.com/wmqfl861/wls861/pull/150) D-50 与 [#151](https://github.com/wmqfl861/wls861/pull/151) B0 已合入。本轮是原五任务文本加冻结场景补充的新请求，两条件共用输入/约束，不能改称 v4 输入。均使用 `gpt-6.1-sol`、medium、非流式、`observe-only-v1`。

plain = `single_agent_plain`，multi = `multi_agent`。文件字节列只认不可变提交快照；完整 oracle 还检查原件、约束、版本、问题或回执等场景证据，不等同于业务验收。耗时为每格 elapsed，输入/输出为保守占用。

| 任务 | 条件 | 文件字节 | 完整 oracle | 工作流/验收 | 场景 | 调用 | 输入 | 输出 | 秒 |
| --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: |
| tool_roundtrip | plain | 通过 | PASS | 验收待定 | 已触发 | 4 | 21,266 | 383 | 34.046 |
| tool_roundtrip | multi | 通过 | PASS | DELIVERED + 原始回执 | 已触发 | 10 | 81,219 | 5,005 | 178.064 |
| partial_amend | plain | 原件/右通过；新左缺失 | MISSING | AMEND 待处理、无进展 | 已触发，转换未完成 | 6 | 36,873 | 565 | 46.999 |
| partial_amend | multi | 原件/目标通过 | PASS | 预算守卫停止、验收待定 | 已触发 | 19 | 175,755 | 5,903 | 265.341 |
| rejected_rework | plain | 原件/目标缺失 | MISSING | 开放用户问题 | 未触发 | 2 | 9,817 | 289 | 27.709 |
| rejected_rework | multi | 原件/目标缺失 | MISSING | 开放用户问题 | 未触发 | 6 | 45,102 | 2,125 | 94.863 |
| exhausted_rework | plain | 失败/阻塞快照缺失 | FAIL | 开放用户问题 | 未触发 | 2 | 10,189 | 278 | 18.301 |
| exhausted_rework | multi | 失败/阻塞快照缺失 | FAIL | 开放用户问题 | 未触发 | 6 | 46,726 | 2,139 | 84.518 |
| delivery_receipt | plain | 目标通过 | MISSING | 验收待定、无确认回执 | 未触发 | 4 | 21,034 | 333 | 32.373 |
| delivery_receipt | multi | 目标通过 | PASS | DELIVERED + 原始回执 | 已触发 | 8 | 54,077 | 2,459 | 99.113 |

合计 **67 次原始调用、502,058 输入、19,479 输出**，各格耗时相加 881.327 秒（不是整轮墙钟）。plain 合计 18 次 / 99,179 输入 / 1,848 输出；multi 合计 49 次 / 402,879 输入 / 17,631 输出。完整 oracle 的六次非零退出全部保留；两格原始 `CONFIRMED` 回执分别属于 multi roundtrip 和 multi delivery。

每格冻结上限 20 次 / 300,000 输入 / 20,000 输出 / 720 秒，Worker 累计上限 16。系统冻结 runtime retry **2**、provider codec retry **0**，执行授权每批 `max_calls=1` 且 `max_total_calls=1`，没有内部重试余量；本轮实际 paid retry **0**，后续独立模型轮次不冒充重试。67 次原始费用均为 `null`，合计费用未知，不写成零。G3 累计 67/600 次、502,058/12,000,000 输入；到 v1 累计 263/3,000 次、3,822,119/60,000,000 输入，包含历史运行。

plain roundtrip 字节和 oracle 通过仍只有提交，验收待定；plain delivery 的 final 字节正确，但缺原始确认回执，完整 oracle 为 MISSING。plain AMEND 确已提交，随后无新 left 修订快照，保留转换未完成的结果。

multi AMEND 的原件和目标字节通过，但最后一批在凭据读取前被原模型预算守卫拒绝，新增调用为 0；已有验收是 `SUPERSEDED`，当前左右任务均未完成验收，右任务目标修订 2、已应用修订 1，不能写成交付。两格确认交付以前的 show 为 RUNNING，随后原始 ack 为 CONFIRMED、封存状态为 DELIVERED；补充记录解释时间顺序，没有改写原结果。

两种 rejected_rework 都在捕获不合格初稿的真实性/最终验收要求处先提问，没有原始负面审核或纠正，也没有实际尝试提交守卫；场景未触发，不能据此断言产品缺陷。两种 exhausted_rework 有开放问题，但缺原始失败/阻塞审核快照；提问不证明返工耗尽，完整 oracle 的 FAIL 保留。

私有证据索引（本机，不随仓库分发）：`/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/g3-b1-analysis/`，`aggregate-results.json` SHA-256 `bf1e485b821b3b5d60776f2f0bb6b0c0131bb62cd1335b8fe11d5f4cdaad4914`。汇总关联逐格结果与时间/守卫补充；`analysis-completion-receipt.json` 记录十格副本及捕获校验，根目录 `budget-after-g3-b1.json` 记录累计额度。原始日志、数据库、凭据和私有控制器均不入 Git。

**G3-C 合成候选（不计入上述 B1 真实调用）：** 分支 `feat/g3-fixed-single-task-20261117` 已正常导入 #152 main `de7f5b5`，改动 94 产品行、171 测试行。显式 `initial_plan.mode: fixed-single-task-v1` 只省首次 Dispatcher 规划；同一有效文件任务实测总调用 **7→6**、管理 **5→4**，Worker 均为一次 `fs_write` 加一次 `submit`，完整 brief、成果字节与保留的角色顺序一致。排除文件写入后固定开销为 6→5，原 **6→4 未达（UNMET）**；没有新增真实调用或改变冻结格式、默认值、AMEND、验收/验收后经验守卫。源码/38 项契约与定向 Release 通过；原始失败及修正证据在私有 `g3-c-evidence/result.md`，CI/D-40 仍 pending。

#152 核心 285/285、受影响 ASan 21/21、源码 PASS 后已合入；三组 Worker 托管 ASan 29.63/36.72/21.44 秒均低于 180 秒期限的 50%。main 完整矩阵 `44152280898` pending；候选受影响 ASan 选择器补齐至 25 组（95 项 CTest 注册），尚不能据选择器检查宣称候选 ASan PASS，链接与验证口径集中在[当前交接](../reviews/current-handoff.md#验证与证据)。下一步完成候选 CI/D-40，再按[前向计划 §15](../reviews/claude-2026-11-16/04-plan.md#15-修订记录)推进 E-9 两 PR、新冻结格式与 B2，不提前宣称 G3 出口。
