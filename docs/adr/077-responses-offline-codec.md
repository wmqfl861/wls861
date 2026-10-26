# ADR-077：外部网关 profile 与 Responses 离线编解码

日期：2026-10-26。状态：**决定已固定，实施中；实测结果另记**。节点 **R3.1**，承接
[路线图](../ROADMAP.md)的 S1-05/13；与[ADR-076](076-doctor-deployment-diagnostics.md)
及[ADR-078](078-responses-sse-replay.md)组成本次三个开发节点。

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
