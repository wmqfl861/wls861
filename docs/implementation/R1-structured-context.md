# R1.2：冻结选择的结构化模型请求

实现：0.7.4-r1.2；目标蓝图v0.4.1；设计见[ADR-068](../adr/068-structured-model-input.md)。

## 表示与持久化

`CompiledContext.logical_input` 在 v2 是可直接检查字段的 JSON 对象：selection、prefix、
完整 task_contract 与有序 messages。prefix 独立保存工具参数 Schema、具名 system 段和
依赖；messages 的 role/content/tool_calls/tool_call_id/source 独立，不把工具调用编码成
一条纯文本用户 prompt。`compile_structured_context` 的旧 messages 入参必须为空，拒绝
静默丢弃未转换历史。工具定义/依赖按名称规范排序，消息顺序保持。

消息只允许 user、assistant、tool。assistant 可提出1..32个工具调用；同一轮所有结果
必须完成后才能加入下一条普通消息，每个 ID 在整个上下文中唯一，不能有孤立结果、重复
结果或未配对调用。空的工具结果内容可表达真实空输出。历史中旧工具名是观察，不加入
当前 tool_names；权限仍在响应采用和 Store 的当前 grant/版本/CAS 处检查。

`wls-compiled-context-v2` 的 prefix_sha256、cache_identity、logical_input_sha256 分别
绑定规范前缀、完整模型/协议/思考缓存选择及结构化输入。`wls-model-request-v2` 再增加
logical_request_sha256，覆盖其完整捕获封装（含调用ID、绑定、冻结配置、输出限额、超时
和预留），计算时排除该哈希及 wire_request_sha256 自身。输入/请求两级 wire 字段均为
null：没有网络发送。哈希不是认证，改变输入并重算哈希不能绕过 Store 的授权。

恢复不仅核哈希，还重新编译验证字段、模型元数据、Canonical排序及工具配对。额外字段、
伪造 wire 指纹、格式/上下文不一致都拒绝。旧 v1 序列化、帧和恢复代码保持兼容。
`compiled_task_contract` 提供统一宿主读取入口，管理工作流和经验目标不再各自解析旧帧。

## 冻结选择与运行器

在新请求所用的 system YAML 中显式设置：

```yaml
context:
  request_format: structured-v2
  # 其他既有 context 字段继续保留，不能仅用此片段替换完整配置。
```

支持 framed-v1、structured-v2、structured-v3；缺字段按 framed-v1，未知值在准入前拒绝。
新请求样例选择 structured-v3；窗口仍是独立显式策略，`config check/explain` 返回原冻结选择。
已有请求只恢复其原冻结配置，不读取后来 YAML。Store 在新预约时同时检查格式选择，错配
不消耗预算；已有精确预约重放保留旧封装。数据库格式和已发布迁移不改变。

run-fake/pump-fake 的 Dispatcher、TeamLead、Worker 均可使用v2；管理角色仍读取当前权威
快照并捕获单一问题/提交/验收/经验目标。Worker 历史来源是自己作用域的已保存尝试。
单一、完整且已采用的响应与存在的原反馈核对后生成 assistant/tool 消息；result 保留完整
JSON、错误/失败状态与原文件字节，不把失败结算等同业务成功。

逻辑 tool ID=`history_`加（域标记、attempt_id、原call_id）规范JSON的SHA-256；原ID、
原版本绑定及记录指纹保留在source。跨重开或重复供应商ID不串结果。反馈缺失、迟到/未采用、
多个调用、无完整响应或运行时拒绝记录为明确的user观察；不补造结果、不执行旧工具。
402/403兼容入口的宿主反馈同样仅表示观察，不伪造原生工具对。当前404+路径忽略外来反馈，
以持久记录为准。

## 容量与下一切片

本批仍保留完整历史的256条/8MiB读取边界，结构化编译仍受1MiB总逻辑字节、256条消息等
限制；原生对占两条消息。超限在预约前明确拒绝，不裁剪约束或伪造旧文件结果。
这不是窗口或压缩实现。R1.3 必须把冻结窗口、遗漏索引、R1.1原始证据回读及未决状态保护
接入Worker并验证连续61,100字节结果和跨重开可继续；当前该问题仍开放。

没有新增真实模型调用、网络适配器、付费重试、弱沙箱、SQL迁移、模型新权限或自动压缩。
实际测试与发布状态见[报告](../reports/r1-structured-context-validation.md)。

## G1-2 opaque 原件补充（2026-11-08）

`StructuredContextMessage` 末尾追加缺省 null 的 `responses_output` 载体：仅经
structured_attempt_history 核验的原生 assistant 轮可携带，tool/user 消息拒绝；encode、
restore、logical hash 与完整 request hash 均覆盖该字段，超限在预约前以 Quota 拒绝。
worker/management 窗口与 single-agent 消息计量同时计入该字段；最近需接续的
reasoning-bearing 轮超出冻结 inline_bytes 时以 Quota 退出，不静默降级为丢 reasoning 的
history_reference，更早的完成轮保持原引用策略。兼容 profile 的原生轮回放使用原
provider item/call ID（跨轮重复在编码前拒绝），旧 profile 保持 history_... 逻辑 ID 不变；
`scripts/evaluate_r3.py` 的独立 logical-agent projection 同步携带并校验该原件，缺失/篡改
encrypted_content 为 invalid evidence。边界详见
[ADR-077 G1-2 节](../adr/077-responses-offline-codec.md)。

## G1-F：固定角色目录与本轮允许工具

新请求可显式冻结 `context.tool_policy: fixed-role-v1`，仅支持 `structured-v3`
和持久反馈 Store。示例现已选择它；缺字段的 v1/v2/E-v3 快照不补默认值，原
context、缓存身份、请求哈希和协议字节保持原解释。

完整 `tool_names`/协议 `tools` 使用固定枚举与冻结角色/Worker 模板许可；以后新增
注册工具不会扩充 `fixed-role-v1`。未实现的 recall/load_memory 不列入目录，
owned-capacity 仍禁止 run。角色变化继续形成缓存边界，单 Agent 的历史保留原回合
权限、provider IDs、opaque 输出及来源，不以当前角色或本轮允许列表重审旧回合。

捕获时先计算原动作和当前授权得到本轮工具，再构造完整目录。规范逻辑请求保存
`tool_policy`，末尾 `current_state.allowed_tools` 保存排序唯一、非空、至多64项的
目录子集；两者进入逻辑/请求身份，策略还区分缓存身份。恢复重编译和 Store 准入
均核冻结选择与完整目录，当前授权/history/experience 检查针对本轮列表。
experience 本轮仅允许 report_experience，即使完整目录包含 history 也不能打开它。
本轮不允许的模型工具选择在原用量结算及 accepted/CAS 门禁后进入 tool_selection
纠错反馈；宿主撤权、UNKNOWN、预算、期限及最终派发守卫仍立即停止。

ProviderProfile 可独立显式选择 `tool_choice_policy: allowed-tools-v1`。仅配合新
固定目录策略时，Responses encoder 输出官方 allowed_tools 的 type/mode/tools
对象；未选择能力的端点保持完整 tools 和末尾允许列表，由宿主强制执行。
tool_choice 是动态控制字段；稳定性比较仅覆盖 tools/instructions 与有序历史项，
不把整个 HTTP JSON 声称为严格前缀。此改动不实现折叠、模型投影或真实缓存验收。

## G1-E v3 契约锚、更新和当前状态

本布局接合已合入的 D #57 `guided-v1` 提示词、K #59 响应/profile 解码及 J #58
`bounded-v1` 运行控制；各自原冻结选择、费用/用量原件和宿主安全检查继续独立生效。

`structured-v3` 同时保存完整原 `task_contract`、原 `messages`、对象 `contract_anchor`、
有序 `contract_updates` 和末条 `current_state`。宿主的 CAS、目标及经验判断继续只读完整原
task_contract。锚保存请求原文/约束、任务 brief 和说明正文（含验收项）；正文中的宿主绑定
单独保存在 current_state，原件仍完整留在 task_contract。预算、版本/代次、待办、等待、
workflow_action 原目标、全部问题/提交/经验/文件原件及未决事实留在 current_state。

第一次捕获冻结锚。后续 AMEND/新说明只追加完整语义投影及 semantic_versions，使用
`after_attempt_id` 标明原历史边界，不把当前契约替换锚，不使用窗口中的消息偏移充当持久位置。
相同内容/语义版本不重复追加。Store 仅加载最近一个同 Actor 作用域的原 envelope；单 Agent
采用原请求级全角色作用域。该次有界读取最多 2 MiB，重新验证完整 context/request/reservation
指纹；损坏、缺锚、错版本或超限最新记录都拒绝，不回退旧记录或用当前契约补造原锚。

wire 顺序为 tools、稳定 instructions、稳定锚、原历史与按边界插入的契约更新、最后一条 user
current_state。不改历史 provider item/call ID、reasoning、feedback 或原逻辑消息。窗口索引
和 message_sources 在末条状态出站；团队唯一 execution_policy_sha256 及其 worker_execution_policy
依赖也移到状态，WorkerTools 与 Store 仍验证原冻结工作区身份。末条状态不写入原历史。

`wls-model-request-v3` 只接受 `wls-compiled-context-v3` / `wls-logical-input-v3`；v1/v2 同样精确
匹配且原字节/哈希解释不变。v3 prefix_sha256 覆盖规范 prefix 与原锚，完整 logical/request
指纹另外覆盖所有投影、原件及预约字段。restore 重建精确投影并核哈希，Store 新预约在事务内
核锚与更新连续性。管理窗口按完整最终逻辑字节计量，单 Agent 仍逐值核全角色原 messages。

稳定前缀比较采用实际 encoder 的有序 input 项（去掉最后状态），并分别比较 tools/instructions；
不比较带结束括号的完整 HTTP body。window-v1 保持原整轮淘汰语义：窗口移动会改变前缀，更新
边界离开窗口后按原更新顺序放在可见历史之前。角色或权限工具集变化同样是缓存边界。
固定目录和 lean 投影使用各自显式冻结策略，折叠仍属后续范围，不重解释已持久 v3。

## G1-G：可选 lean-v1 模型投影

新请求可在 `system.context` 显式选择 `request_format: structured-v3` 与
`model_projection: lean-v1`，新示例已显式选择。缺字段保留原 v1/v2/E-v3 字节；冻结、capture、restore 和
Store 新预约校验同一选择，逻辑/请求指纹及 cache identity 绑定该选择。它独立于工具目录策略。

lean-v1 仅在编码副本中移除 message_sources 及窗口消息的来源 sidecar；原逻辑 messages、
来源与反馈原件继续供宿主核验。窗口遗漏、UNKNOWN、未决事实及原读取句柄仍完整可读；
业务对象中名为 source/metadata 的字段保持。稳定契约和按 E 原边界插入的契约更新直接编码
为单层规范 JSON 对象文本，更新保留 semantic_versions，末条状态仍最后出站。

只有已配对的原 fs_read 调用且结果符合正常页结构才去重：content 为字符串（含空串）时
仅省略 content_base64；content 为 null（含 UTF-8 分页切分）时仅省略 content。路径、偏移、
大小、SHA、EOF 和 next_offset 保留；错误与观察不改写。宿主提交快照仍读取原 base64，
原生 reasoning 与 provider item/call ID 保留。原逻辑与 wire 字节上限继续分别执行。
