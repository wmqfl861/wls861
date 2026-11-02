# N4：下一批三张设计与验收卡

日期：2026-11-02 UTC。阅读基线为正常本地合并提交
`f3fc9bfaf345030ee994097d2a2443e3c290e9fc`，两个父提交分别为 PR14
`4289589384d75f29d42233cbaad256ceb711e9bf` 和 PR13
`ceaa9b3e18618c592345001dfc597bdaf579cf81`。这表示候选包含两个分支，
不表示 GitHub PR 或 main 已合并。运行版本仍为 `0.11.1-r3.3c-audit`、Store 410。

本批产出是 **DESIGN_PROPOSED / IMPLEMENTATION_NOT_STARTED** 的三张卡及本索引。
下文“拟议”类型、方法、状态和表名是后继设计候选，当前头文件没有这些接口。
没有实现 DAG、自动异步 inbox 驱动、Windows 私有文件权限或新迁移；没有新增产品测试。
所有验收用例均为 **PLANNED / NOT_RUN**，文档任务的旧版缺陷反例为 **N/A**。
本目录不改变既有报告、原 P1 修复处置、历史 sanitizer 失败或六状态。

## 派发依据与本次核对

协调交接来自提交 `c33978ca62fe3fc63b4cf3440afdc4f5b647f13b` 的
[`docs/reviews/coordinator-20261101/`](https://github.com/wmqfl861/wls861/tree/c33978ca62fe3fc63b4cf3440afdc4f5b647f13b/docs/reviews/coordinator-20261101)。
执行工作区根目录 `inputs/` 的三份文件已经完整读取，并逐份核对大小及 SHA-256：

| 文件 | 字节 | SHA-256 |
| --- | ---: | --- |
| `REVIEW_AND_HANDOFF.md` | 17188 | `064125baafca0db50ea3cc253bd81c6190beadf9239545e7f68bb68cbf47d190` |
| `NEXT_AGENT_PROMPT.md` | 16132 | `0b7cabf4e876d02ca0e4e3a39cec269e3b6f6574846136bfcc5177b5fd817c72` |
| `AUDIT_EVIDENCE.json` | 12900 | `58d173a21749af183a3aa453ead9801adca89d36cdd6b1c3e31d02d595b234f3` |

依据还有当前 [AGENTS](../../../AGENTS.md)、[A3](../A3-async-coordinator.md)、
[A4](../A4-fair-scheduling.md)、[S1 事件](../S1-event-runtime.md)、
[工作流](../S1-workflow-runtime.md)、[验收](../S1-acceptance-delivery.md)、
[按需恢复](../R2-demand-driven-recovery.md)、[Windows 原生基础](../windows-native-foundation.md)
和 [B0–B6 依赖](../../spec/v0.5.0/10-authorized-cycle-and-followups.md)。
卡片中的当前行为以本基线的公开头及对应实现为准。

结构查询遵循已读取的 `codebase-memory` 技能。父 Agent 提供的图为
`wls861-next-cycle`、generation `2026-10-31T06:55:42Z`，根仍是旧 772 输入；
Async 源缺失，Store/EventRuntime 头覆盖部分。本文作者的工具目录没有可调用的图工具，
因此没有冒称重新完成图查询或覆盖验证；实际按当前头文件、实现、SQL 和契约逐项读/检索，
不用旧图的未命中证明接口不存在。核对范围是三卡列出的路径，不是全仓语义审核。

## 卡片、可交纵向范围与先后顺序

| 卡 | 最小可交能力 | 前置与明确退出 |
| --- | --- | --- |
| [N4-A：有限异步工作流](01-async-inbox-driver.md) | 单 Store owner 下接原 inbox、捕获目标、唯一调用、原反馈和逐 ID 确认；先 FakeModel 的两个请求、各两个独立节点 | 先采纳持久事件—调用关联及完成事务方案；重启不重派、部分完成不吞事件、控制可达后，才交该有限入口 |
| [N4-B：DAG 与独立审核](02-dag-review-tickets.md) | 三节点依赖汇合，加一个固定候选的独立审核子请求、有限返工 | 复用获准的异步驱动和 A4；先采纳图版本及双方票据事实，事务守卫与双容量证据齐全后交一条纵向路径 |
| [N4-C：Windows 只读身份](03-windows-readonly-identity.md) | 本机 NTFS 上的根 Handle、对象身份、ACL 拒绝与完整公共资源捕获 | 与 Linux 卡并行；依赖 N3 的实际目标环境和来源记录；普通用户允许/拒绝与替换竞争通过后，只声明该只读档位 |

建议顺序为稳定候选的工程门禁 → N4-A → N4-B；N4-C 可从 N3 前置并行。
N1 尚未闭环不妨碍设计或隔离的离线实现准备，也不由这些准备豁免产品发布门禁。
完整 Workspace/Store/进程/强隔离/清理是 Windows 后继卡。真实 S1 质量与费用验收独立需要
中转端点、模型、能力、可信计费、预算及调用授权；本三卡没有真实 WLS 调用授权。

## 交给协调者的具体选择

1. **事件关联持久化：** 采纳 N4-A 的事务约束后，在追加事件事实或新附加表/迁移两种实现中选择。
   本文建议先固定语义和容量，再批准最小存储方案；内存 future 映射不足以支持 Worker 崩溃后去重。
2. **DAG/审核协议版本：** 决定新图版本索引、S1 submission 到 candidate 的不可变映射、
   producer 完整执行绑定的补充载体，以及审核子请求的额度来源。原 SQL/Schema 不能原地补字段。
3. **Windows 支持政策：** 固定首个 Win11 build、普通令牌、工具链、NTFS 和受信管理主体集合；
   若安全祖先固定或 ACL 判定无法证明，相关打开拒绝，不发布通用文件能力。

每张卡采用“现有接口 / 拟议接口 / 持久事实 / 状态与失败 / 正负验收 / 退出”格式。
拟议新能力没有可运行命令可供宣称成功。后继实现须给精确输入、适用原套件、原断言/期限、
source/binary 身份、非作者复核及最终 SHA 的本机/远端结果；新增方法与 CTest 注册数分别计数。
N4 本批只做文档引用、标识和差异检查，产品构建/测试及远端 CI 不是本批执行成果。

作者为本地执行 Agent `/root/delivery_report`。本文的来源核对与设计自查不等于非作者审核，
也不等于主协调者采纳。最终本地提交与后继整合/发布由父 Agent 的外部回执登记，
不在本文预填自身提交 SHA、推送或批准结果。
