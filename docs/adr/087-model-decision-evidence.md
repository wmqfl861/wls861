# ADR-087：版本化的模型决定来源验收

- 日期：2026-10-26（America/Los_Angeles）
- 状态：接受
- 关联：[ADR-082](082-paired-evaluation-evidence.md)、[R3 评测契约](../implementation/R3-evaluation-framework.md)、[两模块接续](../implementation/two-module-continuation.md)

## 背景

原 `r3-small-tasks-v1` 框架核对固定材料、共同配置、角色身份、原账本及最终 Store 事实。
它能证明一个请求存在 update/keep、否定验收、返工、升级或确认交付，也会列出原 live
attempt 和传输观察；这些条件不足以证明每个业务决定来自该模型的实际响应。
受信宿主仍可通过独立 Store API 作出合法管理动作。最终事实成立不能把宿主决定变成模型决定。

现有 v1 计划和合成报告已经发布。直接改变其评分含义，或把旧 `passed` 解释为实际模型质量，
都会丢失原报告的证据范围。另一方面，AMEND 两任务与现有 `single_agent` 同一 Worker
principal/team 规则存在执行身份设计缺口；该缺口不能靠放宽评测身份检查解决。

## 决定

### 1. 保留 v1，新增明确的 v2 来源契约

- `schema_version: 1` / `r3-small-tasks-v1` 保持固定任务、判据和身份解释。
  报告新增明确标记 `structural-v1`、`original_structural_checks_only`，模型决定来源为 `not_checked`。
- `schema_version: 2` / `r3-small-tasks-v2` 固定 `decision_evidence: model-origin-v1`，
  在结构判据之外核对原模型应用链。新 provider 计划必须使用 v2；旧 v1 provider 计划仍可读取，
  其结果只具有原结构含义。
- Synthetic 计划默认仍为 v1；开发回归可显式选择 v2。`synthetic_fake`、`local_tls`、`provider`
  分类继续来自原 Store 记录，不能由运行索引或新标签改写。
- `provider_quality_acceptance_established` 与普遍质量/费用优势标记保持 false。
  来源链通过只证明受信宿主记录内的关系；端点身份、模型能力、业务语义、人审质量、实际计费与
  单 Agent 比较仍需各自明确执行证据。

### 2. 从原响应走到实际应用，不从最终状态补写决定

每项计分的模型应用必须关联以下原始资料：

| 原资料 | 必须一致的关系 |
| --- | --- |
| attempt / request envelope | call/attempt、角色、调用序号、配置 hash 和完整 frozen binding |
| captured task contract | 原 v1 帧或 v2 logical input 内的 workflow_action、目标 ID/CAS 和任务/请求版本；不用当前行替代 |
| 原 completed response | 唯一已采用的原 tool_call、原 arguments；attempt accepted 不等同于业务接受 |
| live 发送/观察（存在时） | 同 attempt 的唯一 send claim、原 binding、完整传输、已冻结请求 hash、原 decoded response 和原字节 hash |
| immutable tool feedback | 原 response/binding/result 及一致性 hash；缺反馈不授予事后应用权 |
| 原 application event | 由 attempt 与 tool_call 生成的原 command ID、原 actor、payload hash、目标版本和实际 result |
| 原业务记录 | plan/task、operation、submission、check、acceptance、AMEND disposition 或 question 与该 application 相同 |

只有宿主注入的最终业务事实、别的 attempt 响应、错误目标、缺 feedback 或不相同的
传输响应均不能满足来源链。缺失可定位资料保留为失败；矛盾或破坏的一致性资料为 `invalid_evidence`。
公用报告只列 ID、角色、动作名、目标 hash、feedback/event/transport 身份与来源范围，不展开
原提示词、模型 arguments、私有响应、独立账单原文或交付正文。

这些 hash 仅校验本地关系与原记录完整性，不是外部签名，也不能防止有权限修改全部本地事实的宿主。

### 3. 固定任务各自需要完整的决定链

- 工具往返：原 Worker `fs_read(seed.txt)`、`fs_write(result.txt)`、`fs_read(result.txt)`
  按原调用顺序、相同绑定完成；诊断完整读取必须原 offset=0、EOF、长度及内容 hash 一致。
  `fs_read` 原本不生成 ops 行，v2 使用 immutable feedback。提交中的 `/work/` 路径只作该前缀的
  明确映射，不放宽到任意宿主路径。必须由同一原绑定随后产生模型 submission，再由模型检查
  该 submission；期望文件的原字节也必须属于这同一份提交。不能把 A 的工具往返、宿主 A 的
  正确文件和 B 的模型提交拼成一个成功来源。
- AMEND：原 `submit_amendment_plan` 与必要的 `resolve_amendment` 捕获正确阶段、pending 节点和
  每项任务 CAS；模型原 dispositions 与原处置记录相同。不同任务的 update 和 keep 都须有
  当前版本的模型提交、检查和接受。
- 否定返工：同一原任务的模型否定接受 → 模型 `change_task` → 较新修订的模型提交/检查/接受。
- 耗尽升级：原失败来源、捕获时已耗尽的冻结计数、模型 `escalate_task` 与实际当前用户问题一致。
- 确认交付：精确原 package/receipt 的每个 acceptance 来源须有模型 plan、order、submission、
  check、accept 链。Root 技术发布和用户 receipt 继续是原独立宿主动作，不能被重新称为模型决定。

当前成功来源还需实际 acceptance 仍为 ACCEPTED、实际任务仍为 ACCEPTED，且原 producer 的
需求/计划/任务/指令版本与当前业务版本一致。原反馈继续核当时完整 epoch；恢复 epoch 的变化
不自动否定已经成立的业务接受。返工的否定来源、纠正目标与较新接受必须属于当前需求/计划；
升级必须对应当前需求/计划的同一原问题，不能由旧模型链与另一当前宿主事实分别满足判据。

### 4. 不放宽基线身份或账本含义

v1/v2 `single_agent` 继续只接受同一 Worker principal/team，不能把多个团队或管理角色重命名为
单 Agent。通用基线执行器及两任务跨身份设计保持开放，未来另行版本化。

所有失败、未知、未执行样本继续占据固定十个样本位。未知 usage 保留 null 和 unknown 计数；
额外核对不改写原响应或结算，也不授予旧结果新的业务资格。评测只读，不执行迁移、恢复、工具、
网络或核对写入。原读取容量界限继续适用。

## 验证与影响

新增真实 SQLite/Fake 负例覆盖宿主 command 代替、缺 feedback、错 attempt 和 captured target；
loopback TLS 回归核对真实原响应、同一 workflow 调度、独立合成账单及不重复应用。
本地合成运行的来源链通过可验收工程连接，不能形成 provider 或模型质量结论。
专项实际命令与结果另记于 [验证报告](../reports/live-workflow-evidence-validation.md)。
