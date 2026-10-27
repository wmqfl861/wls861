# R3 单 Agent 逻辑身份与基线运行器

设计决定见 [ADR-089](../adr/089-single-agent-role-broker.md)。本模块定义共享原始模型
历史、原角色权限代理及 v3 评测，不代表真实供应商质量对照已经执行。

## 1. 显式冻结策略

在外部系统配置的 `context` 中加入：

```yaml
request_format: structured-v2
logical_agent:
  mode: single-agent-role-broker-v1
  max_attempts: 128
  max_history_bytes: 524288
```

三个业务模型角色必须使用相同模型与 reasoning。策略随请求原配置冻结；不能给旧
多角色请求事后贴标签，也不能在一次运行途中切换上下文拓扑。原角色和工具权限保持。
`max_attempts` 是逻辑历史容量，不增加原请求尝试预算；原预算仍可更低。历史字节和
原上下文整体容量分别检查，超限显式退出，不能截断已有失败或未解决事实。

库入口为 `SingleAgentRuntime::pump` 和 `pump_live`；CLI 对应 `pump-single-fake` 与
`pump-single-live`，参数及网络/计费授权沿用各自原 pump。原 `pump-fake`、`pump-live`
拒绝单 Agent 冻结策略，防止命令名称和执行拓扑混淆；`live-status` 可只读查看原冻结身份。

## 2. 一个逻辑执行者与多个权限代理

每次调用仍由原运行器选择一个当前角色/任务/目标。该调用带完整当前契约和该权限
代理允许的工具，并在实际 structured-v2 消息中包含该请求所有前序原响应与反馈。
因此下一位权限代理接续同一模型历史；角色、团队及 Worker ID 继续是权限记录，
不作为逻辑 Agent 数量。模型工具响应仍经原首次结算与业务应用，不获得跨任务授权。

原成功工具轮次以原生 assistant/tool 对进入实际协议请求；失败或未采用的响应保留
为有来源的观察，不能变成成功工具结果。原调用工具 ID 通过现有历史映射生成唯一 wire
ID，原 ID 和原反馈 hash 保留。以前的完整上下文不递归写入下一轮。

## 3. 顺序和恢复

每个原模型 request 的 task contract 保存 `logical_agent_execution`：固定逻辑 ID、
contract、全局步号、history count、history SHA256、previous chain SHA256。原历史
记录内也保留该步原标记。链从 request ID、冻结配置 hash 和完整策略派生，逐项包含
原预约、原响应、原结算和原反馈。新预约在原事务中重建并核对标记及实际原生消息。

缺失、改变、遗漏或调序任一原调用都会拒绝。两个捕获不能同时从相同前链继续；
原幂等预约重放不追加新步。前序未结算或缺不可变反馈时，新调用保持 NotReady，
不重放旧业务效果。正常闭合轮次在重新打开 Store 后仍可继续同一逻辑链，当前调用
使用新恢复身份；历史保留其当时 binding。

## 4. 评测范围

v3 保留固定五任务和十个条件样本位，继承 `model-origin-v1` 的原业务来源判据。
单 Agent 条件需要完整共享历史证据，多角色条件需要原隔离策略；任意旧数据库追加
同一个 ID、宿主直接完成业务、跳过前序调用或只提供 hashes 都不能通过。
共同条件只剥离这个明确的拓扑变量，其余上下文策略、工具、模型、reasoning、预算
和环境仍逐项相同。v1/v2 历史报告的身份与结构解释保持。

跨 CLI 重启时，v3 的 `original-instruction-epoch-rebind-v1` 只承认原 Store 恢复证据。
原 `instructions` 的完整 body 必须等于原模型工单及其原 binding，恢复事件中的 body
SHA256、前后 dispatcher/execution epoch 必须与原工单和实际提交精确匹配；有界读取
原工单之后、实际提交之前的全部 recover，核对每次实际当前 epoch 连续递增及精确终值。
Store 的 `previous_*` 指向不可变原工单 epoch，不是上一投影；评测保留这个原含义，
不杜撰中间 binding 字段。缺中间恢复记录也不能通过。其余 binding 字段不得改变。
报告仅输出原 attempt/event ID、顺序和 hashes，不输出工单正文。缺少或损坏投影保持
失败；v1/v2 继续要求工单与提交的完整 binding 相等，不重新解释旧报告。

使用 `evaluate_r3.py plan --identity-contract single-agent-role-broker-v1` 显式生成 v3。
种子请求须 structured-v2，且尚未发生模型调用。计划固定五任务、两个条件及一轮重复；
`single_agent_policy` 采用种子已有策略或上面的128步/512KiB策略。每个实际 single 请求
须精确匹配该策略，多角色请求不含该字段；报告保留所有失败和未执行样本。

实际 provider 运行仍需外部端点/能力/凭据、正数费用预留和独立宿主授权。费用未知
保持 null；连续 live 工作还须独立受信账目核验。合成测试不构成真实质量或成本结论。
