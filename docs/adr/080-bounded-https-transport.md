# ADR-080 — 有界 HTTPS 组件与原 Store 集成边界

日期：2026-10-26。基线：`8d3c36e3dfff1d64f911a89b79b107778f129807`。
接续 [R3.3a/b/c 计划](../implementation/R3-controlled-live-plan.md)，
实施契约见 [R3 HTTPS 传输](../implementation/R3-https-transport.md)。
本决定覆盖 R3.3a 的组件及 R3.3b 的先行接口设计；后者尚未实现。

## 问题

现有 HTTP codec 和 SSE replay 可以校验调用方提供的完整字节，但没有真实连接、TLS、
取消或网络等待。原 Store 只准入 FakeModel；原结算按完整指纹重放，没有后到用量的
追加核对路径。直接把网络库接进模型接口，会在发送权、未知费用和业务权限之间留下缺口。

本节点应先给出可以用自建服务验收的传输事实，同时保留下一节点必须履行的预约、
唯一发送权、原结算和追加对账约束。组件出现不构成生产调用授权。

## 决定

1. 使用系统 **libcurl >= 7.88** 的 multi 接口承担 HTTPS、HTTP framing 和 TLS。
   编译及运行时核验版本，运行时要求 SSL 和 ASYNCHDNS；不新增自写 TLS、HTTP framing
   或同步 DNS 实现，不修改已有 vendored 依赖。选定发行版的实际版本写入验证报告。
   这是对蓝图默认 Asio/Beast 网络实现的聚焦选型调整：当前出站 HTTPS 复用 libcurl，
   未来 `wlsd` 的调度/服务化目标保持，不以这项组件决定重写调度器。
   ASYNCHDNS 也包括可能在清理时等待 resolver 线程的实现，故另限定：数字 IPv4 直连，
   或宿主显式提供 URL 的精确 host:port 到 IPv4 映射；未映射的 DNS 名须实际运行库使用
   c-ares，否则发送前拒绝。映射不改变 URL、SNI 或证书主机名检查，且纳入传输配置 hash。
2. 组件使用 HTTPS-only、证书链验证和主机名验证。显式关闭代理环境继承、重定向和
   netrc；不启用认证协商、自动付费重试或官方端点回退。测试使用显式 loopback、
   临时 CA 和合成 bearer；不读取 profile 的凭据环境变量。
3. 一次调用使用同一个 `steady_clock` deadline 和 `std::stop_token`。
   multi 轮询最长等待 25 ms，并按剩余时间缩短；循环与回调共同检查取消及到期。
   25 ms 是请求的单次轮询上限，不是操作系统调度或任意依赖调用的硬实时保证。
4. 对响应头、body、累计保留字节和回调块数分别施加上限。HTTP/SSE 的业务语义继续
   交给原严格解码器；完成整个 HTTP 消息且完整协议校验通过之前，不释放工具。
   有效 SSE 终态之后的非法尾部仍失败。中断或超限保留可解释的有界原观察。
5. libcurl 观察、原应用层 body、编码请求 hash、供应商 response ID 和业务可采用性
   使用不同字段。HTTP 层回调字节不是 TLS 报文；本地上传计数不是供应商收到或计费证明。
   未观察到发送也不自动构成免费重试依据。
6. 凭据只作为调用期间的秘密输入，不能加入请求快照、公开 evidence、错误正文或日志。
   不保存 outgoing headers/debug trace。原响应可能含敏感内容，必须与安全诊断摘要分开；
   不通过删除原字节来伪造完整证据，也不自动把它们发布到可分享报告。
   libcurl 初始化与联网前拒绝非空 `SSLKEYLOGFILE`，返回固定宿主错误，不清除或修改
   宿主环境。可信宿主不得并发修改环境或由其他组件预启用/缓存 TLS key logging；
   关闭 debug 不是防止任意进程内 TLS 秘密落盘的完整隔离机制。
7. R3.3a 只交付库组件和受控测试入口；`wls` CLI、`OfflineRuntime`、Store 的 fake-only
   边界及离线 profile 的 `real_calls_enabled=false`、`max_paid_retries=0` 保持。
   没有新增真实 adapter 准入、生产凭据解析、真实预算结算或独立 live CLI。

## R3.3b 必须保持的先行契约

以下是后继实现约束，不是本次新增的 Store 方法、SQL 或已通过的数据库测试。

- 冻结外部 profile 的模型、端点、协议/映射版本、认证引用、能力依据、容量、时限及预算策略。
  新配置只影响新请求；当前政策可以撤销新发送，不能回写旧快照。
- 在短事务中保存完整原请求、独立 attempt 与逐维度预留，随后取得一次性发送权。
  发送权绑定原 attempt、编码请求 hash、原版本和代次；网络等待不占用数据库事务。
- 领取发送权后即使没有响应，也不能重新领取或从崩溃状态推断零发送。
  无法保存原传输/结算事实时阻止新的相关业务，不继续派发工具。
- 原响应、原 usage/null、原结算指纹与当时业务可采用性保持不可变；精确结算重放仍返回
  原结果。迟到响应可以保存原账目，但不能使用当前版本重标记原 response。
- 后到用量采用独立核对 ID、幂等键、原 settlement hash、来源与证据 hash、预期投影版本，
  追加事实并在同一事务中 CAS 更新账本投影。同键异证据冲突；模型自报和费用估算不是
  已验证实际用量。未知维度继续占用预留。
- 新确认维度按真实值调整已花费与预留，记录旧值、新值和差额。超预留真实支出仍保存，
  同时告警并停止新的付费准入。矛盾证据保持待核对，不能覆盖原事实制造守恒。
- 对账不恢复旧结果业务资格，不清工具 UNKNOWN，不重发工具，不改变包或回执；
  OneRequest 的请求范围继续适用于对账。清理须另满足退出、所有权和交付门禁。
- 新结构使用新增迁移，保留所有已发布 SQL、旧快照、原结算和旧 API 的解释。

## 为什么选择此边界

libcurl 已提供 HTTP/TLS、多路等待、版本能力与有界回调接口，可以把新增代码集中在
项目特有的身份、取消、容量和观察语义上。固定 HTTPS 和明确禁止默认外部行为，
使本地夹具能够逐项证明证书、重定向、代理、断流和中断边界。

这里不以连接成功证明模型行为正确，也不因本地原观察足够就放开 Store。原结算与
追加对账分离，让后来的费用证据可以修正账本投影，同时保留当时的事实和业务决定。

## 验收与未完成范围

普通用户执行自建 TLS 的正向、坏证书、错主机、HTTP 错误、超限、半写、断流、
取消/deadline 和终态尾部反例；原 HTTP/SSE、Store、工具、历史、清理与身份回归继续。
构建使用严格警告、两种 umask、ASan/LSan、UBSan 和 Clang；具体执行结果另记报告。
本 ADR 中的测试要求不代表已经执行通过。

R3.3b 的发送权、迁移和对账实现，R3.3c 的供应商/单 Agent 对照，管理角色容量、
强沙箱部署正向和服务化仍是各自后继节点。本地 TLS 不替代这些验收。

## 官方依据

以下官方文档于 2026-10-26 核对；只用于 API 语义，不能代替所选版本的实际测试。

- [curl_multi_poll](https://curl.se/libcurl/c/curl_multi_poll.html)：等待行为和内部较短定时器。
- [CURLOPT_NOSIGNAL](https://curl.se/libcurl/c/CURLOPT_NOSIGNAL.html)：同步 resolver 的超时边界。
- [CURLOPT_XFERINFOFUNCTION](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html)：空闲时回调频率及中断。
- [curl_version_info](https://curl.se/libcurl/c/curl_version_info.html)：实际运行库及能力检查。
- [curl multi 已知阻塞边界](https://curl.se/docs/todo.html)、
  [resolver 问题 #4852](https://github.com/curl/curl/issues/4852)：threaded resolver 的清理等待风险。
- [CURLOPT_SSL_VERIFYPEER](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html)、
  [CURLOPT_SSL_VERIFYHOST](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYHOST.html)：证书链与主机名分别验证。
- [CURLOPT_PROXY](https://curl.se/libcurl/c/CURLOPT_PROXY.html)、
  [CURLOPT_NETRC](https://curl.se/libcurl/c/CURLOPT_NETRC.html)、
  [CURLOPT_PROTOCOLS_STR](https://curl.se/libcurl/c/CURLOPT_PROTOCOLS_STR.html)：显式限制环境和协议。
- [CURLOPT_DEBUGFUNCTION](https://curl.se/libcurl/c/CURLOPT_DEBUGFUNCTION.html)：trace 可能含敏感信息。
- [libcurl environment](https://curl.se/libcurl/c/libcurl-env.html)：`SSLKEYLOGFILE` 会记录 TLS secrets。
