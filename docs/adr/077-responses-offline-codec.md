# ADR-077：外部网关 profile 与 Responses 离线编解码

日期：2026-10-26。状态：**限定切片已实现；实测结果另记**。节点 **R3.1**，承接
[路线图](../ROADMAP.md)的 S1-05/13；与[ADR-076](076-doctor-deployment-diagnostics.md)
及[ADR-078](078-responses-sse-replay.md)组成本次三个开发节点。

实际 API、CLI 与限制见 [Responses 实施契约](../implementation/R3-offline-responses.md)，
实际结果见[本批验证](../reports/r2a3-r3-offline-validation.md)。实网与 Store 集成另验收。

## 决定与范围

首个协议选择 OpenAI Responses 的 HTTP JSON 文本与 function_call 子集，标识为
`openai-responses-v1`，本地 codec 版本为 `1`。独立 C++20 组件先编码结构化请求、解码
显式提供的本地响应。实现不创建网络连接，不读取认证值，不启动服务，不接管 Store。
本节点不移除现有 config_loader、FakeModel、OfflineRuntime 和 Store 的 fake-only 门禁。

外部 profile 明确给出模型引用和网关模型名、HTTPS base URL、相对请求路径、认证引用、
能力与边界；不内置供应商地址、模型或凭据。样例使用保留的 `.invalid` 域名。
首版 `real_calls_enabled` 必须为 false、`max_paid_retries` 必须为 0。认证仅为名称引用，
从不读取环境变量或凭据文件。profile 规范字节哈希进入编码证据，能力声明仍是输入声明，
不等于供应商现场已经核验。

## 请求与证据边界

只接受经原序列化校验的 structured-v2 ModelRequest，保持原 call/attempt/role/index、
执行绑定、配置哈希、完整任务契约和预留事实。旧 framed-v1 明确拒绝，不猜测拆分旧帧。
system 段、工具 Schema、当前任务与顺序消息分开映射；工具调用和结果用 call_id 配对。
原工具 Schema 不因供应商严格模式被悄悄改写，显式使用兼容的非严格函数定义。
思考设置只按 profile 明确支持的语义映射；不支持的 off/effort/budget 拒绝，不静默降级。

编码保存实际生成的 body 字节、body/profile/逻辑请求哈希及 HTTP method/url，明确
`network_dispatched:false`。旧 `wire_sha256` / `wire_request_sha256` 仍为 null。
编码字节不是已发送收据，离线响应不是实际供应商调用或 Store 接受记录。

完整 HTTP 响应保留原 body 和哈希；供应商 response ID 与宿主 call/attempt ID 分列。
只把完整、合法、受支持的终态映射为完成响应；错误、拒绝、不完整和不支持输出保持明确状态，
未采用输出不授予工具权限。reasoning、图像/音频、hosted tools 等不在首版输出子集中，
不得丢掉这些项目后声称完整可继续。原始响应保留在有界证据中供后继实现处理。
缺失的 token/cache/cost 保持 null；不按字节数或样例费率推算，不把缺失当零。
429/5xx 与协议错误均不授权重试。每个输入、输出、工具列表、文本及参数都有显式上限。

## CLI 与后继

新增只读离线入口用于编码与回放，参数必须先完整检查，再读取有界 fixture；不打开数据库。
范例与真实 CLI 回归随实现提交，完整命令以实施契约为准。
后继真实 HTTPS/TLS、认证解析、网络取消、attempt/wire 持久化、预算结算及获准小任务
另立节点；本地 codec 不成为绕过当前 Store 版本与业务门禁的入口。

## 协议依据与验收

2026-10-26 实际读取的官方依据：

- [Responses 迁移与 Item 映射](https://developers.openai.com/api/docs/guides/migrate-to-responses)
- [Function calling](https://developers.openai.com/api/docs/guides/function-calling)
- [Responses streaming events](https://developers.openai.com/api/reference/resources/responses/streaming-events)

完整 create API 参考页因抓取大小上限未读取；不据此声称核验全部协议或模型能力。
测试至少覆盖规范字节、原 tuple、原生工具往返、profile/模型/协议错配、未知字段/重复键、
旧格式拒绝、边界、错误 HTTP、nullable usage、无效参数、未知工具和零网络/零重试。
测试使用明确合成的请求与响应，不将通过结果记为真实供应商或产品质量验收。

## G1-2 追加：wls-responses-compat-v1 兼容子集（2026-11-08）

在冻结 profile 末尾追加可缺省 `responses_compatibility` 扩展后，同一 codec 额外接受一个
显式 opt-in 子集；无扩展的 profile 序列化、哈希与既有严格行为逐字节不变。扩展对象只允许
`{"format":"wls-responses-compat-v1","reference":"openai-responses-2026-11-08",
"model_aliases":{"<profile.model>":["<snapshot>",...]}}`：唯一 key 必须精确等于当前
profile.model，值为 1..8 个唯一、非空、≤128B 的 ASCII token，列表排序规范化后随完整
配置进入 profile 哈希。无 wildcard、前缀或日期推断；响应 model 可等于 alias 本身或属于
该显式列表，SSE created→terminal 仍须同模型。请求侧 `store:false` 时显式发送
`include:["reasoning.encrypted_content"]`；instructions 改为六段（system_prompt、
role_prompt、function_prompt、environment_card、approved_rules、project_snapshot）
按固定顺序 `# <段名>\n<原文>` 以空行分隔组成的 UTF-8 纯文本，`instructions_sha256`
等进入编码 evidence，不把哈希包装进 instructions。

输出侧新增 reasoning 项目与 refusal 内容：reasoning 需 id/type/summary（有界
summary_text 列表），encrypted_content 为不透明字符串或 null；终端项目必须 completed，
content 只允许缺省或空数组，reasoning_text 等未知字段拒绝。缺密文的完整解析产生
`reasoning_replay_unavailable` 模型错误而非可继续完成；reasoning-only 不构成业务完成。
refusal 内容令整响应为模型层失败 `model_refusal`：公开 text/tool_calls 清空，经验证的
usage 与完整合法 `responses_output` 原件（`{format:"wls-responses-output-v1",
reference,model:<实际>,output:<完整有序原数组>}`）保留；协议级未知字段/结构错误优先于
refusal 分类。`ModelResponse.responses_output` 缺省 null，旧序列化不写该字段；共享
`src/responses_compat.hpp` 纯校验器同时服务 HTTP/SSE 终端解码、restore、历史原生轮
回放投影一致性检查。下一轮编码按原 output 顺序原样回放 reasoning（JSON 值、字段与
encrypted_content 字节不变），function_call_output 使用原 provider call_id；跨轮重复
provider item/call ID 在编码前明确拒绝，兼容 profile 下无原件的原生轮不得静默丢
reasoning 后继续。旧 profile/无扩展历史保持 history_... 逻辑 ID 路径不变。
本地兼容契约与文档子集标识固定为上述字符串；fixture 模型 `o3`/`o3-2025-04-16` 仅用于
离线回放，该官方页已标 Deprecated，不构成生产模型选择或可调用性声明。
