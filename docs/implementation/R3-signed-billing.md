# R3 受信中转签名账单适配契约

决定见 [ADR-088](../adr/088-signed-billing-adapter.md)。本契约扩展原 Store 410 的受信
`VerifiedUsageEvidence` 路径，保留原结算、原发送事实和五维可空用量。
它是需要中转签发端另行实现的明确协议，不能推导任意现有兼容 Responses 服务已经支持。
本批验收使用本地 TLS 和独立合成签发密钥，没有真实供应商或付费调用。

## 1. 冻结 wire challenge

完整原 `responses.provider_profile` 可选增加：

```json
{"billing_challenge":"wls-billing-challenge-v1"}
```

该片段须合入完整有效 profile，不是独立可执行配置。缺少此字段时旧序列化和编码报告
保持原字节/hash，不隐式为旧请求开启账单协议。未知字符串和显式空字符串拒绝。

启用后：

```text
challenge = SHA256("wls-billing-challenge-v1\n" + canonical_json(to_json(original_model_request)))
X-WLS-Billing-Challenge: <challenge>
```

原 ModelRequest 包含唯一 call/attempt ID、冻结配置、完整绑定及逻辑输入；即使 wire body
恰好相同，新 attempt 的 challenge 也不同。challenge 是新的固定 HTTP header，不改变
原 Responses body，不承诺供应商幂等或取消退款。编码报告使用
`wls-encoded-responses-request-v2-billing` 并纳入 challenge；原预约、发送 claim 和传输
仍验证同一编码记录。只支持固定这个 header，不接受模型或宿主任意追加 header。

## 2. 外部信任政策

`billing_trust_policy_from_json` 接受以下严格字段；缺失和多余字段均拒绝：

| 字段 | 契约 |
| --- | --- |
| `schema_version` / `kind` | `1` / `wls-billing-trust-policy-v1` |
| `trust_domain` | `local_tls_fixture` 或 `provider_billing`，精确匹配原 execution kind |
| `request_id` | 原请求 ID |
| `config_snapshot_sha256` | 原冻结配置 SHA-256 |
| `issuer` / `account_id` | 明确的外部受信账单来源与账户；有界 ASCII 标识 |
| `public_keys` | 1～8 个 key ID → 32 字节 Ed25519 裸公钥的 64 位小写 hex |
| `models` | 去重后的全部原已选 model_ref → **完整 model entry** 的规范 JSON SHA-256；不接受遗漏或多余模型 |

政策最多 64 KiB，model 集合最多32项。值由宿主明确配置，不从 receipt、环境变量或模型
文字取得。政策本身不包含私钥、API key、收费估算或网络权限。
`BillingTrustPolicy::sha256()` 是规范 JSON 指纹；CLI 授权另绑定实际捕获政策文件原字节
hash，两种指纹用途不同。变更公钥或其他政策内容需要相应显式宿主授权。

## 3. 签名收据

完整收据是严格对象：

```text
schema_version = 1
kind = "wls-signed-billing-receipt-v1"
payload = <下表的完整对象>
signature_hex = <64 字节 Ed25519 签名，128 位小写 hex>
```

签名消息是 UTF-8 字节：

```text
"wls-signed-billing-receipt-v1\n" + canonical_json(payload)
```

使用纯 Ed25519，一次验证，不再对消息另加 SHA256，也不接受算法协商。payload 全部字段
都在签名范围内；重复 JSON 键、多余字段、浮点/负值用量、错误单位均拒绝。

| payload 字段 | 契约 |
| --- | --- |
| `trust_domain`, `issuer`, `account_id` | 精确等于政策 |
| `key_id` | 政策中的一个已授权公钥 |
| `receipt_id` | 签发账户内稳定的账单身份；修订和换 key 不换此身份 |
| `revision` | 正整数，标记签发方修订；原账本仍以独立 projection CAS 串行追加 |
| `billing_challenge` | 实际请求头收到的唯一 challenge |
| `request_body_sha256` | 实际 HTTP request body 原字节 hash |
| `response_body_sha256` | 完整 HTTP response body 原字节 hash；HTTP transfer dechunk 后的 body，不含 chunk framing |
| `provider_response_id` | 来源已知的 response ID 或显式 null；已观察原 ID 必须匹配 |
| `provider_model` | 实际 wire request 的精确 model 字段 |
| `token_unit` / `cost_unit` | `tokens` / `micro_usd` |
| `usage` | 必须含 input_tokens、output_tokens、cache_read_tokens、cache_write_tokens、cost_micro_usd；每维为非负 int64 或 null |

cache token 是独立报告维度，不能再加到 input/output 上作为额外总 token。缺失统计不能
填0；五维必须全部有独立依据，连续工作流才会通过原完整账目门禁。费用是已认证来源的
实际 micro USD，不由本库价格表推算。

## 4. 同步返回与晚到核对

中转可在最终 HTTP response header 或 trailer 返回：

```text
X-WLS-Usage-Receipt: <完整收据 JSON>
```

该字段至多一次，名称不区分大小写。信息性响应（1xx）中出现、重复字段、无完整终态、
折行、截断或超限均不能成为同步账单。原 transport 的所有 header/行/累计限制仍适用；
本适配器最高64 KiB总header、8 KiB单行。收据文件导入上限16 KiB，不能以此扩展网络头限额。
HTTP/SSE 都核原 body hash；SSE 可在完整流结束后以 trailer 签署实际输出。

`verify_observed_billing_receipt(Store&, policy, request, response, original_settlement)`
从原私有观察取数据，不读取临时文件，不发账单网络请求。原 request/response/settlement
必须精确相同。真正缺 header/receipt 时返回 null；存在不合法 receipt 时拒绝。
证据在原首次结算之后、原业务应用之前交给旧 `reconcile_usage`，随后继续所有原业务守卫。

`verify_signed_billing_receipt(Store&, policy, attempt_id, receipt_bytes)` 用于晚到导入。
需要原 send claim、原 envelope 和原 settlement。已有完整响应必须匹配其完整 hash；
已观察 response ID 必须匹配。完整 HTTP429/5xx 等错误体也按同样规则核费，不能获得工具
资格。缺 response ID 仍为 null；无响应时必须有原恢复后宿主结算，源映射保留 unobserved；
部分观察保留 partial。原账单证据不会补造原 transport/response，也不会自动创建缺失结算。
原观察明确 network_started=false 或根本没有 send claim 时拒绝来源收费映射。

## 5. 来源唯一性、修订与重放

规范来源身份只包含 trust_domain、issuer、account_id、receipt_id。
`source_reference` 是 `wls-signed-billing-v1:<identity SHA-256>`。
新 `VerifiedUsageEvidence::required_reconciliation_id()` 必须被调用者原样采用：

```text
billing:<identity SHA-256>:<完整 evidence SHA-256>
```

原 Store 事务通过现有 reconciliation 主键的范围查找取得该身份的第一条归属，
验证 source reference/kind/attempt。换key、不同签名字节或revision不会创建新账单身份；
同attempt可以追加不同维度或冲突事实，同收据跨attempt或请求拒绝。
这只证明本 Store 内的归属；不同独立数据库没有共享的全局账单去重服务。

原预算、投影、来源记录、事件同事务提交；失败不占用账单身份。同键同完整原命令重放
返回原结果，异证据/ID/key/CAS冲突拒绝。用量冲突保持已知旧值并标disputed，超预留实际
金额仍全部记录并停止新付费准入。来源验证不清除工具UNKNOWN或恢复旧模型业务资格。

`Store::replay_usage_reconciliation` 是只读精确重放观察，用于CLI接管前核对已有结果。
当前Root身份必须有效，只有比较历史command时采用其已持久的旧dispatcher epoch。
不改epoch、预算、反馈、原响应或交付；旧写入API保持原语义。

## 6. 控制文件与部署边界

`Store::validate_control_file_path` 检查绝对规范路径不能位于本Store任何已绑定业务工作区，
包括兄弟请求。它只返回成功或固定错误，不泄漏其他请求的目录。
这项检查与既有有界/no-follow/类型先验读取共同使用，并在只读预检和取得写者后复核。

实际供应商签发端、账单来源真实性、账户配置和真实收费测试仍独立验收。公钥的可信性由
宿主指定，签名验证不等于自动认证任意供应商。模块默认没有任何信任根，且保持原网络与
provider开关、费用预留、有限batch、零自动重试和强隔离要求。

## 7. CLI 接入与 v2 授权

下面命令使用已经准入并冻结了 challenge 扩展的请求。`WLS_BILLING_DB`、
`WLS_REQUEST_ID`、`WLS_BILLING_POLICY` 和 `WLS_LIVE_AUTHORIZATION` 分别是宿主选择的
原数据库、请求 ID、信任政策文件和授权文件；这些名字只是 shell 变量，不会覆盖原冻结
模型配置。先取得原身份，再由宿主配置第2节的信任政策：

```sh
wls live-status --db "$WLS_BILLING_DB" --request "$WLS_REQUEST_ID"
wls live-status --db "$WLS_BILLING_DB" --request "$WLS_REQUEST_ID" \
  --billing-policy-file "$WLS_BILLING_POLICY"
```

第一条返回原 `config_snapshot_sha256` 和完整 model entry 的 `profile_sha256`，供政策
的 `models` 使用。第二条验证该政策后返回 `authorization_template` v2，以及政策的
原字节和规范 JSON 两种 hash。两条都是只读操作，不解析凭据、不接管写者；返回模板的
网络开关为 false、费用预留为0，须由宿主审阅并明确填写后才能执行。

完整 v2 授权字段如下。示例中的尖括号必须替换为该请求实际模板提供的原值；示例数值
只展示字段类型与有限 batch，不是费用定价或真实供应商调用授权：

```json
{
  "schema_version": 2,
  "kind": "wls-live-workflow-authorization-v2",
  "request_id": "<original req ID>",
  "config_snapshot_sha256": "<64 lowercase hex from the original snapshot>",
  "execution_kind": "local_tls",
  "network_enabled": true,
  "provider_enabled": false,
  "models": {
    "<original model_ref>": "<64 lowercase hex of the complete frozen model entry>"
  },
  "max_calls": 4,
  "max_worker_calls_per_task": 2,
  "max_batch_duration_ms": 30000,
  "reserved_cost_micro_usd": 100,
  "billing_policy_sha256": "<64 lowercase hex of the captured policy file bytes>"
}
```

字段必须完整且无额外键。`models` 必须精确覆盖去重后的三个业务角色所选模型；
`execution_kind` 必须匹配全部原 profile。`max_calls` 为1～64，
`max_worker_calls_per_task` 为1～16，`max_batch_duration_ms` 为1～3600000；
`reserved_cost_micro_usd` 为正整数，且不得超过原请求费用限额，每次实际预约仍核剩余额度。
`local_tls` 要求 network=true、provider=false；`provider` 同时要求两个开关为true，
且只能在另有真实端点、能力、账户与费用授权时使用。

`billing_policy_sha256` 绑定**政策文件原字节**，不是 `BillingTrustPolicy::sha256()`。
仅调整政策 JSON 的空格或换行也会改变这项授权身份。`pump` 在接管前只读取一次授权和
政策，以这些字节解析、验 hash 并捕获公钥；后续使用不再读取同一路径的新内容。
接管后只复核路径与原请求绑定，公开 `host_authorization_sha256`、
`billing_policy_sha256` 也来自同一次捕获。v2 缺政策、v1搭配政策、或政策原字节 hash
不符均拒绝；没有计费适配时继续使用原v1，未知费用仍停止后续调用。

角色隔离请求使用：

```sh
wls pump-live --db "$WLS_BILLING_DB" --request "$WLS_REQUEST_ID" \
  --authorization-file "$WLS_LIVE_AUTHORIZATION" \
  --billing-policy-file "$WLS_BILLING_POLICY"
```

原快照已经冻结 `single-agent-role-broker-v1` 时，改用 `pump-single-live`，其余计费参数
相同。两入口都可以追加 `--workspaces-file FILE`；只有提供显式工作区时才接受
`--sandbox-manifest FILE`。命令选择不能改变原逻辑 Agent 拓扑。同步签名核验从原响应的
唯一收据 header/trailer 取得证据，无需宿主预知后续 attempt ID。

## 8. 晚到收据与精确重放

中转没有同步返回收据、或账单晚到时，宿主将独立取得的完整签名收据保存为业务工作区
外的普通文件，再选择已经存在原 settlement 的确切 attempt。无结算、无send claim、
原观察证明未联网、签名/来源/原wire身份不符时，命令拒绝；它不会先制造一个缺失结算。

```sh
wls reconcile-billing --db "$WLS_BILLING_DB" --request "$WLS_REQUEST_ID" \
  --attempt "$WLS_ATTEMPT_ID" --billing-policy-file "$WLS_BILLING_POLICY" \
  --receipt-file "$WLS_BILLING_RECEIPT" \
  --expected-version "$WLS_USAGE_PROJECTION_VERSION" --key "$WLS_RECONCILIATION_KEY"
```

`--expected-version` 是原 usage projection 的正整数 CAS，**不是** request、attempt 或
dispatcher epoch 的版本。首次结算初始化为1；后续以宿主只读
`Store::usage_projection(attempt_id)` 的 `projection.state_version`，或上次成功结果的
`result.after.state_version` 为准。`live-status` 给出受阻 attempt 和未知维度，不返回该
投影版本。并发核账可能令刚读取的版本失效，旧CAS会拒绝，不能猜一个新值绕过冲突。
`--key` 为宿主给本次精确导入选择的非空幂等键；更换证据或修订需要独立命令和当前CAS。

该入口不接受网络授权文件、不解析模型凭据、不发账单/模型网络请求。它验证政策与
签名、绑定原 attempt 和 settlement 后只追加账目，输出 `model_calls: 0`、
`network_calls: 0`、`credentials_resolved: false` 和 `business_authority_restored: false`。
原响应成功与否、UNKNOWN、工具反馈、业务资格及交付状态均继续由原记录和守卫决定。

首次导入成功后，**原样**重复同一个政策、收据、ID、key和当初的expected version会在
只读阶段返回原result，`replayed` 为true；此时不用把版本改成最新值，也不会接管写者、
推进epoch或追加记录。换key、换CAS、改变签名字节或政策内容而复用已有命令身份会拒绝。
晚到证据内的政策身份使用规范JSON；单改政策文件排版不改变账目证据，输出仍报告此次
实际捕获的政策原字节hash。它与网络执行v2要求原字节hash完全一致的规则分开。
收据revision只能为同一原attempt补齐维度或记下争议，不能把原收据转给另一attempt。
即使核账后五维已全知，也不会自动重放旧响应或继续batch；新工作仍需显式pump及其
原授权、预算、请求状态和全部业务门禁。

本页是接口契约，测试成绩以本批专项报告、统一输入和最终提交CI为准。
