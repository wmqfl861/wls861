# R3：Responses 离线协议契约

蓝图 v0.4.1；R3.1 设计见 [ADR-077](../adr/077-responses-offline-codec.md)，
R3.2 设计见 [ADR-078](../adr/078-responses-sse-replay.md)。本文件说明本地协议组件的
输入、输出和拒绝边界；实际构建与测试结果由本次版本报告另行记录。

## R3.1 范围与调用边界

R3.1 实现 `openai-responses-v1`、codec 版本 `1` 的 HTTP JSON 文本与
`function_call` 子集。头文件为 `include/wls/provider_profile.hpp` 和
`include/wls/protocol_codec.hpp`，公开接口均在 `wls::s1` 命名空间。
组件接受显式 profile、原 `ModelRequest` 和调用方提供的响应字节，产出独立的本地报告。

编码和解码均不创建网络连接，不解析凭据，不发送 Authorization header，不启动服务、
线程或重试，也不打开 Store。现有 config_loader、FakeModel、OfflineRuntime 和 Store
的 fake-only 门禁保持原语义。一个 profile 或合成请求通过 codec 校验，只证明它符合
这套离线输入契约，不能证明存在真实 attempt、预算预约、当前业务资格或供应商能力。

`ModelResultStatus::Completed` 在 JSON 中写为 `status:"completed"`，表示本地响应在
本子集内完整且可解码；另外两种 JSON 状态为 `error` 和 `interrupted`。工具 Schema、
原捕获调用、当前版本、执行代次、权限、UNKNOWN、AMEND 和最终 Store 提交/派发守卫
仍由宿主执行。该组件不执行解码出来的工具，不把模型完成状态写成业务接受或交付。

## 外部 profile

完整可读样例见
[`config/examples/providers/responses-offline.json`](../../config/examples/providers/responses-offline.json)。
`provider_profile_from_json()` 要求顶层、`capabilities`、`limits` 的所有定义字段显式存在；
缺失字段、未知字段、重复能力、错误类型和不支持的版本均拒绝。调用方读取原 JSON 字节时
应先使用有界 `parse_json_strict()`；profile 解析器另外限制规范化后的 profile 不超过 64 KiB。

| 字段 | 当前契约 |
| --- | --- |
| `format` / `adapter` | 固定为 `wls-provider-profile-v1` / `openai-responses` |
| `adapter_version` / `protocol` | 固定为 `1` / `openai-responses-v1` |
| `profile_id` / `model_ref` | 显式标识；1–128 字节，允许 ASCII 字母、数字、`_`、`-`、`.` |
| `model` | 网关侧模型名；1–128 字节可打印 ASCII token，响应必须逐字相等 |
| `base_url` / `responses_path` | 显式 HTTPS 地址和路径；目标 URL 为二者直接拼接，不补默认端点 |
| `api_key_env` | 仅环境变量名称引用；符合名称语法，最多 128 字节，不读取对应值 |
| `real_calls_enabled` | 只能为 `false` |
| `max_paid_retries` | 只能为整数 `0` |
| `capabilities` | 显式声明 streaming、tool calling、parallel tools、reasoning 和 token 上限 |
| `limits` | 显式声明字节、数量、深度和时间上限 |

URL 只接受当前实现的 HTTPS DNS/IPv4 形式主机语法、可选 1–65535 端口以及可选路径前缀。
没有 DNS、TLS 或服务探测。IPv6 authority、userinfo、query、fragment、百分号编码、
反斜线、重复斜线和 `.`/`..` 路径段均不在此版语法内；base URL 不能以 `/` 结尾。
`responses_path` 必须以 `/` 开始，不能仅为 `/` 或以 `/` 结尾。路径段只接受 ASCII
字母、数字、`_`、`-`、`.`、`~`；base URL 最多 2048 字节，路径最多 1024 字节。

能力是调用方声明，组件不以模型名称猜测能力。`streaming`、`tool_calling` 和
`parallel_tool_calls` 必须是 JSON boolean；并行工具要求同时启用工具能力。
`context_window` 必须在 1–100000000 内，`max_output_tokens` 必须为正且不超过该窗口。
这些声明和请求中的 reservation 只参与本地上限检查，不是 tokenizer 实测或账本结算。

### Reasoning 映射

`reasoning_modes` 只接受 `provider_default`、`off`、`effort`。至少声明一种可用模式；
出现 `effort` 时，`reasoning_efforts` 必须非空；未声明 `effort` 时该列表必须为空。
支持声明的 effort 名称为 `minimal`、`low`、`medium`、`high`、`xhigh`、`max`，
请求实际使用的值还必须属于该 profile 的明确集合。

| 已解析的宿主请求 | Profile 条件 | 编码结果 |
| --- | --- | --- |
| `ProviderDefault` | 声明 `provider_default` | 省略 `reasoning`，不替供应商选择档位 |
| `Disabled` | 声明 `off` | `reasoning: {"effort":"none"}` |
| `Explicit` | 声明该 effort | `reasoning: {"effort":"请求中的值"}` |
| `Inherit`、未知 effort 或 budget 表达 | 不接受未解析或无映射的语义 | 编码前拒绝，不静默降级 |

`max` 是一个可声明的 effort 名称；本接口没有把宿主 token budget 换算为 reasoning
effort 的功能。允许编码某个 reasoning 配置也不表示支持所有对应输出：`reasoning`
输出 item 仍在当前拒绝范围内，因此不能据此声称支持完整多轮 reasoning 工具对话。

### 边界值

下表的默认值是 C++ `ProtocolLimits` 初值和示例 profile 的值。JSON 输入仍须完整填写。
所有边界必须为正整数；超出硬上限的 profile 会在处理请求前拒绝。1 KiB 为 1024 字节，
1 MiB 为 1048576 字节。

| Profile limit | 默认值 | Profile 硬上限 | 作用 |
| --- | --- | --- | --- |
| `max_request_bytes` | 1 MiB | 4 MiB | 原逻辑请求规范字节与最终编码 body 分别受限 |
| `max_response_bytes` | 1 MiB | 4 MiB | 单个 HTTP 原始响应 body |
| `max_text_bytes` | 256 KiB | 4 MiB | 解码后的累计输出文本 |
| `max_tool_argument_bytes` | 256 KiB | 4 MiB | 所有返回工具参数字符串的累计字节 |
| `max_output_items` | 128 | 512 | output 数组，以及单个 message 的 content 数组 |
| `max_tool_calls` | 32 | 32 | 返回的工具调用数量；另受并行能力限制 |
| `max_json_depth` | 64 | 64 | 响应 JSON 与工具 arguments 的严格解析深度 |
| `max_timeout_ms` | 60000 | 300000 | 原请求 timeout 的允许上限 |
| `max_stream_bytes` | 4 MiB | 16 MiB | 提供给流回放层的 profile 总字节上限 |
| `max_sse_line_bytes` | 256 KiB | 1 MiB | 提供给流回放层的 profile 单行上限 |
| `max_sse_event_bytes` | 1 MiB | 4 MiB | 提供给流回放层的 profile 单事件上限 |
| `max_sse_events` | 4096 | 16384 | 提供给流回放层的 profile 事件数上限 |
| `max_sse_chunks` | 16384 | 65536 | 提供给流回放层的 profile 块数上限 |

HTTP codec 校验 timeout 数值，不等待时间经过，也不声称执行了网络 deadline。
SSE 层另有实际生效边界，不得用这里 profile 允许的较大数值推断 SSE 一定支持该规模。

## 原请求与 HTTP 编码

`encode_responses_request(profile, request, stream)` 首先调用原 `ModelRequest` 序列化
校验，并要求 `structured-v2`。`framed-v1` 不做猜测转换；FakeModel 请求也不会被自动
重标记。下列绑定必须同时匹配：

- `request.model.model == profile.model_ref`；
- `request.context.adapter_version == profile.adapter_version`；
- `request.context.protocol_version == profile.protocol`。

请求的 call/attempt ID 必须为最多 128 字节的非空可打印 ASCII token；当前声明及历史
工具名称限定为最多 64 字节的 ASCII 字母、数字、`_`、`-`。工具调用/结果配对、逻辑输入
与指纹一致性沿用原请求校验。`stream=true` 需要 streaming 能力；声明工具需要 tool
calling 能力。最大输出 token、输入/输出预留总和和 timeout 必须落在 profile 声明内。

编码不修改传入的 `ModelRequest`。原 call/attempt/role/index、完整 execution binding、
配置快照和 reservation 仍属于宿主请求；它们进入本地 evidence，不由供应商响应补写。

| structured-v2 内容 | Responses body 映射 |
| --- | --- |
| model 引用 | 取 profile 显式 `model`，不使用隐式模型名 |
| 命名 system 段、dependencies、工具版本 | 规范 JSON 字符串放入 `instructions`，格式标签为 `wls-responses-system-v1` |
| 完整 task contract、各消息的 source 引用 | 首个 user input 的规范 JSON 字符串，格式标签为 `wls-responses-task-v1` |
| user/assistant 文本 | 按原顺序写 role/content；仅有工具调用的空 assistant 文本不另造空消息 |
| assistant 工具调用 | 原生 `function_call`，保留 call_id/name，arguments 使用原对象的规范 JSON |
| tool 结果 | 原生 `function_call_output`，保留 call_id 和完整原输出字符串 |
| 当前工具 Schema | 原生 `function` tools；parameters 保持原 Schema，显式 `strict:false` |

body 还明确写入 `max_output_tokens`、`parallel_tool_calls`、`stream`，以及
`store:false`、`background:false`、`truncation:"disabled"`。不发送 `previous_response_id`，
不依赖供应商存储会话。工具 Schema 不为适配 strict 模式而增删属性、必填项或限制。
这些映射保存原信息，不构成真实模型对提示语理解质量的验收。

返回的 `EncodedResponsesRequest` 包含 `method="POST"`、拼接后的 URL、实际生成的 body
字符串、独立 body/profile/logical-request 指纹、stream 标志和本地 evidence。
`body` 是待发送描述，组件没有发送动作。

## HTTP 解码与明确拒绝

`decode_responses_response(profile, request, http_status, raw_body)` 使用同一原请求/profile
校验。HTTP 状态参数必须在 100–599 内；profile/request 无效、状态参数无效或原 body
超限会抛出 `Error`，不会先复制不受限输入。受限 body 内的 JSON/协议问题返回带原宿主
tuple 的 `error`/`interrupted` 响应，并保留原始字节供离线检查。

原 body 使用严格 JSON 解析：错误 UTF-8、重复键、尾随内容、非法数值或超深结构不能
成为有效完成响应。只有 HTTP 200 进入 Responses 终态解释。其他 HTTP 状态即使带有
看似成功的 output，也不产出 `completed`；可解析错误对象记录为 `http_<status>`，
连 JSON 都不合法时记录为 `invalid_response`。重定向、429 和 5xx 均没有自动重试。

200 响应必须具有 `object:"response"`、有效 provider response ID、与 profile 完全相等
的 model 和明确 status。provider ID 单独记录，绝不替换宿主 call/attempt ID。
网关若将输入 alias 自动改成其他返回 model 字符串，本版会以 `provider_model_mismatch`
拒绝，不能用现读模型名修补原请求。

| Provider status | 宿主结果 | 输出处理 |
| --- | --- | --- |
| `completed` | 满足全部输出校验后 `completed` | 返回支持的文本/工具；error 和 incomplete_details 若出现必须为 null |
| `failed` | `error` / `provider_failed` | error 必须为 object，不能为 null；不返回文本或工具 |
| `incomplete`、`cancelled` | `interrupted` / `response_<status>` | 保留原 body 和已解析的合法用量，不返回部分输出 |
| `queued`、`in_progress` | `interrupted` / `response_not_terminal` | 不把非终态解释为完成 |
| 未知 status | `error` / `unsupported_response` | 保留原 body，拒绝使用 |

### 完成输出子集

- `message` 必须有唯一 item ID、assistant role、completed status 和 content 数组。
  只接受 `output_text`，按 output/content 顺序拼接原文本，不自行插入分隔符。
  `annotations`、`logprobs` 可以省略或为空数组；非空或类型不符明确拒绝。
- `function_call` 必须有唯一 item ID 和 call_id，name 必须是原请求当前声明的工具。
  status 可省略，出现时必须为 completed。arguments 必须是完整、严格、无重复键的
  JSON object；调用数、累计参数字节和 parallel capability 均检查。这里只检查协议对象
  形状和声明工具名，实际参数 Schema 及工具执行权限仍由宿主验证。
- 已完成响应至少含非空文本或一个合法工具调用。空结果、重复 ID、未知工具、部分参数、
  非对象参数、output/item/part 内的未知字段，以及任何不支持的输出类型都使整个结果
  失败，清空已暂存的文本和工具。

reasoning item、refusal、图像/音频、hosted tools 及其他多模态/内置工具输出均未实现。
不会跳过这些输出再返回成功。原始 item 和其字段仍留在有界 raw body 中。

响应顶层使用明确白名单；已知的 envelope 元数据留在 raw body 中，不因此获得执行含义。
特别兼容官方示例的 `access_programs:null`、`reasoning_effort:null`、`input:[]`；
这三项若携带新内容则拒绝。`response.reasoning` 等已知 envelope 元数据不等于支持
reasoning 输出 item，也不证明供应商实际执行了请求的全部配置。

## 用量：已知、未知和不一致

`usage` 可缺失或为 null。下列数值必须为非负 int64 范围整数；缺失或 null 保持未知，
明确返回的 `0` 才保存为零。字符串、boolean、小数、负数、超范围整数和未知 usage
字段均拒绝，不能以类型转换制造已知值。

| Provider usage | `ModelUsage` / 处理 |
| --- | --- |
| `input_tokens` | `input_tokens` |
| `output_tokens` | `output_tokens` |
| `input_tokens_details.cached_tokens` | `cache_read_tokens` |
| `input_tokens_details.cache_write_tokens` | `cache_write_tokens` |
| `total_tokens` | 只作一致性检查；原值保留在 raw body |
| `output_tokens_details.reasoning_tokens` | 只作一致性检查；原值保留在 raw body |
| 费用 | `cost_micro_usd` 始终为 null，本协议子集没有可靠的该宿主货币单位来源 |

已知 input/output 分量不得超过已知 total；两者和 total 全部已知时，还必须满足精确求和，
并在求和前检查 int64 溢出。每个已知 cache 分量不得超过已知 input，reasoning 分量
不得超过已知 output；这些子分量也不得超过已知 total。只有一部分计量时不倒推出
其他字段，例如 output=3、total=12 仍保留 input=null；output=3、total=2 则拒绝。

用量作为整体解析成功后才写入 `ModelResponse`，非法 usage 不留下半套已知字段。
HTTP 解码已经取得的合法用量可在后续输出校验失败时保留；在 JSON、顶层字段或模型
身份等前置阶段失败而未取得用量时，仍保持未知。raw body 保留调用方实际提供的内容，
不按字节数估 token、不推价格、不把取消解释为免费，也不产生真实账本记账。

## 原始字节、身份与记录指纹

编码 evidence 标记 `evidence_kind:"locally_encoded_request"`；解码 evidence 标记
`evidence_kind:"offline_http_response_fixture"`。两者均保留原宿主 tuple、profile 与
逻辑请求指纹，并明确 `network_dispatched:false`、`credentials_resolved:false`、
`paid_attempts:0`、`business_accepted:false`、`tool_dispatch_performed:false`。
这些零值描述离线组件没有执行动作，不能被用作对未知真实请求费用的判断。

| 指纹 | 精确含义 |
| --- | --- |
| `profile_sha256` | `canonical_json(to_json(profile))`；绑定有效配置，非 profile 原文件字节 |
| `logical_request_sha256` | 原 `ModelRequest` 的逻辑请求指纹；不因协议编码被覆盖 |
| `body_sha256` | 本地生成 body 字符串的精确字节 SHA-256 |
| `raw_body_sha256` | 本地提供响应 body 的精确字节 SHA-256，包括空白、换行和错误字节 |
| `record_sha256` | 整个编码/解码 JSON 记录去掉此字段后的内部 canonical JSON SHA-256 |

Profile 中能力集合的顺序会规范化，因此只是改写文件空白或集合排列不必改变 profile
指纹。内部 `canonical_json` 递归排序对象键、保留数组顺序；不声称兼容 RFC 8785。
两份仅空白不同的响应可以解出同一 `ModelResponse`，同时具有不同 raw-body 指纹。

输出 struct 是公开的宿主值对象。codec 生成 `record_sha256`，`to_json()` 同时核验
body/raw 指纹及完整记录指纹，拒绝后续字段修改造成的自相矛盾报告。完整记录绑定
URL/method/stream、profile/request 指纹、响应状态与用量、原 tuple、provider ID 和
evidence；只改 body 并重算 body hash 也无法通过原 record hash 校验。

JSON 中 `record_integrity:"local_consistency_only"` 明确其用途：检测本地报告的意外
修改。这不是对恶意宿主的认证、数字签名、服务端证明或 Store 授权；能替换整套输入和
指纹的受信调用方并不因此获得新的业务权限。

`DecodedResponsesResponse::raw_body` 始终保留原 C++ 字节字符串。JSON 报告用
`raw_body_encoding:"hex"` 和 `raw_body_hex` 无损输出，错误 UTF-8 也不会替换或丢失。
生成 body 的字节指纹与原 `context.wire_sha256` / `wire_request_sha256` 分离；
这两个旧 transport 字段继续为 null，编码不伪造已发送收据。

## C++ 使用 API

下面函数接受调用方已经按大小上限读取的本地字节，不执行文件读取或网络请求。
`request_bytes` 必须是该 profile 对应的原 structured-v2 请求。异常交由宿主按原
错误策略处理，不通过修改 profile、tuple 或协议版本来重试不匹配请求。

```cpp
#include "wls/protocol_codec.hpp"

#include <string_view>

wls::s1::Json inspect_http_fixture(std::string_view profile_bytes,
                                  std::string_view request_bytes,
                                  int http_status,
                                  std::string_view response_bytes) {
    using namespace wls::s1;
    const auto profile = provider_profile_from_json(
        parse_json_strict(profile_bytes, 65536, 64));
    const auto request = model_request_from_json(
        parse_json_strict(request_bytes, profile.limits.max_request_bytes,
                          profile.limits.max_json_depth));

    const auto encoded = encode_responses_request(profile, request, false);
    const auto decoded = decode_responses_response(
        profile, request, http_status, response_bytes);

    // to_json checks the local record fingerprints. Nothing is dispatched.
    return boost::json::object{
        {"encoded_request", to_json(encoded)},
        {"decoded_response", to_json(decoded)}};
}
```

直接以 C++ 构造 `ProviderProfile` 时，可调用 `validate_provider_profile()` 做预检；
编码和解码也各自验证 profile。`to_json(profile)` 输出完整有效 profile，
`provider_profile_sha256(profile)` 返回其有效配置指纹。编码的 `stream=true` 只改变
body 声明并检查 streaming 能力，本函数本身仍不会收取或解析流。

## 合成 fixture 与后继接入

共享 fixture 定义在
[`tests/protocol_test_support.hpp`](../../tests/protocol_test_support.hpp)，命名空间为
`wls::test::protocol`。辅助函数 `profile()`、`context()`、`request()`、
`response_with_output()`、`text_response()`、`tool_response()` 专用于确定性离线样例。
提交的 HTTP 示例包括
[`request.json`](../../tests/fixtures/responses/request.json)、
[`http-response.json`](../../tests/fixtures/responses/http-response.json) 和
[`tool-response.json`](../../tests/fixtures/responses/tool-response.json)。

样例显式使用 `https://gateway.example.invalid`、`fixture-responses-model`、合成 ID 和
标明没有 Store admission 的配置哈希；`WLS_OFFLINE_FIXTURE_API_KEY` 只是未解析的名称。
请求从独立 structured-v2 fixture 构造，不能把真实 FakeModel Store 预约改标签后称为
Responses 调用证据。任何测试成功也只验证本地实现，不证明供应商成功率、成本、模型
任务质量或部署网络隔离。

后续接入真实运行器需要独立实现和验收实际 HTTPS/TLS、凭据解析、供应商能力核验、
实时取消/deadline、原 attempt 与 wire 持久化、迟到结算、预算告警及获准小任务。
本文件的接口和 profile 不解除这些门禁；缺失凭据或预算不触发付费调用。

## 离线 CLI 与输入文件

`encode-model` 和 `replay-model` 只依赖显式 profile、原请求和本地响应夹具，入口在
仓库配置、SQL 资产读取及任何 Store 构造之前分派。工作目录不需要包含 repo config、
prompts、schemas 或 SQL 文件；命令不打开数据库，不解析凭据，也不发送请求。
`--source-root` 和 `--db` 对这两个命令均为未知选项，不能通过静默忽略来改变输入来源。

```sh
wls encode-model --profile PROFILE --request REQUEST [--stream true|false]
wls replay-model --profile PROFILE --request REQUEST \
  --http-response BODY [--http-status 200]
wls replay-model --profile PROFILE --request REQUEST \
  --sse-stream STREAM [--chunk-bytes 4096]
wls replay-model --profile PROFILE --request REQUEST --replay REPLAY
```

`replay-model` 必须且只能选择 `--http-response`、`--sse-stream`、`--replay` 之一。
`--http-status` 仅适用于 HTTP body，范围为 100–599；`--chunk-bytes` 仅适用于原始 SSE
文件，范围为 1–65536，默认 4096。`encode-model --stream` 只接受 `true` 或 `false`，
默认 `false`。全部选项校验完成后才读取输入，未知选项不会触发配置或数据库读取。

`doctor`、`encode-model`、`replay-model` 在构造 Options 之前逐个校验命令后的 argv
参数，选项名和值都必须是有效 UTF-8，且每项不超过 4096 字节。协议命令的全部已选路径
在任何文件读取前还要经过 `input_path_details`：原路径及其绝对词法解释分别有 4096
字节上限，并验证可以进入 UTF-8 JSON 报告。这样，错误路径不会在错误报告序列化阶段
再次因无效 UTF-8 失败。这里的长度按字节而非字符计数。

文件通过有界普通文件读取器打开；逐级拒绝父目录符号链接，叶子也使用 no-follow 和
nonblocking 打开，再确认普通文件及大小。读取过程中继续执行字节上限，不接受文件
增长突破边界。profile 原文件上限为 64 KiB，请求原文件上限为 4 MiB，HTTP body 受
`profile.limits.max_response_bytes` 限制；SSE 文件和 replay JSON 的限制见下节。

外层 JSON 报告的 `format` 为 `wls-offline-protocol-report-v1`，`mode` 分别为
`encode_only`、`http_body_replay` 或 `sse_replay`，并包含 `encoded_request` 和 `inputs`；
回放还包含 `result`。路径信息的 `path_kind:"lexical_interpretation"` 明确表示词法解释，
不把该字符串当成实际文件身份。`inputs` 中的 `raw_sha256` 和 `bytes` 对应本次实际读到
的文件字节；HTTP status 另外标为 `origin:"local_fixture_parameter"`。

`network_requests:0`、`paid_attempts:0`、`tool_dispatches:0`、`credentials_resolved:false`
和 `store_opened:false` 描述该离线命令的动作。编码成功或回放 `completed` 返回退出码
0；生成了报告的 `error`/`interrupted` 回放返回 4。参数、配置和输入文件错误返回 2，
错误 JSON 写入 stderr；成功报告写入 stdout。外层报告序列化后另有 32 MiB 输出上限。

## R3.2 SSE 离线回放

### 范围、公共 API 与输入格式

R3.2 提供 OpenAI Responses 文本和函数调用事件的有界离线回放。公开接口位于
[`include/wls/sse_replay.hpp`](../../include/wls/sse_replay.hpp)，实现位于
[`src/sse_replay.cpp`](../../src/sse_replay.cpp)。回放接受调用方已经取得的输入字节，
不创建网络连接、不解析凭据、不创建工作线程、不等待真实时间、不重试，也不派发工具。
独立设计依据见 [ADR-078](../adr/078-responses-sse-replay.md)。

```cpp
ResponsesSseReplay replay_responses_sse(
    const ProviderProfile& profile,
    const ModelRequest& request,
    const std::vector<SseReplayChunk>& chunks,
    SseReplayLimits limits = {},
    std::stop_token stop = {});
```

每个 `SseReplayChunk` 包含 `bytes`、相对本次 attempt 起点的 `elapsed`，以及在读取
该块之前生效的 `cancel_before`。`elapsed` 为 `std::chrono::milliseconds`，必须非负且
不递减。这里的时间是调用方提供的离线观察值，不是本地等待指令。profile、请求、调用方
limits 或时间序列无效时抛出 `Error`；有效调用中的传输和协议失败返回带原 tuple 的
`ModelResponse`。不会通过修改 profile、tuple 或协议版本重试。

`--sse-stream` 读取原始字节文件，按 `--chunk-bytes` 切分，各块时间均为 0。
该模式可以保留并诊断无效 UTF-8 原始输入。`--replay` 读取严格 JSON，顶层只允许
`format` 和 `chunks`；每一块必须且只能提供 `data`、`elapsed_ms`、`cancel_before`。
下面的有效输入格式表达先读一条注释、随后取消，预期结果为 `interrupted`：

```json
{
  "format": "wls-sse-replay-v1",
  "chunks": [
    {
      "data": ": offline fixture\n\n",
      "elapsed_ms": 0,
      "cancel_before": false
    },
    {
      "data": "",
      "elapsed_ms": 1,
      "cancel_before": true
    }
  ]
}
```

`data` 是 JSON 字符串，`elapsed_ms` 是非负 int64 范围整数，`cancel_before` 是布尔值。
重复键、额外字段、错误字段类型和时间倒退均拒绝。JSON 夹具文件另有 8 MiB 上限和
16 层解析深度上限；解码后的块总字节数仍受流字节上限约束，CLI 在回放前校验整个块列表。
完整文本流和取消夹具分别见
[`text.sse`](../../tests/fixtures/responses/text.sse) 与
[`cancel.json`](../../tests/fixtures/responses/cancel.json)。

SSE 结果的 `evidence.format` 为 `wls-responses-sse-replay-v1`。这与输入 JSON 的
`wls-sse-replay-v1`、CLI 外层的 `wls-offline-protocol-report-v1` 分属不同记录层。

### SSE 字节封装

解析器支持 CR、LF、CRLF 换行，以及 CRLF、UTF-8 字符和起始 BOM 在不同块之间的拆分。
只移除流开始处的一个 UTF-8 BOM。多个 `data:` 行以换行连接；空行才会提交一个事件，
文件结束不会替代最后的事件分隔空行。

注释行不产生 Responses 事件。SSE `id` 作为独立的传输元数据保留，长度不超过 256
字节；含 NUL 的 `id` 字段忽略。`retry` 和未知 SSE 行字段会计数并忽略，不会触发重试。
未知 Responses JSON 事件类型或已知事件的未知 JSON 字段则明确拒绝。SSE `event` 字段
可以省略、为 `message` 或与 JSON `type` 完全相同；其他不一致值拒绝。

有效 UTF-8 可以跨任意读取边界拆分。无效 UTF-8 会拒绝，不进行替换解码；已经消费的
原始字节仍在报告中无损保存。终态后的新 data 事件、重复终态和未完成尾部都会使回放
结果不可用。注释不构成新的 data 事件；最终未换行的注释仍会接受 UTF-8 和容量校验。

### 支持的 13 种事件

这是明确限定的事件子集，不代表完整 Responses 流式协议实现。

| 事件类型 | 本阶段约束 |
| --- | --- |
| `response.created` | 必须首先出现且只能出现一次；响应状态为 `in_progress`，初始 output 为空 |
| `response.in_progress` | 可省略；最多一次，必须在输出项之前，响应 ID 和 model 与 created 一致 |
| `response.output_item.added`、`response.output_item.done` | 按 output index 建立并完成文本消息或函数调用；done 快照必须与已观察内容一致 |
| `response.content_part.added`、`response.content_part.done` | 只支持消息中的 `output_text`；content index 连续建立，done 必须与文本结果一致 |
| `response.output_text.delta`、`response.output_text.done` | 累积文本，done 文本必须与所有 delta 的拼接逐字节一致 |
| `response.function_call_arguments.delta`、`response.function_call_arguments.done` | 累积参数；done 参数必须与 delta 拼接一致，并且是严格 JSON object |
| `response.completed`、`response.failed`、`response.incomplete` | 终态事件类型、响应 status、响应身份和全部输出快照必须一致；随后复用 R3.1 HTTP 响应解码器 |

只支持 `message/output_text` 和 `function_call` 输出。reasoning 输出、refusal、多模态
内容、托管工具和其他事件类型均明确拒绝。`annotations`、`logprobs` 可缺省或为空数组，
非空值不在本阶段支持范围内。完成的 message 必须声明 `status:"completed"`；
function_call 的 status 可缺省，出现时必须与生命周期一致，完成时只能为 `completed`。

`response.output_text.delta` 和 `response.function_call_arguments.delta` 另允许可选
`obfuscation` 字符串。这是 Responses 默认流配置可能携带的传输填充，不是模型文本或
函数参数：它保留在原始流中，仍计入单行、事件封装和总输入字节限制，但不追加到输出、
不占用语义文本或参数容量。null、boolean、number、array、object 均拒绝；其他事件上
出现该字段仍拒绝，其他未知字段的拒绝规则也保持。编码器不关闭供应商默认填充来回避
解析兼容性检查；此项支持不改变离线、零凭据解析及零工具派发边界。

`response.completed` 要求所有已建立的输出项完成；文本还需完成 text 和 content
生命周期，函数调用还需完成参数生命周期。`failed` 和 `incomplete` 可以带有部分输出，
但终态快照仍必须与此前实际观察到的部分内容一致，不能用终态补入未观察的内容。

### 身份、序号与原始请求绑定

首个 `sequence_number` 必须是 0 或 1，后续严格递增 1。负数、非整数、超出 int64 范围、
重复、跳号和乱序均拒绝。`output_index`、`content_index` 从 0 连续建立；item ID 和
provider function call ID 不得重复，后续事件必须引用对应 index 已建立的身份。

以下四类事件允许可选 `response_id`：

- `response.output_item.added`；
- `response.output_item.done`；
- `response.function_call_arguments.delta`；
- `response.function_call_arguments.done`。

出现时，它必须是不超过 128 字节的非空可打印 ASCII token，并且等于
`response.created.response.id`。该兼容字段不会放宽序号或其他生命周期校验。

SSE `id`、provider response ID、function call ID 与宿主的 attempt 身份分别记录。
任何终态、取消、断流或协议错误都保留请求中的 `call_id`、`attempt_id`、`role`、
`role_call_index`、完整 `binding` 和 `config_snapshot_id`，不会创建新的 attempt，
也不会将迟到事件解释为另一轮成功。

### Deadline、取消与终态采用

每一块在读取之前检查取消和 deadline；`elapsed_ms >= request.timeout_ms` 即为超时
边界。这里的 `timeout_ms` 是原请求 JSON 字段，C++ 对应 `request.timeout`。同一块同时
声明取消并达到 deadline 时，取消检查先执行。`cancel_before:true` 可以出现在空块上，
用于确定性表达中途取消。`stop_token` 也在读取前、字节解析过程中和完成前检查。

所有时间判断均来自原始请求 timeout 和记录的 elapsed，不读取墙钟、不 sleep、不创建
自动重试。`evidence.timing_source` 为 `caller_supplied_elapsed_ms`，`deadline_ms`
记录原请求期限；`elapsed_ms` 记录最后检查到的块时间，包括可能在读取前被拒绝的块。

只有完整终态完成生命周期一致性校验、经过共享 HTTP 解码器，并且全部后续读取和尾部
检查通过后，才可能得到可用的 `status:"completed"` 响应。缺失终态、截断事件、取消
和超时返回 `interrupted`；协议不一致、未知事件、超限和终态后 data 返回不可用的
`error`。两类失败结果均清空 text 和 tool_calls。CLI 在读取/拆分输入阶段拒绝的超限
属于输入错误，不会伪造一个已经执行过回放的结果。

`stream_complete` 描述回放的读取、封装与终态处理是否完整；`response_usable` 还要求
共享解码器给出 `completed`。完整收到 `failed`、`incomplete`，或者共享解码器返回错误
结果的终态，不能通过 `stream_complete` 推导为业务成功。

即使 `response_usable:true`，也只表示该离线协议子集产生了完整响应。宿主仍须执行当前
Store tuple、工具参数 Schema 和权限检查。SSE evidence 始终声明 `business_accepted:false`、
`network_requests:0`、`retry_attempts:0`、`tools_dispatched:0`，回放过程不派发工具。

### Usage 保留规则

计量字段复用共享 HTTP 解码器的校验规则。缺失值保持 null，不以 0 代替；
`cost_micro_usd` 不从离线样本推导价格或账单。完整终态到达并通过绑定及共享解码器
验证后，已经确认的 usage 会立即保存。此后即使出现额外 data、未完成尾部、取消或
deadline，响应仍为不可用的 `error`/`interrupted`，text 和 tools 仍为空，但已经确认
的计量不会被抹除。

后续流错误的计量恢复只采用共享解码器认可的 completed、failed 或 incomplete 终态；
任意错误 payload 不会成为该恢复路径的计量来源。`terminal_usage_validated` 表示已通过
这一步校验，不代表每一个计量字段都非空；`terminal_usage_preserved_after_stream_error`
标记后续流错误中执行了保留。共享解码器直接返回结果时的计量语义仍见上文 HTTP 用量契约。

### 原始输入与消费边界

`ResponsesSseReplay::raw_stream` 保存回放已经消费的完整块，包含 BOM、原始换行、注释
和传输字段。一个块进入解析器之前会先整体保留，因此：

- `retained_stream_bytes` 是已经消费的完整块字节数；
- `parsed_stream_bytes` 是解析器实际遍历到的字节数；
- 若解析在块中途失败，前者可以大于后者；
- 因取消、deadline 或总字节上限在读取前被拒绝的块，不计入 retained raw；
- `input_chunks` 是提供的块数，`consumed_chunks` 是实际消费的前缀块数。

`observed_obfuscation_fields` 记录已通过字段类型检查的 delta 填充字段数，
`observed_obfuscation_bytes` 记录其解码后 UTF-8 字符串的累计字节数；缺省时二者为零。
后者不计 JSON 字段名、引号或转义字节，不替代原始流的字节预算，也不表示所在事件已
通过后续生命周期检查。填充的原始拼写和位置仍由 retained raw 及其摘要保留。

CLI 为文件读取记录的 `inputs.*.raw_sha256` 覆盖整个输入文件。对于 timed replay，它
覆盖原始 JSON 夹具；SSE 结果中的 `raw_stream_sha256` 则覆盖实际消费的流字节。取消或
拒绝之后，这两个范围可以不同。共享 HTTP 解码器接收的是终态 response 对象的规范 JSON，
`terminal_codec` 中对应的原 body 指纹也不能代替精确 SSE 字节指纹。

`partial_output_observations` 提供已观察内容的身份、长度、摘要与完成标志；它不提供
可派发的部分工具调用。`terminal_response` 保留观察到的终态快照，但该字段单独存在
不代表终态已被采用。JSON 中的 `raw_stream_base64` 无损保存原始流，包含错误 UTF-8；
`raw_stream_sha256` 对应解码后的原始字节，而非 base64 文本。

### 有界处理

有效限制取调用方 `SseReplayLimits` 与 provider profile 相应限制的较小值。调用方
limits 必须为正，且只能缩小实现硬上限；profile 允许更大的值不会扩大下表边界。
`max_content_parts` 只有 SSE 本地限制，没有同名 profile 字段。

| 资源 / `SseReplayLimits` 字段 | 实现硬上限 |
| --- | ---: |
| 原始流总字节数 / `max_total_bytes` | 4 MiB |
| 单行字节数 / `max_line_bytes` | 256 KiB |
| 单个 SSE 事件封装 / `max_event_bytes` | 1 MiB |
| data 事件数 / `max_events` | 4096 |
| 读取块数 / `max_chunks` | 16384 |
| 输出项数 / `max_output_items` | 128 |
| 函数调用数 / `max_tool_calls` | 32 |
| 单个消息的内容分段数 / `max_content_parts` | 32 |
| 所有输出累计文本 / `max_text_bytes` | 256 KiB |
| 所有函数调用累计参数 / `max_tool_argument_bytes` | 256 KiB |

事件封装计数包含字段行和分隔符，并对每个换行按两个字节保守计费。文本和参数按字节
计数；参数上限在全部函数调用之间共享。终态 response 规范 JSON 还受 profile 的
`max_response_bytes` 约束，JSON 解析受 profile 的 `max_json_depth` 约束。

### 本地记录一致性

`to_json(replay)` 输出以下字段；SSE 使用 `record_kind`，HTTP 编码/解码记录使用上文
的 `record_integrity`，两个字段名不互换：

```text
record_kind = "local_consistency_only"
response
evidence
raw_stream_base64
raw_stream_sha256
terminal_response
record_sha256
```

`record_sha256` 是除自身之外上述完整记录的内部 canonical JSON 摘要。序列化前会重新
计算并核对；响应、宿主 tuple、usage、证据、原始流或终态快照被修改而摘要未同步时，
拒绝输出不一致记录。仅修改 raw 并更新其局部 hash，仍不能通过原完整记录摘要校验。

该摘要用于本地记录一致性检查，不是提供方签名、网络执行凭证、账单凭证或宿主授权。
它覆盖嵌入的 SSE 记录，不覆盖 CLI 外层 `inputs`、`encoded_request` 或整个原始 timed
replay JSON。网络与计费事实仍明确标记为未经离线回放验证。

回归定义见 [`tests/sse_replay_tests.cpp`](../../tests/sse_replay_tests.cpp) 和
[`tests/test_protocol_replay_cli.py`](../../tests/test_protocol_replay_cli.py)。其中包括遍历
每个两块拆分位置、逐字节读取、生命周期和身份不一致、确定性取消/deadline、终态后
异常的计量保留、默认 delta 填充的类型与容量边界，以及本地记录修改拒绝；实际执行
结果以版本验收报告为准。

## 协议依据

官方协议核验范围与日期见 [ADR-077](../adr/077-responses-offline-codec.md)。HTTP item
映射和非严格函数定义依据
[Responses 迁移指南](https://developers.openai.com/api/docs/guides/migrate-to-responses) 与
[Function calling](https://developers.openai.com/api/docs/guides/function-calling)；
终态 envelope 和 usage 示例依据
[Responses streaming events](https://developers.openai.com/api/reference/resources/responses/streaming-events)。
完整 create API 参考页受到抓取大小限制，未据此声称支持其全部字段或所有模型能力。
2026-10-26 独立审核另核对官方
[Python SDK 的 Responses 创建参数定义](https://raw.githubusercontent.com/openai/openai-python/main/src/openai/types/responses/response_create_params.py)：
`stream_options.include_obfuscation` 的说明明确 delta 填充默认开启，因此在已有两类
delta 中显式识别该字符串。该字段兼容依据不等于供应商现场能力或所有事件类型已验收。
SSE 行封装参考 [WHATWG Server-sent events](https://html.spec.whatwg.org/multipage/server-sent-events.html#parsing-an-event-stream)；
本实现对无效 UTF-8 明确拒绝，且不实现浏览器的重连行为。
