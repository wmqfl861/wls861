# 独立计费与单 Agent 基线联合验收

日期：2026-10-27（UTC）。开发基线：
`1b7020e3676a57da233aa097d32956a80942734e`，Store 410。

## 结论与证据范围

本次独立 Release 联合执行 **14/14 个测试方法通过**，固定五个任务的
`multi_agent` / `single_agent` 两条件 **10/10 个原始证据单元通过**。
测试以普通 UID/GID 65534、空附加组运行，unittest 实测 43.683 秒，整个测试进程
43.892 秒。套件数、方法数、成对单元数和方法内反例数分别统计，不相加。

每次模型调用都经过实际 CLI、原 Store 预约、HTTPS 传输、原响应结算、独立签发的
Ed25519 账单收据及生产验签/核账路径；连续运行没有注入测试专用费用 hook。
签发端使用 OpenSSL 命令行，生产端使用自身收据校验器。两者使用同类密码库，
这里的独立性指签发输入、签名流程和断言不调用生产 verifier 来生成预期结果，
不声称使用两个独立密码算法实现。

端点、模型响应、TLS CA、签发公钥/私钥、凭证、任务和账目都是本机合成夹具。
零真实供应商调用、零付费调用。报告保持
`provider_quality_acceptance_established=false` 和
`general_quality_or_cost_advantage_established=false`；合成签名费用不能用作真实价格、
模型质量或多 Agent 优势结论。

测试实现见 [test_billing_baseline_e2e.py](../../tests/test_billing_baseline_e2e.py)。
模型选择逻辑及 TLS 基础设施复用原有 `test_live_workflow.py` / `test_https_transport.py`；
本测试独立实现签发、真实 CLI 编排、原数据库断言和实际 wire 检查，未把复用夹具
描述成独立模型或独立任务生成器。

## 冻结输入与共同条件

执行从只读用途的冻结副本读取源码和资源，实际二进制亦单独复制，避免运行期间
工作树同步改变输入。这里报告的是该固定输入的专项成绩；最终完整 Release/ASan
门禁由整树报告另行记录。

| 输入 | SHA-256 |
| --- | --- |
| Release `wls` | `4ae6766372f54651260cb346fb607d13a8674d3c66a04bef486c04e48c2e7eb2` |
| `tests/test_billing_baseline_e2e.py` | `9e0aa6e7ebba643bc0e30849558b9aade8c2e379dd1c7b53e46ca5e707c48f9e` |
| `scripts/evaluate_r3.py` | `6bc4263e50f062504e22bfc40ee9ec582be308fdced9aeda3bc6c69836251cbf` |
| `tests/test_live_workflow.py` | `34f4d8e21e8dddaeeb6b915906ef85e7f11b79581726b484fabd4d101427320d` |
| `tests/test_https_transport.py` | `69b48522ae3005a18880a0414e8c5f9b49f903ff4f4bbe09958e138bade1d392` |

外层 umask 为 0002；本测试的 TLS/业务夹具显式切换到 0077，并在清理时恢复。
不能把外层 0002 冒称为内部每次创建文件时的权限策略。

计划在零模型调用的原 seed 请求上生成，采用 v3、`model-origin-v1` 和
`single-agent-role-broker-v1`。五份固定材料、工具、推理设置、模型完整条目、
TLS 端点、预算和评测规则对两条件一致。单 Agent 条件只额外冻结原逻辑身份策略：
128 次历史上限、524288 字节历史上限；各角色仍有原 Store 权限和生命周期。

共同合成 profile 和模型能力均明确声明 context window 262144、最大输出 4096。
每个请求原预算固定为 64 次尝试、4000000 输入、1000000 输出、100000 micro USD；
每次预留成本 100 micro USD。测试每次授权有限 batch，正常后续 batch 最多 6 次调用、
最多续接 8 个 batch，两条件一致；单 batch 最长 60000 ms，单 CLI 子进程等待上限
150 秒，CTest 套件上限 900 秒。上述数值不代表任何真实模型能力。

## 固定十个原始单元

以下次数来自原 `call_attempts`，费用来自签名核验后的投影；耗时为该原单元实际
CLI 编排时间，包含恢复和本地文件操作，不是供应商延迟基准。

| 固定任务 | 条件 | 原尝试数 | 已知合成费用（micro USD） | 实测时间（ms） | v3 结果 |
| --- | --- | ---: | ---: | ---: | --- |
| `tool_roundtrip` | multi_agent | 9 | 153 | 1678 | passed |
| `tool_roundtrip` | single_agent | 9 | 153 | 2123 | passed |
| `partial_amend` | multi_agent | 19 | 323 | 2970 | passed |
| `partial_amend` | single_agent | 19 | 323 | 5965 | passed |
| `rejected_rework` | multi_agent | 13 | 221 | 2199 | passed |
| `rejected_rework` | single_agent | 13 | 221 | 3664 | passed |
| `exhausted_rework` | multi_agent | 25 | 425 | 4439 | passed |
| `exhausted_rework` | single_agent | 25 | 425 | 10011 | passed |
| `delivery_receipt` | multi_agent | 7 | 119 | 1209 | passed |
| `delivery_receipt` | single_agent | 7 | 119 | 1440 | passed |

这 146 次原尝试均保留原 `cost_micro_usd=null` 与原 settlement fingerprint，
每次另有一份核验收据和 17 micro USD 的投影；unknown、disputed、over-reserved
均为零。请求预算合计准确计入投影，释放全部成本预留。原事实没有被回填为“原本已知”。
所有原执行来源均为 `local_tls`；每次发送的 billing challenge 均有实际请求 header
记录，且请求内不重复。

固定任务分别验明原生读写回读、两任务部分 AMEND 后 update/keep、原拒绝与修正后
验收、返工耗尽后向 Owner 提问、原交付清单及精确回执。正例均跨独立 CLI 进程恢复。
v3 的通过依据原模型应用、反馈和业务事实，不能由当前任务状态或最终摘要替代。

## 14 个方法的实际覆盖

| 方法 | 原始事实与拒绝结果 |
| --- | --- |
| `01_original_native_roundtrip_pair` | 两条件完成原生 read/write/readback，并核原反馈和成对任务证据。 |
| `02_two_task_partial_amend_pair` | 原 AMEND 后 alpha 更新、beta 保留，跨 CLI 恢复后按原版本事实完成。 |
| `03_original_rejected_rework_pair` | 同一任务原拒绝、原修正工单、新提交、独立检查和验收来源相连。 |
| `04_exhausted_rework_owner_question_pair` | 原返工耗尽后停在当前 Owner 问题，不伪造成功交付。 |
| `05_original_delivery_exact_receipt_pair` | 实际 publish 和 exact ack CLI，核原清单 hash 与要求/计划版本。 |
| `90_invalid_signed_sources…` | 错签名、错 challenge、错响应 body hash、重复收据 header 均不核账、不创建任务/操作；后续 batch 为 needs_accounting，不重发。 |
| `91_partial_unknown_and_over_reserved…` | 缺 cache 维度保持 null；超预留费用保留原 101，不截为 100；均阻止下一次发送。 |
| `92_explicit_reconciliation…` | 缺 header 后显式本地核账；第二个 CLI 进程精确重放返回原结果，全部表的行快照不变，零新发送/业务操作，原 accepted/response/settlement/cost 不变。 |
| `93_trust_inputs_inside_sibling_workspace…` | 同 Store 兄弟请求已绑定 workspace 内的 policy/auth/receipt 被拒，全部数据库行和发送数不变；同名前缀的独立兄弟目录正常通过。 |
| `94_opposite_runner_mode…` | 冻结 single 配置走旧 multi 入口、multi 配置走 single 入口均在副作用前拒绝。 |
| `95_complete_http_error…` | 完整 429 错误体晚到收据及完整 503 错误体同步收据均可核账；原 ERROR/accepted=0/settlement/cost 不变，零任务/操作、零自动重试。 |
| `96_policy_identity_and_keys…` | 第一实际请求发生时修改外部公钥文件；当前 batch 仍用已捕获原 bytes/hash/key，随后旧授权与新文件不匹配即拒绝，零新副作用。 |
| `98_run_index_labels…` | 十份原请求逐一反转 condition 标签，全部 invalid_evidence，其他九个缺失单元保留 not_executed，原库不变。 |
| `99_all_ten_fixed_cells…` | expected/observed 均为 10、paired complete、10 passed，来源/未知账目和两项质量声明旗标均符合原范围。 |

单 Agent wire 检查直接读取 TLS 服务器收到的 Responses 输入，把其中原生 call 的
名称、完整参数和原工具结果与此前每个原 attempt/feedback 比较，证明跨角色全历史
覆盖和值一致。同时分别比较原 call/output 列表长度与其唯一 call_id 数，拒绝同 ID
重复条目，再要求两者的 ID 集合与全部原历史恰好相同，证明每个原历史 ID 实际出现
恰好一次。原候选仅用字典的覆盖盲点由交叉审查指出后补齐，没有减少原值比较。

本联合夹具从最终 HTTP header 交付签名收据，并专测重复 header；没有另设签名
receipt trailer 场景。传输层原 trailer 行为不应被这 14 个方法冒称为新的专项结果。

控制文件测试观察的是数据库行及网络发送零变化。文件真正读取前执行全 Store 路径
守卫，另由 `billing_cli_control_bytes` 与 CLI 调用顺序审查确认；退出码或零数据库变化
本身不能证明“没有读取任何文件字节”。

## 保留的首轮失败及修正理由

首轮候选实际运行 14 个方法、34.746 秒，进程 34.910 秒，退出码 1，
unittest 记录 10 个 failure/subtest 失败。受影响的是五个成对方法及聚合方法，
不是十个不同测试方法。固定十单元聚合为 **1 passed、4 failed、5 invalid_evidence**；
没有删除、改标签或以最终成功覆盖原失败结果。其他签名、核账、权限路径和标签反例通过。

第一类是 v3 单 Agent 评测错误读取 `requests.config_snapshot_sha256`；该原字段在
`request_runtime`。修复只从原 runtime 表读取并核验，不创造配置 hash。

第二类是跨真实 CLI 接管后，原工单和原提交的 epoch 合法不同。原 v3 直接比较 binding
导致四个 multi 单元来源判定失败。修正只对 v3 核对原 immutable instruction、body hash、
工单与提交之间的完整 recover 事件区间、原 previous epoch、逐次推进的新 epoch 及精确
终值；没有把“最终状态相等”当成模型来源。v1/v2 的既有解释保持。

第三类是共同合成 profile 原本声明 window 100000。单 Agent 保留全部原历史后正确撞到
模型容量门禁，发生在 partial AMEND、rework 和 exhausted 三个单元。针对首轮三库的
临时 read-only capture/encode 探针返回同一个原错误：

`request reservations exceed declared provider token capabilities`

| 原失败单元 | 已完成原尝试 | 原历史字节数 | 诊断下一输入预留 | 输出预留 | 原 window |
| --- | ---: | ---: | ---: | ---: | ---: |
| partial_amend single | 14 | 60741 | Dispatcher 102730；TL 99250 | 4096 | 100000 |
| rejected_rework single | 12 | 59401 | Dispatcher 96392；TL 112877 | 4096 | 100000 |
| exhausted_rework single | 12 | 58607 | Dispatcher 106239 | 4096 | 100000 |

探针没有预约或发送，三库 `iterdump` 前后相等。512 KiB 历史上限并未用尽；原运行时以
prompt 字节作保守输入预留。随后为两条件的 seed 和全部十单元共同选择明确声明
262144 window 的合成模型，再生成新计划；未删历史、扩大生产历史容量、改变请求预算、
绕过输入门禁或把原失败重标为成功。

同时改进测试证据留存：调用前保存原计划，登记后及每次 finally 保存 run index，
聚合断言前保存完整报告。索引使用相对 DB 路径；清理前可导出证据，失败也可保留。
导出的本次十库与原 plan/index 在普通 UID 下重新只读评测，所得报告与运行时规范化
报告相等，仍为 10 passed，十个原 SQLite 文件 SHA-256 前后不变。签发私钥和临时签名
输入不随导出复制。

## ASan 联调中的真实到期与有限批次修正

窗口及评测修正后的第 2 个独立 Release 候选曾实际通过 14/14，耗时 46.307 秒，
E2E 源码 hash 为 `ef76f07dd0254af6c31bf224b5802adddfaddcfdf6507942ca9e7ccc3b8f3522`。
随后整树负责人对该候选执行 ASan，联合测试实际运行 478.801 秒，partial AMEND 和
exhausted 两个 single 单元的原 traceback 均为 `stop_reason=deadline`，聚合为
8 passed、2 failed。unittest 的第三个 failure 是聚合断言，不能算成第三份模型失败。
这轮未被记录为通过，也没有删掉不完整单元。

在原夹具清理前，通过 read-only SQLite 连接备份了两库，并核对原记录：

| ASan 原单元 | 已完成原尝试 | 原状态 | 最后 batch recover | 最后 settlement | 实测单元耗时 |
| --- | ---: | --- | --- | --- | ---: |
| partial_amend single | 17 | 全部 COMPLETED / accepted=1 | 13:19:26 UTC | 13:20:27 UTC | 93383 ms |
| exhausted_rework single | 15 | 全部 COMPLETED / accepted=1 | 13:22:59 UTC | 13:23:59 UTC | 66077 ms |

两库没有失败或取消的原模型 attempt；到期后的 Dispatcher 捕获被原 batch 门禁拒绝，
没有新的调用资格。原 60000 ms 授权到期是真实停止，业务尚未达成固定任务终态，
不能把它评为成功，也不应通过扩大期限掩盖。

因此将正常 batch 从最多 32 次调用改成最多 6 次，把有限显式续接循环从 5 批改成
8 批，两条件共同应用。单 batch 60000 ms、CLI 150 秒、套件 900 秒、原请求预算、
全部历史和所有业务/来源断言均保持；对 deadline 的拒绝断言也保持。
更多 CLI 恢复必须重新通过原 v3 来源链，不能靠同一进程内的缓存代替。最终本文所报
第 3 轮 Release 再次通过 14/14，十个原单元的尝试数仍为每条件 9/19/13/25/7。
最终 ASan 和整树普通用户门禁另见负责人整体报告，本文不将 Release 成绩移作 ASan 成绩。

## 输出指纹与复现

最终成功运行现场位于 `/tmp/wls861-billing-baseline-independent-03`，首轮失败及中间
成功分别位于对应 `-01` / `-02` 目录。它们是本次执行现场，不是可长期依赖的仓库资源。
十个私有临时数据库不入 git；脚本可以从合成夹具重建完整流程。以下真实输出已归档，
不能把临时目录可能存在等同于持续证据服务。

两个日志仅移除各行末尾空格/tab；首轮共移除 5 字节，没有删行、改失败结果或重排输出。
两个 JSON 原字节直接保存。归档前已检查无夹具凭据值、私钥、签名及私有 transport
原始字节字段；paired JSON 保留安全来源摘要、全部十单元和未知项。

| 可跟踪归档 | 原始 SHA-256 | 归档 SHA-256 |
| --- | --- | --- |
| [首轮失败日志](billing-baseline-independent-initial.txt) | `9b7a7f24b02a8a993c605859ada9254807bd52e09a4e63c695973ea634f87706` | `d3410b38e34e395fbac8a2c812c00f294a5cb2fb2e2e26ca1a5110a6948863bc` |
| [最终 Release 日志](billing-baseline-independent-final.txt) | `e2649ee7e296191ce5b89cebf3b4736b104f676f0c9ccc6e328f1e728c45b587` | `e2649ee7e296191ce5b89cebf3b4736b104f676f0c9ccc6e328f1e728c45b587` |
| [首轮 read-only quota 诊断](billing-baseline-independent-quota.json) | `26b7321cd1d49fd96a6df7cefded464f8c9c441453e19f7c8e93ea53a022df65` | `26b7321cd1d49fd96a6df7cefded464f8c9c441453e19f7c8e93ea53a022df65` |
| [最终十单元原报告](billing-baseline-independent-paired-report.json) | `5d96a5220dcd177a0815741a3b2258de348115a3c3d9383a6cdb06251bc96e4a` | `5d96a5220dcd177a0815741a3b2258de348115a3c3d9383a6cdb06251bc96e4a` |

其他现场输入和过程指纹：

| 原输出 | SHA-256 |
| --- | --- |
| 中间 Release 成功原日志 | `0cbef071409c61d06d74b9688ac366bdab3f3b17461a013be90e96b7e4e67b49` |
| 首轮整树 ASan 原日志（包含该联合测试失败；整体报告另行归档） | `0c160b78c6bfed56fe1f45ecaccceb629e3a8fab460d8c277b8fcda4ac894239` |
| 最终 `independent-plan.json` 文件 | `067887bf68dfe6357adb520ed773d0005c4a9922a03b8126d5b89e1170d3732e` |
| 最终 `independent-run-index.json` 文件 | `58bf1e1f75b54398db387c9412413484a47fea85a1d92857156867f9f3b484ad` |

计划契约自身的 `plan_sha256` 为
`562ee329045a9ee825d4613fc3d490dac19142290f2c633768f923eeb438e1d4`；
它按契约排除自身 hash 字段，与上表整个 JSON 文件的 hash 是两个不同值。

在普通用户、固定依赖 Python、已构建 `wls` 和可写临时目录下复现：

```sh
PYTHONDONTWRITEBYTECODE=1 \
WLS_BILLING_BASELINE_EVIDENCE_DIR=/tmp/wls-independent-evidence \
/path/to/test-python tests/test_billing_baseline_e2e.py /path/to/wls "$PWD"

/path/to/test-python scripts/evaluate_r3.py report \
  --plan /tmp/wls-independent-evidence/independent-plan.json \
  --runs /tmp/wls-independent-evidence/independent-run-index.json
```

导出目标须为新目录。运行完全使用 disposable loopback TLS，不需要真实供应商配置或
真实密钥。独立同 receipt 归属、key rotation、CAS/事务回滚、历史前链竞争、失败历史、
容量与更细权限反例见两个模块的 native 专项；本报告只计入实际执行的联合范围。
