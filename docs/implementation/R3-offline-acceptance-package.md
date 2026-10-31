# R3 离线产品验收包 v1

本模块实施 v0.5.0 P2 的**离线准备**部分。真实供应商、实际费用和独立语义质量验收仍为
`NOT_RUN`；主协调者的正式验收为 `PENDING`。它不修改运行时、Store 410、已发布 SQL、
原评测 v1/v2/v3 或供应商适配器，也不授予真实调用权限。

接口是 `scripts/r3_offline_acceptance.py`，版本化输入位于
`scripts/offline_acceptance/v1/`。遵循
[产品验收卡](../spec/v0.5.0/07-product-acceptance.md)、
[原评测器](R3-evaluation-framework.md)、[单逻辑 Agent](R3-single-agent-baseline.md)、
[签名计费](R3-signed-billing.md)和[原 live 工作流](R3-live-workflow.md)。
本模块新增入口和测试，未改变原判据；因此无需重新定义 ADR-087/089 的协议。

## 使用与输入冻结

在普通用户拥有的私有数据盘目录执行，提前设置数据盘上的 `TMPDIR`。`--package` 必须是
尚不存在的目录，父目录必须已经存在。以下路径仅为示例；替换成当前已验证的二进制和数据盘目录。

```sh
python3 scripts/r3_offline_acceptance.py prepare --package /mnt/d/acceptance-run-01
python3 scripts/r3_offline_acceptance.py dry-run --package /mnt/d/acceptance-run-01
python3 scripts/r3_offline_acceptance.py run-fake --package /mnt/d/acceptance-run-01 --wls /mnt/d/build/wls
python3 scripts/r3_offline_acceptance.py report --package /mnt/d/acceptance-run-01
python3 scripts/r3_offline_acceptance.py check-real-template --card /mnt/d/acceptance-run-01/real-execution-template.json
```

`prepare` 写入原五任务逐字材料、seed 和独立 AMEND 文本、固定执行顺序、两份配置、函数工具
和环境配置、Fake 参数模板、人审 JSON/Markdown 表及**禁用**真实执行准备卡。原材料及逐项
机器判据直接来自 `evaluate_r3.TASKS`；新增包不能改写评测器的任务、样本数或旧版本解释。
配置与材料的 SHA-256 固定在 `manifest.json`；检查不仅核对清单自哈希，还核对当前版本化
输入原字节，因此仅改配置并重算清单也不能把 `run-fake` 切换为真实适配器。

两条件只有 `context.logical_agent` 不同：single 固定
`single-agent-role-broker-v1 / max_attempts=128 / max_history_bytes=524288`，multi 保持
原角色历史隔离。共同配置固定 structured-v2、fake/scripted、provider_default reasoning、
零付费重试、同一环境、同一初始约束、文件工具以及一次共享返工额度。Fake 请求预算为
64 attempts、1,000,000 input tokens、100,000 output tokens、0 micro USD；每格墙钟
上限五分钟。这些数值只属于合成实验，不是供应商预算默认值。

版本化配置只授予 `fs_read / fs_write / ask / ack_change / submit`；无业务 `run` 权限。
环境卡没有需执行的检查命令。清理保持禁用，原数据库和工作区留作证据，不用资源删除来
制造已完成状态。包目录是 0700，包文件与命令日志为 0600，拒绝覆盖现有包及符号链接输入。

## 干跑、执行与十格分母

`dry-run` 仅用有界本地读取验证冻结字节。它不打开 Store、不启动子进程、不读取凭据环境、
不请求 socket，也不生成任何 reservation 或 send authority。输出保留十个 `not_executed`
格；这些是未执行状态，不能冒充零调用成功的模型样本。相关测试直接拦截 SQLite、网络、
子进程、环境查找及写打开函数，并比较运行前后的全部包字节，不只相信输出中的零计数。

`run-fake` 只能执行原 `submit / pump-fake / pump-single-fake / amend / publish-delivery /
ack-delivery`。它在新 `run/` 中创建只用于冻结计划的种子请求，使用原 `make_plan` 在任何
业务尝试之前生成 synthetic v3 计划，然后执行五任务 × multi/single 两条件，每格一份
独立临时 Store。单角色切换仍由原 broker 和 Store 授权；宿主没有写入模型业务行的旁路。

| 任务 | 合成动作及原证据 |
| --- | --- |
| tool_roundtrip | 原 Worker read seed → write result → read result → submit，原 TeamLead check；检查原提交 `alpha\nbeta\n` 字节和反馈 |
| partial_amend | 原计划和两个工单后输入 AMEND；模型先对 alpha update，再在下一批对 beta keep；遗漏阶段 beta 不执行；最后两个任务按当前绑定提交、检查和接受 |
| rejected_rework | 原候选被模型拒绝；下一批模型 change_task、工单、新提交和接受；保留第一份拒绝、提交和原字节 |
| exhausted_rework | 同一冻结一次额度被用完后第二次拒绝；模型 escalate_task 产生当前开放的 owner 问题；不借回答或重启补额度 |
| delivery_receipt | 完整原来源链接受后，以宿主技术入口发布原字节；核已验证持久包，再按精确 hash/需求/计划/recipient/method/key 回执 |

Fake 角色序号属于每次 pump，跨批真实历史仍由 Store 保存。固定脚本只证明工程机制，不能
证明模型理解或语义质量。参数模板是版本化模块材料；生产入口不导入测试脚本。

每个外部命令保存原 argv、stdout、stderr、退出码和宿主单调时钟耗时；命令超时记录
`TIMEOUT`，保留原输出。运行索引及时记录已准入的原 request，失败不重试、不用新请求替换。
第二次对同一包执行会拒绝，不覆盖原失败或成功样本。修订实验需要新包及新记录。

`run/plan.json`、`runs.json` 可直接供原评测器使用。`run/report.json` 包含未改变的 v3
报告和单独的运行器结果；全部失败、无效证据和未执行仍保留在十格分母。原 usage 的未知
维度继续 null / unknown，不换成零；预算冲突、来源缺失、实际单 Agent 原生历史被省略
均由原评测器拒绝或判失败。两条件质量和费用优势字段始终未建立。

`report` 只读已有 Store，不改写原尝试、反馈、结算、工单、提交、接受或历史。
`run/identity.json` 记录实际二进制、入口与评测器身份，`evidence-sha256.json` 冻结本次
输出文件摘要。完整源码输入清单、独立工程审核和最终提交/CI 身份由本批统一交付报告记录。
这些本地一致性摘要不是供应商签名或可信账单。

进程在运行中断后，可以对已经形成的计划/索引执行 `report` 查看剩余未执行格。没有完整
结束记录不能宣称完成；若在种子准入或计划生成前中断，保留命令日志，另建新包执行，
不能补造原计划或删除失败目录。

## 人审与真实执行准备卡

JSON 和 Markdown 人审表都固定十格，每格记录语义判据、reviewer、时间、原证据引用和说明。
表初始全部 `NOT_RUN`。程序不会从结构通过自动填入人审通过。固定先 multi 后 single、
单次重复、合成费用、宿主调度和缓存温度是明确混杂因素；报告不能据此声称统计显著优势。

`real-execution-template.json` 是准备记录，**不是** `wls-live-workflow-authorization-v2`
文件；也不存在 `run-live` 子命令。需要填写精确 endpoint/model/adapter/protocol/reasoning、
能力与核验来源、宿主凭据引用和权限、各维度逐请求/逐尝试额度、call/wall/batch/取消及剩余额度、
计费签发来源与 verifier/原 wire 绑定/未知维度规则、工具环境、费用责任方、原请求/配置/profile
和材料摘要、审批时间/范围及协调者执行卡。金额和授权不预填。

`check-real-template` 检查必填字段和受限类型，缺项退出 2；已完整退出 0 仅表示字段齐全。
它不连接端点，不验证远端能力或账单来源真实性，不解析 `env:NAME` 或 `file:/path` 凭据。
准备卡中 `execution_authorized` 必须始终为 false，真实执行状态必须为 `NOT_RUN`，否则拒绝。
填写完整后的结果仍将独立核验、真实质量、实际费用和真实执行标为 `NOT_RUN`。
后续真实执行仍需要协调者另行派发，沿用原 live/签名计费门禁；本模块不能将某 Responses
服务假定为支持本项目的签名账单协议。

## 测试与证据范围

```sh
python3 -B tests/r3_offline_acceptance_tests.py /path/to/source
python3 -B tests/r3_offline_acceptance_cli_tests.py /path/to/wls /path/to/source
```

第一项是一个纯 Python 合约套件。第二项是一个 CLI/Store 集成套件，其中十个业务样本位
不是十个独立套件。它覆盖实际十格来源/账目/身份、分批 AMEND、原否定与额度耗尽、精确
交付、只读报告、覆盖拒绝、缺失来源、账本冲突、重复样本、重哈希伪造原生历史、伪装 provider、
真实 Fake 未知费用及旧 v1/v2 单 Worker 语义。负例只破坏独立数据库备份，不修改正向样本。
既有签名计费 E2E、loopback、恢复/清理和历史兼容仍按本批完整验证执行并单独记录。

新增包此前不存在，因此旧版“该入口不存在”不是产品故障反例。适用红例是在现有 Store
中真实生成业务证据后独立篡改来源、账目或身份，并确认原评测器不接受；测试工具故障、
夹具错误和真实产品缺陷必须分开登记。所有实际命令、退出码、失败调试记录及未执行项
以本批新的验证报告和私有原始日志为准，不从本文推导执行通过。
