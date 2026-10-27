# R3.3a：有界 HTTPS 传输与 R3.3b 先行接口契约

蓝图 v0.4.1；决定见 [ADR-080](../adr/080-bounded-https-transport.md)。
接续 [R3.1/R3.2 离线协议](R3-offline-responses.md)及
[R3.3a/b/c 计划](R3-controlled-live-plan.md)。本文件区分传输组件与尚未实现的
Store 集成；实施状态见 [STATUS](../STATUS.md)，执行成绩以本批报告和对应提交 CI 为准。

## 1. 本次范围

R3.3a 提供库级 HTTPS 传输：显式已校验请求、显式测试信任材料、实时取消、
共享 deadline、有界响应观察，以及原 HTTP/SSE 解码器之间的接合。测试仅访问
自建 loopback TLS 服务，使用临时证书和合成 bearer；不解析真实凭据环境变量。

本组件不持有 Store、不创建 attempt、不预留或结算预算、不执行工具，也不决定重试。
`wls` 和 `OfflineRuntime` 仍使用 FakeModel；离线 profile 仍要求
`real_calls_enabled=false`、`max_paid_retries=0`。本地测试发生网络连接不代表
真实模型调用准入已经存在，不将 FakeModel reservation 重标记为真实发送证据。

本文件第 6–8 节是 R3.3b 必须遵守的设计，并不表示本节点新增了真实 Store 方法、
SQL 迁移或已通过对账集成测试。仅修改这个设计也不能解除 fake-only 边界。

### 公开库接口

接口位于 [`include/wls/https_transport.hpp`](../../include/wls/https_transport.hpp)，
命名空间为 `wls::s1`：

```cpp
HttpsTransportResult perform_responses_https(
    const ProviderProfile& profile,
    const ModelRequest& request,
    const EncodedResponsesRequest& encoded,
    const HttpsTransportOptions& options,
    std::string_view bearer_token,
    std::stop_token stop = {});

Json https_transport_summary(const HttpsTransportResult& result);
```

调用方同时给出原 `ModelRequest` 与 `EncodedResponsesRequest`，组件重新核验二者与
profile 的一致性；不能修改 URL、body 或本地 hash 后沿用原请求身份。无效输入在网络
开始前抛出 `Error`；传输/协议失败保留原 response tuple，返回无可用文本/工具的结果。

该受信宿主库 API 技术上可以接收通过下述解析器门禁的 HTTPS 目的地址，**不是 loopback
访问控制器**。本批执行只使用 loopback；将库入口用于真实供应商之前必须实现第 6–8 节
的 Store 路径与调用授权，不能把测试范围误写成库内网络隔离或真实准入。

| `HttpsTransportOptions` | 默认值/含义 |
| --- | --- |
| `ca_bundle_pem` | 显式 CA PEM 字节；不是文件路径，也不从环境或 profile 凭据引用读取 |
| `resolved_ipv4` | 可选严格数字 IPv4；仅映射 URL 的精确 host:port，不改 URL/SNI/主机名验证 |
| `max_header_bytes` | 累计保留 header 预算，默认 64 KiB |
| `max_header_line_bytes` | 单行 header 预算，默认 8 KiB |
| `max_total_response_bytes` | 原 header/body 累计预算，默认 5 MiB；不替代 profile 的单项限制 |
| `poll_interval` | multi 等待粒度，默认 25 ms；不能超过本组件允许的短轮询上限 |

`HttpsTransportResult` 分开提供 `status` 与最后观察到的 `phase`。终态为
Completed、HttpError、Cancelled、DeadlineExceeded、CertificateError、ConnectionError、
TransferError、LimitExceeded 或 ProtocolError；阶段为 NotStarted、Connecting、TlsHandshake、
RequestWrite、ResponseHeaders 或 ResponseBody，不根据成功/失败码重新推造经过的阶段。

结果记录原编码/profile/逻辑请求/CA hash 与 `transport_options_sha256`、HTTP 状态、
网络是否启动、传输是否完整、
原字节是否截断、`upload_may_have_occurred`、header/body 观察计数、提供给 libcurl 的
body 字节与 libcurl 报告的上传字节，另有实际 elapsed 和可空的连接/TLS/首头/首体时间。
缺失时间保持 `null`，不补零。`record_sha256` 用于有界结果的一致性，不是签名回执。
连接/TLS 时间是执行器确认阶段已完成的观测时间，不是网络抓包得出的精确发生瞬间；
同一轮首次同时确认 TCP 与 TLS 时可使用相同上界时间，不能出现 TLS 先于已记录的 TCP。

`raw_headers`、`raw_body`、`response` 和 `protocol_evidence` 是私有内容；
`https_transport_summary` 只输出身份、有界宿主代码、计数、时间和 hash，排除原字节、
模型文本/工具、供应商诊断和 bearer。它不自动公开整个结果对象。

## 2. 系统依赖与网络边界

采用系统 **libcurl >= 7.88** 的 multi 接口。构建检查版本，运行时检查实际加载的
版本及 SSL、ASYNCHDNS 能力；缺失时在发送前拒绝。原 OpenSSL Crypto 依赖继续用于
项目已有摘要等功能，TLS/HTTP framing 由 libcurl 及其系统 TLS 后端负责。
系统库版本与后端写入实际验证报告，不从编译头文件版本推导运行库能力。
这项出站组件选型按 ADR-080 聚焦调整蓝图默认的 Asio/Beast 网络实现；
未来 `wlsd` 调度与服务化目标继续保留，不在本节点替换。

**解析器门禁单独检查。** `CURL_VERSION_ASYNCHDNS` 可能表示 threaded resolver；
官方已记录其 remove/cleanup 在等待解析线程时阻塞的情况，单有这个标志不保证 deadline
内返回。本节点允许严格数字 IPv4 直连，或使用显式 `resolved_ipv4` 通过 `CURLOPT_RESOLVE`
固定 URL 精确 host:port 的地址；其他 DNS 名必须确认实际运行库使用 c-ares。没有映射的
threaded DNS 名在网络开始前拒绝，不发起解析。IPv6 仍在原 profile 语法之外。
映射属于受信宿主配置，和 CA/容量等一起进入传输选项 hash；它不改变主机身份或授予
目的地址权限，也不会自动解析或刷新。外部 c-ares DNS 成败与取消需另列现场证据，本地
数字 IP/固定映射夹具不证明该部署路径已经验收。

| 约束 | 实现需要保证的行为 |
| --- | --- |
| 目标协议 | HTTPS-only；沿用 profile 的严格 URL 语法，无隐式默认端点 |
| TLS | 证书链验证与主机名验证同时开启；错误 CA、错误主机名均拒绝 |
| 测试信任 | 临时 CA 通过显式选项传入；不以关闭验证接受自签证书 |
| 代理 | 显式空代理配置；`HTTP_PROXY`、`HTTPS_PROXY`、`ALL_PROXY` 等不能改变目的路径 |
| 重定向 | 不跟随；3xx 留作原 HTTP 错误响应，不能向 Location 再发送 bearer |
| 隐式凭据 | 禁用 netrc、URL userinfo 和认证协商；仅使用宿主明确给出的合成测试 bearer |
| TLS key log | libcurl 初始化与联网前拒绝非空 `SSLKEYLOGFILE`；固定错误且不改写宿主环境 |
| 重发 | 一个调用只进行一次传输；429、5xx、超时、断流不创建新尝试或再发请求 |
| 输出 | 不输出 libcurl verbose/debug trace，不保存 outgoing Authorization 头 |
| 生命周期 | 每个调用自有句柄和回调状态；所有退出路径移除并销毁句柄，不遗留活动连接 |

CA 通过 `CURLOPT_CAINFO_BLOB` 提供，清空默认 CA 目录；信任材料 hash 单独记录。
HTTP 使用受限的单次传输，prerequisite 回调在连接及 TLS 完成、发送 HTTP 请求之前检查
取消和发送次数，第二次进入必须拒绝，不能借认证/重定向/内部重绕发送同一调用。
上传使用 16 KiB 的 libcurl 缓冲，每个 socket 请求 64 KiB 的 `SO_SNDBUF`。
这是所有调用共用的有界发送策略；内核可能调整实际缓冲值，不代表远端接收窗口大小，
也不能以提供给上传缓冲的字节数代替实际上传或远端收取字节。

不使用代理、重定向或其他协议去回退一个失败端点。未来若需开放这些能力，必须另行
冻结目的地址、认证作用域和政策，并独立测试；不能通过修改环境变量悄悄改变已批准请求。

## 3. 一个时间预算与取消

入口建立一次 `steady_clock` 起点与绝对 deadline，预算来自已经校验的请求 timeout。
配置、连接、TLS、请求上传、响应头和响应体等待消耗同一份预算，不为各阶段重新计时。
libcurl 超时同时设为剩余预算，外层 multi 循环仍在 perform/poll 前后检查时间与 stop token。

每次 `curl_multi_poll` 等待上限为 `min(poll_interval, ceil_ms(remaining))`，
其中 `poll_interval` 不超过 25 ms；毫秒取整最多向上增加不足 1 ms，不为下一阶段
重新增加预算。progress、header 和 body 回调也检查中断。不能只依赖
progress 回调，因为官方文档明确其空闲期调用可能降到约每秒一次。

取消/到期已被本调用观察到时，结果不能变回 Completed。只观察到协议终态但 HTTP
传输尚未正常结束时，后续取消、断流或非法尾部仍否定采用。完成和取消竞争按实际
观测顺序记录，不以事后已停止的 token 伪造之前的时间事实。

25 ms 描述事件循环请求的等待粒度，不承诺操作系统、TLS 后端或调度暂停的硬实时上限。
验证报告应保留实际 elapsed、取消请求到返回的范围及失败阶段；不能把 fixture 人工
填写的 elapsed 当执行时钟。异步 DNS 能力及数字地址/显式映射/c-ares 门禁共同避免
标准同步或 threaded 清理等待路径；本地 loopback 夹具不充当外部 DNS 故障验收。

## 4. 原观察、容量和协议采用

### 4.1 字节的来源与完整性

保存的响应 body 是 libcurl 交付到 HTTP body 回调的字节。HTTP chunk framing 可以
由 libcurl 解开；因此这份 body 不是完整 TCP/TLS 抓包，也不包含所有线上 framing 字节。
不得将它命名为完整 wire transcript。header 回调可能包含状态行、临时响应及 trailer，
其预算必须累计，不能每出现一个响应块就清零。

每个缓冲区在复制前检查剩余容量，并使用无溢出的长度运算。超限时保留限额内的原前缀、
观察到的范围和截断事实，然后中断；不能在报告中声称完整消息 hash。回调块的数量也有
上限，防止大量极小块绕过字节配额耗尽内存。原始字节保留和解码输出分别受限。

| 观察 | 可以说明 | 不能说明 |
| --- | --- | --- |
| 规范化 profile / 原请求 hash | 本地使用的配置和逻辑输入身份 | 供应商已接受这些配置 |
| 编码 body hash | 待发送 body 的精确字节 | body 已经发出或收到 |
| 连接/TLS 时间或状态观察 | 本地传输库报告完成的相应阶段 | 供应商完成了模型处理 |
| 上传计数 | libcurl 在该次传输中的本地上传观察 | 供应商收到、只执行一次或收费为零 |
| header/body 回调 | 实际交付给宿主的应用层字节与保留范围 | 未保留或尚未到达字节不存在 |
| HTTP 正常结束 | 本次 HTTP 消息完成且无已观察的传输错误 | Responses 内容合法或工具获准 |
| 严格解码 Completed | 当前支持子集中的响应完整合法 | 当前 Store 绑定仍有效或业务已验收 |

没有观察到某阶段应保持未知或明确未观察；不得把 getinfo 的默认零计时解释成零发送、
TLS 未发生或不计费证明。时钟、字节范围、截断和供应商 response ID 各保留原语义。

### 4.2 HTTP/SSE 组合

传输成功只提供一个解码前提。非流式继续使用原 `decode_responses_response`；
流式使用实际接收块及其单调时间，进入原 `replay_responses_sse` 的有界严格检查。
原回放 evidence 仍表示回放检查，不覆盖独立的实际传输 evidence。

只有传输正常结束、全部保留容量未超限、HTTP 响应符合要求以及整个协议解码成功后，
才可返回可供后续宿主检验的 Completed。任何错误/中断返回中不得含可派发的部分工具。
响应为 200、看到 `response.completed` 或收到完整一个 function call，均不足以提前采用。
有效终态后的多余事件或非法数据仍交给原解码器拒绝。

原 SSE 每行、每事件、事件数、块数、输出文本、工具参数和原字节上限继续生效。
实际 SSE 限额取 profile 和原回放硬上限的交集；合法的较宽 profile 不应导致小型合法
响应在已发送后因回放配置本身越界而失败。接收 body/块计数也使用对应的有效上限。
对于 Content-Encoding 或其他当前未声明的转换，不得静默改变原响应后再声称字节一致。
传输故障后的原 usage 仍须按已得到的可靠协议证据解释；不依据 body 大小估 token，
不将错误、取消或未知映射为费用零。

### 4.3 凭据与报告

bearer 只作为当次请求的秘密输入，在构造 HTTP 头之前验证长度和字符边界，拒绝
CR/LF 等头注入。公开编码请求、profile 快照、日志和错误记录只包含认证引用名称，
不能出现实际值。不开启默认 verbose，不记录 outgoing headers 或 debug 文本。

libcurl 的 `SSLKEYLOGFILE` 环境变量可以单独启用 TLS secrets 落盘，因此组件在其
初始化和联网前拒绝非空设置，不打印变量值、不改写或清除宿主环境。测试须观察零连接
接受且 key-log 文件未被创建。可信宿主不能在检查后并发修改环境，也不能由进程内
其他组件提前启用或缓存 key-log handler；本组件不声称隔离任意同进程代码或 TLS 后端。

原 response 本身可能携带供应商回显的敏感内容。原证据应作为私有内容保存或交给
明确接收者，安全摘要仅显示有界状态、计数、身份和 hash；不能因为是响应就自动公开。
合成凭据测试可以检查安全摘要不含 token，但不能据此宣称任意供应商响应都无敏感内容。

## 5. 本地反例与完成定义

本地服务必须由测试自行创建，绑定数字 loopback 的临时端口，使用自建临时 CA 和证书。
错误证书与主机名用例不能通过改变生产验证开关实现。服务端保留合成请求次数/收取字节
作为测试断言；这些服务端夹具记录与客户端观察分开，不伪装成供应商收据。

| 类别 | 必须验证的反例 |
| --- | --- |
| TLS | 正确信任/主机成功；不可信 CA、错误主机名、握手停滞拒绝或到期 |
| 环境 | 恶意代理环境不改变目的地；重定向目标收到零请求；合成 bearer 不进入安全日志；threaded DNS 无映射拒绝，显式映射保留证书主机检查 |
| HTTP | 成功 JSON、HTTP 错误、错误 framing、响应头超限、body 超限 |
| SSE | Unicode 跨块、事件跨块、分块编码、半个字符、未完成终态、完整终态后非法尾部 |
| 中断 | 发送前取消、握手/等待头/等待体取消、共同 deadline、取消与完成竞争 |
| 不确定发送 | 请求只收取部分 body、已上传后断流；保留阶段和不确定性，不自动重发 |
| 资源 | 回调失败/超限/取消后连接关闭；重复故障不会累积活动句柄；块数与累计容量守卫 |
| 兼容 | 原 codec/SSE/CLI 仍离线，原 Store 仍拒绝真实 adapter，原账本和工具回归通过 |

以上为要求，只有实际执行的项目才写入本批报告。报告需给出源码输入身份、库版本/后端、
普通用户身份、调用 umask、命令、退出码和失败记录，并核对同一源码的既有完整 CI。
合成 TLS 服务成功只验收本地传输组件，不等于供应商支持、真实费用或模型质量验收。

## 6. R3.3b 先行类型边界（尚未实现）

下表名称是语义分层，**不是本次声明的 C++/SQL API 名称**。实现前须映射到现有
`AttemptReservation`、`reserve_attempt`、`settle_attempt` 及新增结构，并单独评审迁移。
不能为了复用现有接口把真实发送装进 `adapter_kind="fake"` 或改写原假调用。

| 概念记录 | 必须绑定 | 权限/可变性 |
| --- | --- | --- |
| FrozenProviderProfile | 配置规范 hash、协议/映射版本、模型、URL、认证引用名称、能力依据、容量/时限、预算/重试策略 | 请求冻结；不含真实凭据；旧快照不更新 |
| ReservedAttempt | 原 call ID、独立 attempt ID、完整 request envelope、request/plan/task/instruction binding、角色索引、代次、profile hash、逐维度预留 | 发送前持久化；不能重标记旧 fake attempt |
| SingleSendClaim | 原 attempt、编码请求 hash、完整原绑定、发送权 ID、领取代次和当时政策 | 一次性 durable CAS；精确重放只读回原领取结果，不能获得第二次发送许可 |
| TransportObservation | 原 attempt/claim、阶段、时间范围、上传观察、原响应范围/截断、解码前后独立指纹 | 追加原事实；不推断供应商收到或计费；不改原请求身份 |
| OriginalSettlement | 原 attempt、原响应状态/原 usage/null、结算指纹、当时业务可采用性及原因 | 不可变；现有精确重放含义保持 |
| UsageReconciliation | 原 attempt/settlement hash、独立核对 ID、幂等键、预期投影版本、可信来源/证据 hash、逐维度已知/未知值 | 追加事实；来源由宿主验证，不接受模型声明作为凭证 |
| LedgerProjection | 原值、已确认值、逐维度预留与差额、核对版本、未决/矛盾原因、超预算状态 | 单独 CAS 投影；不能覆盖原结算或改变业务资格 |

类型设计必须使以下事实无法混淆：已编码、获准尝试、已领取发送权、本地观察到上传、
供应商返回 response ID、已取得有效 usage、账本已核对和业务可采用。布尔值
`success` 或一个可修改的响应对象不能替代这些记录。

## 7. R3.3b 事务次序与崩溃语义（尚未实现）

1. **准入检查。** 在任何真实凭据解析或发送前，核验冻结 profile、当前可撤销政策、
   完整版本/角色/权限、能力和非空预算。原业务 readiness 与控制/对账许可保持区分。
2. **预约事务。** 原 envelope、独立 attempt 与逐维度预留一并提交；失败则零发送。
   请求/预算/CAS 变化造成失败时不能沿用已失效的预检查结果。
3. **发送权事务。** 再核原绑定和政策，以 durable CAS 获取唯一发送权。事务提交之前
   不连接发送；事务之外执行网络。重复领取、重开恢复和旧 epoch 都不能重新授予同一权利。
4. **实际传输。** 消费原编码请求和显式秘密输入，保存带原 attempt 的观察。
   内存里的“还没写”不能抵消已经落盘的领取事实；进程崩溃后的缺失记录保留未知。
5. **原结算事务。** 保存原响应/原观察与原 usage，保持 immutable fingerprint。
   当前绑定全部仍有效才可返回业务可采用；stale、AMEND、取消后的响应也保存原账目。
   传输/解码/持久化未决时，不继续派发相关工具或新付费业务。
6. **追加对账事务。** 验证原身份、可信证据、幂等键和预期投影版本，原子追加核对事件
   并更新账本投影。任一步失败整体回滚；新版本冲突后必须重新读取，不盲目覆盖。

| 崩溃/中断位置 | 恢复所需行为 |
| --- | --- |
| 预约事务前或回滚 | 不存在可消费发送权，零发送 |
| 预约后、领取前 | 保留原预约；只有当前准入仍成立才可能领取，不能另造同 ID attempt |
| 领取提交后、网络前 | 旧权利不可重新领取；没有响应不证明零发送 |
| 上传中/响应中 | 保留已持久化观察及未知预留；不得自动重发 |
| 收到响应、原结算保存前 | 禁止业务采用；只可核对原事实，不能从最新状态重造原响应 |
| 原结算提交后 | 精确重放返回原结算；重启不重复释放预留 |
| 追加核对事务内 | 追加事实与投影同成同败；幂等重放不重复计算差额 |

上述是发送权的至多一次宿主派发契约，不声称供应商端到端 exactly-once。
后续如采用供应商幂等键，其适用端点、有效期和保证必须另有可验证依据。

## 8. R3.3b 追加对账与旧库兼容（尚未实现）

### 8.1 逐维度与守恒

输入 token、输出 token、缓存子量、实际费用分别保留已知/未知。不能用 total 推测缺失
分量，不能把估算价写为实际费用，也不能因模型失败填零。核对只接受来源明确且能绑定
原 attempt 的新证据；币种、单位和转换规则必须明确，不能从模型名称推价格。

核对记录同时保存原值、之前投影值、新确认值和账本差额。仅新确认的维度释放其对应
预留并增加已确认消费；其他维度仍未知。计数和金额均检查整数溢出，attempt 次数不因
对账恢复。新的可信数值超出原预留时，完整保存真实支出并告警，停止新的付费准入；
不能截短支出让预算表面满足上限。此时可用余额可以明确显示超支，不能伪称仍守在额度内。

守恒检查从原预约、原结算和全部核对事件重建每一维度，验证投影一致及未知预留未丢失。
已确认值遇到矛盾证据时保留双方来源并进入待核对，不静默替换前一份事实；矛盾的解决
仍需独立的有依据决定，不能通过幂等键换名绕过冲突。

同一个核对幂等键、同一完整内容返回原结果；同键不同证据/原结算/值必须冲突。
不同键也必须受原 attempt、核对版本和可信证据身份约束，不能重复释放同一维度预留。
查询同时返回原 settlement 和当前 reconciliation 状态，不能只显示更新后的总数。

### 8.2 权限与兼容

对账是受信宿主的会计操作，不能成为模型工具或业务权限补丁。终态请求可以核对原
attempt，但不能恢复 Worker 执行。原 UNKNOWN、AMEND、取消和 stale 结果继续保持；
已有包、回执、经验来源、工具执行记录和原反馈不改。清理仍独立检查完整账目、退出、
所有权、原清单与交付条件。

OneRequest 接管只影响选中请求的对账与预算；不能修改兄弟请求代次、租约或账本。
旧 fake reservation、旧配置缺省解释和原精确结算重放保持。新增结构采用新迁移，
不编辑已发布 SQL；已有数据的结构升级仍需显式全库维护，不能借局部对账隐式迁移。

R3.3b 的完成报告须分别给出：旧数据库兼容、原证据字节不变、账本重建守恒、
故障事务回滚、零发送/零重发、迟到响应不复活工具、终态对账与兄弟隔离的实际证据。
R3.3a 组件测试不计入这些尚未实现的验收。

## 9. 官方 API 依据

2026-10-26 核对以下 libcurl 官方说明，实际采用的库版本和配置由报告记录。

| 依据 | 本契约使用的语义 |
| --- | --- |
| [multi poll](https://curl.se/libcurl/c/curl_multi_poll.html) | poll 等待受参数与内部更早计时器共同限制 |
| [progress callback](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html) | 空闲回调频率较低；返回非零中断；外层循环仍需驱动 |
| [NOSIGNAL](https://curl.se/libcurl/c/CURLOPT_NOSIGNAL.html) | 禁止信号时标准同步 resolver 的超时限制 |
| [runtime version](https://curl.se/libcurl/c/curl_version_info.html) | 实际库版本、协议与能力检查 |
| [multi 阻塞边界](https://curl.se/docs/todo.html)、[resolver #4852](https://github.com/curl/curl/issues/4852) | ASYNCHDNS 不能排除 threaded resolver 清理等待，需单独门禁 |
| [peer verify](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html)、[host verify](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYHOST.html) | 证书链信任和 URL 主机身份独立校验 |
| [protocol restriction](https://curl.se/libcurl/c/CURLOPT_PROTOCOLS_STR.html) | 显式限制 HTTPS，不能依赖库全部协议默认值 |
| [proxy](https://curl.se/libcurl/c/CURLOPT_PROXY.html)、[netrc](https://curl.se/libcurl/c/CURLOPT_NETRC.html)、[redirect](https://curl.se/libcurl/c/CURLOPT_FOLLOWLOCATION.html) | 显式空代理禁用环境代理；忽略 netrc；不开重定向 |
| [header callback](https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html)、[body callback](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html) | 回调范围、完整头行、trailer、可变块长和失败中断 |
| [transfer decoding](https://curl.se/libcurl/c/CURLOPT_HTTP_TRANSFER_DECODING.html)、[content decoding](https://curl.se/libcurl/c/CURLOPT_HTTP_CONTENT_DECODING.html) | chunk framing 与内容解压不同，不能混淆原字节层级 |
| [debug callback](https://curl.se/libcurl/c/CURLOPT_DEBUGFUNCTION.html) | debug 可能包含头和正文的敏感值，不进入安全日志 |
| [libcurl environment](https://curl.se/libcurl/c/libcurl-env.html) | SSLKEYLOGFILE 会记录 TLS secrets，须独立于 verbose/debug 限制 |
| [explicit resolve](https://curl.se/libcurl/c/CURLOPT_RESOLVE.html) | 映射精确 host:port，保持 URL/SNI/证书身份 |
| [CA blob](https://curl.se/libcurl/c/CURLOPT_CAINFO_BLOB.html)、[CA directory](https://curl.se/libcurl/c/CURLOPT_CAPATH.html) | 显式内存信任材料与关闭默认 CA 目录 |
| [prerequisite callback](https://curl.se/libcurl/c/CURLOPT_PREREQFUNCTION.html)、[socket options](https://curl.se/libcurl/c/CURLOPT_SOCKOPTFUNCTION.html) | TLS 后、HTTP 前的检查边界及连接前 socket 缓冲配置 |
