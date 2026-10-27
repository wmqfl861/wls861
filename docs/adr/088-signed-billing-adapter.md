# ADR-088：受信中转签名账单与原费用核对

日期：2026-10-27（America/Los_Angeles）。基线：
`1b7020e3676a57da233aa097d32956a80942734e`，蓝图 v0.4.1，Store 410。

## 背景

原 Responses usage 不提供可直接采用的实际费用，原 CLI 在未知账目处停止。
ADR-081/085 已提供受信 verifier 能力和原结算后的 hook，但没有供应商认证实现。
单纯接受 JSON、按价格表估算、或让宿主为自己填的金额签名，都不能证明供应商账目。
原 wire body 不包含本地 attempt ID；本地 settlement hash 更是在接收之后才产生。
因此不能要求已有中转账单天然签署这两个字段，也不能仅凭可重复的 request body hash
把一张有效账单任意映射到一次新的调用。

## 决定

实现一个明确的新契约 `wls-signed-billing-v1`，供**独立受信中转账单签发方**适配。
本库只验证，不提供生产签发器、私钥加载、收费估算或任何既有中转“已支持”的声明。
真实中转必须另行实现签发端并提供有依据的账目；本批只使用合成签发密钥及本地 TLS。

1. 新请求可在完整冻结的 provider profile 显式增加
   `billing_challenge: wls-billing-challenge-v1`。省略时旧 profile、旧编码报告和 hash
   逐字兼容。启用时由完整原 ModelRequest envelope（包括原 call/attempt ID）派生
   唯一 challenge，进入编码报告完整性检查；原 HTTPS 仅发送固定
   `X-WLS-Billing-Challenge`，没有任意 header 扩展。
2. 中转在实际接收后签署其可观察的 challenge、原 HTTP request body hash、原 HTTP
   response body hash、原模型标识、已知或未知的 response ID 和五维用量。
   它不需要知道本地 settlement hash。签名为 Ed25519，消息是固定域分隔符加规范 JSON。
3. 宿主以独立外部 trust policy 指定 request/config/full model profiles、issuer/account、
   有界公钥集和 trust domain。`local_tls_fixture` 与 `provider_billing` 不可互换；
   模型响应、普通 verified 字段和 receipt 自带公钥都不能选择信任根。
4. verifier 从原 Store 重建原 envelope、编码和冻结设置，先验签，再校验唯一 challenge、
   实际 body 和已观察 response ID，最后映射到**原 attempt + 原 settlement**。
   原签名 receipt 原字节、政策、公钥、来源身份和映射证据保留在私有核对记录。
5. 同步路径只读取原传输中唯一的最终 response header/trailer。缺 receipt 返回无证据；
   存在但格式、签名、身份或完整字节不符时停止。没有额外网络、轮询或付费重试。
   晚到路径显式读取同一签名格式，仅执行原费用核对，不能重新应用旧响应。

## 跨调用重放与事务

一张签名 receipt 的全局来源身份是
`SHA256(canonical{trust_domain,issuer,account_id,receipt_id})`。
它不包含 request、attempt、key ID 或 revision，换密钥和账单修订不能逃避原归属。

新证据要求 reconciliation ID 为
`billing:<source identity hash>:<evidence hash>`，并持久保存规范 source reference。
原 `reconcile_usage` 事务利用已有主键的前缀范围查询检查来源归属；只返回第一条记录，
不需要全表 source_reference 扫描或新 SQL。same attempt 可追加部分维度和修订，
different attempt/request 拒绝；归属记录、投影、预算及事件一同提交或回滚。
真实冲突仍由旧投影保留争议和原已知值，实际超预留不截断。
旧 verifier 没有 required ID，新字段不进入其原 command，旧结算/SQL/API 的解释不改写。

新增只读 `replay_usage_reconciliation` 为跨 CLI 精确重放核对原 command：当前 Root 身份、
请求、证据、ID/key、原 settlement 和原 expected projection version 都必须匹配。
仅在比较历史 command 时采用其已持久 Root epoch，其他身份必须等于当前 Root。
返回原 result，不接管、不推进 epoch、不改预算，也不授予旧业务资格。
原写入 API 的重放语义保持。

## 缺失、失败和权限边界

- 完整 HTTP 失败体也可核费用：已观察 body hash 必须匹配；只有原 decoder 实际观察到
  response ID 时才强制 ID 相同。未知 ID 保留 null，不从 receipt 补造观察。
- 原响应缺失/部分保留时，独立签名账单可依据唯一 challenge 证明费用，映射记录仍明确
  `unobserved`/`partial`；无原发送 claim、无原 settlement 或原观察证明网络未开始时拒绝。
  无响应路径必须已有原 `unobserved_after_recovery` 结算，不自动制造原响应或免费重试。
- 同步 hook 只能采用完整原响应；中断流的 header receipt 不能释放部分工具。
- 可信费用不恢复取消、AMEND、UNKNOWN、旧代次或原否定资格；验收、交付、经验和清理
  继续分别检查原门禁。
- CLI 的授权、信任和收据文件在一次捕获后使用。只读 placement 检查覆盖同一 Store
  的全部已绑定业务工作区，包括兄弟请求，错误不展开其他请求资料。

签名只证明指定信任根签发了这些断言，不是对任意中转计费真实性的自动保证。
真实签发端能力、账户归属、账单获取和供应商实测仍需独立验收；默认无信任根、无网络授权。
格式与接口见 [实现契约](../implementation/R3-signed-billing.md)，实际成绩另见验证报告。
