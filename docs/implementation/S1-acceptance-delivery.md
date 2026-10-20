# S1.5 业务验收、交付与经验记录契约（0.6.0-s1.5）

更新：2026-10-20（UTC）。目标蓝图仍为 **v0.4.1**，开发基线为 S1.4 `98fe590ce1cedda8c0427bd21251299d7b19cd8e`。本增量沿 S1-02/03/08/10/11/12，将当前候选接入独立 Dispatcher 业务判断、原始字节交付包、精确回执和可恢复经验记录。

这是 S1 的一个实现增量，**整个 S1 尚未完成**。本地 FakeModel 驱动真实 SQLite、文件发布和跨进程 CLI；不据此声称模型判断质量、跨功能独立审核、真实供应商调用、强沙箱部署正向或资源删除已验收。实际结果和命令见 [验证报告](../reports/s1-acceptance-delivery-validation.md)。S1.4 的原问题、提交与团队检查契约继续有效。

## 1. 三个不同的完成事实

| 事实 | 产生方及条件 | 不由它推导的结论 |
| --- | --- | --- |
| `CANDIDATE_READY` | TeamLead 对原提交进行本地检查，完整原始字节、自检和验证证据一致 | 尚无 Dispatcher 业务验收或交付回执 |
| 任务 `ACCEPTED` | Dispatcher 对宿主捕获的一个当前候选作明确肯定判断；Store 核版本、权限、AC、原证据和操作状态 | 不表示包已持久写到接收位置，也不表示经验和清理完成 |
| 请求 `DELIVERED` / 交付 `CONFIRMED` | 已发布的精确包再次通过宿主核验和同步；Root 记录包 ID、哈希、版本、接收者、方式及幂等键 | `durable_local` 不表示用户已阅读；业务完成不表示资源已停止或已清理 |

Root 在本批只执行交付技术组装、存储核验和回执控制。业务肯定判断仍来自 Dispatcher 的单独模型动作，不能由 Root 根据 TL pass 或文件存在自行补造。

## 2. SQLite 406 与历史兼容

追加 `db/migrations/008_acceptance_runtime.sql`，从已知 405 在事务内升级为 406。原 `schema.sql` 和 004–007 资产不改。已有请求、冻结配置、原调用/反馈、问题、提交字节和团队检查继续留存。

Store 构造函数第八个参数为 008 资产；新 CLI 提供全部已分发资产。每代指纹必须匹配，错误资产和迁移失败拒绝并回滚。旧 402/403/404/405 调用方保留原版本接口；ReadOnly 不迁移、不接管、不增加业务执行代次。

新增记录保存业务判断、经验报告和交付意图，复用原有独立 `experience_jobs`、`receipts`、`cleanup_jobs`。原版本证据不可原地改写；当前有效性通过状态和绑定判断。收据与收尾作业不因请求外键级联消失，但本批没有删除请求行的入口。

## 3. Dispatcher 当前候选验收

`accept_candidate` 模型输入只包含 `passed`、可选 `notes` 和逐项 `criteria_results`。肯定判断必须给全部当前 AC 的 `criterion_id / met / evidence`，并全部为 `yes`。候选 ID、任务 CAS、提交 CAS、需求/计划/说明版本和文件哈希由宿主在模型调用前捕获，模型不能自行填写或选择另一份提交。

管理上下文包含原请求/AMEND、当前任务和 WorkOrder、原提交、TL 检查以及从原 base64 解码的文件视图。UTF-8 显示原全文，二进制明确标记；不会读取后来变化的工作目录来补造历史。完整上下文容量仍在预约前检查，不能截短约束或把未提供的证据当成已读。

肯定提交时重复检查：

- Dispatcher 的当前身份、请求范围、捕获绑定与 policy；目标任务及提交的精确 CAS。
- 候选仍为当前 `CANDIDATE_READY`，原生产语义版本和 policy 未失效；纯执行代次接管保留原语义事实。
- 原自检、验证、已知问题与 TL 检查一致，当前 AC 精确覆盖，证据非空，不允许模型豁免或缺项。
- 未回答的阻塞问题、在途操作和 UNKNOWN 不允许被肯定判断绕过。未答的非阻塞问题保留原文，并在交付包中明确披露。
- 本批只接普通 `dispatch: plan` 流程；需要正式独立审核而未满足的任务拒绝，模板确定性验收器仍按蓝图 6.4 后续接入。

肯定验收事实、任务 `ACCEPTED`、对应经验作业及事件在同一事务提交。任务终态门禁停止 Worker 新业务；这不自动证明真实进程已停止，也不会把团队虚报为 `CLEANED`。

否定决策或对当前已验收候选的负面撤销保留原判断，将当前候选阻塞并使未确认包失效。每请求普通验收历史上限为256条，必要负面控制可以使用总计512条的有界历史容量，以及既有8192条控制事件容量；不因此绕过模型预算。它不是自动新 WorkOrder；后续明确处置和重新提交仍须经过版本与授权门禁。已确认业务终态不接受这种原地回退。

AMEND、计划或相关语义变更使旧验收和未确认包失效。已接受任务仍需显式 `update / keep / cancel`；不因之前接受而跳过新需求。旧提交、判断和已发布目录继续作为历史留存。

## 4. 经验作业与终态恢复

每次接受在同一事务建立经验作业，按原请求、任务、团队、任务修订和说明修订区分。`source_snapshot` 和源哈希保留原任务、原提交与原验收证据。

TeamLead 通过专用 `report_experience` 动作提交已有 `ExperienceReport` Schema 的模型字段。宿主填入 report/request/task/team 身份；明确 `no_new_experience=true` 时 entries 必须为空，否则必须有实际条目。没有脚本、调用错误、预算不足或进程中断都保留待办，不能自动填写“没有经验”。

经验调用具有独立的 job ID、CAS 和 source hash。它可以在任务已接受、请求已交付后进行，但只声明 `report_experience`，仍使用完整原调用信封和原预算账本。普通 Dispatcher 规划、TL 发说明和 Worker 业务不会因这条管理例外获准在终态继续运行。当前管理身份/政策与原经验源分开保存，不能给旧任务源改盖成当前版本。

本批 `memory_assessments` 只接受空数组；经验 entries 是留存的提案，不自动写 Memory、发布 Skill 或让规则生效。Git/SQLite 记忆同步、版本化加载与评价仍属于 S3。

交付包可以明确列出当时未完成的经验。之后报告完结不修改已发布包；独立持久作业及报告继续可查。恢复扫描包括业务终态请求的未完经验和收尾作业。

## 5. 冻结交付意图和原子目录发布

### 宿主冻结意图

`prepare_delivery` 只组装已有肯定验收证据。它核当前所有必需任务、需求/计划、完整原文件、操作与 UNKNOWN；未完成的可选范围必须明确披露，不能自行声称已完成。

意图先持久化，再进行文件操作。重试同一当前版本及目录身份复用原意图，不重新采样工作目录。包包含清单、说明、完整原始需求及 AMEND、冻结计划、已接受成果、验收证据、限制、副作用和经验报告/明确待办。`questions.json` 保留原问题、身份与转发记录，清单列出未答的非阻塞问题；`usage.json` 保存准备交付时的真实预算账本及逐次用量记录。宿主 envelope 使用独立格式，不把旧 `DeliveryManifest` 中的模型说明当成可以自证验收的事实。

交付包用量冻结于准备交付时，留存回执用量冻结于业务终态时。后续终态经验调用仍计入保留的预算和 attempt 账本，不改写原包或回执；本批可通过 `budget` 和 `history` 查询。将来执行私有行 purge 前必须另行完成最终用量汇总结算，不能把较早快照声称为最终总量。

发布模块消费固定的五个字段：

```text
delivery_id, root_identity, manifest, manifest_sha256, files
```

`root_identity` 为 `{path, device, inode}`。`files` 每项为 `{name, sha256, size_bytes, content_base64}`；文件名是安全的平面 basename，避免不同团队同名路径冲突。`manifest.files` 必须逐项等于去掉 base64 后的元数据，并按名称排序。清单 SHA-256 对完整内部 canonical JSON 字节计算，不添加末尾换行，不声称是 RFC 8785。

原业务成果继续受 S1.4 的每提交 8 文件、单个 64 KiB、总 256 KiB 限制。包整体上限为 64 个显式文件，单文件 2 MiB，manifest 1 MiB，**manifest 与全部解码文件合计 4 MiB**。超限显式拒绝，不丢弃原需求或裁剪证据。这些是单包逻辑容量，不是磁盘配额。

交付目录必须是显式选择且已存在的目录，与控制数据库和已绑定业务 workspace 隔离。宿主固定规范路径、设备和 inode；以后新 workspace 绑定也不能进入现有交付根。

### 文件系统步骤

`DeliveryDirectory` 复用强路径解析和固定根身份，在同一根内建立本次独有 stage，写入全体原文件及 `manifest.json`，同步文件和目录，使用 `renameat2(RENAME_NOREPLACE)` 发布，再同步根目录并核验完整包。Store 随后核当前绑定和原意图，记录 `STORED`。

数据库事务和文件系统不是同一个原子事务。CLI 在只读预检后取得写者，再调用 `authorize_delivery_publication` 复核原意图，防止预检后发生的 AMEND 被旧授权绕过；写后 Store 再次校验。崩溃可能留下 `PREPARED` 意图和已经发布的目录；恢复用原意图核验已有文件并再次同步，完成数据库记录。相同包重复发布不覆盖文件；同 ID 不同内容、文件篡改、缺项或额外项均拒绝。

根身份变化、符号链接、硬链接、FIFO 和非法名字拒绝。失败清理仅限本次调用确证创建的 stage 文件和 inode；不能按名字前缀递归清理崩溃孤儿。rename 之后无法确定同步结果时返回 `OutcomeUnknown` 并保留意图，不能提前生成回执。

只读 `inspect` 核验全包并返回 `verified=true, durable=false`，不执行 fsync、不改数据库、不确认接收。`publish` 的成功结果包含 `durable=true`；复用已有包也重新核验并同步。POSIX 权限和合作锁防止误改及合作进程竞争，不声称隔离任意同 UID 宿主代码；回执前仍须再次核全包。

## 6. 回执与收尾边界

`ack-delivery` 要求精确的 package ID、manifest SHA-256、需求版本、计划版本、接收者、方式及幂等键。CLI 在只读预检阶段拒绝错请求、错目录和错 tuple；写者阶段再次核验并同步原包后，把本次 `fresh_publication` 证据交给 Store，以当前授权提交回执并保留首份证据。精确重放也须重新核验和同步，原稳定意图的幂等身份不包含易变核验字段。

同键同意图可跨进程和代次重放原结果；同键不同包、哈希、接收者或方式冲突。`durable_local` 表示完整包已保存到约定本地目录，`explicit_ack` 表示宿主提交显式确认；网络接收者认证和 HTTP ACK 路由尚属于 S1-12，CLI 不声称已实现这些外部协议。

确认事务同时更新业务和交付状态、保存独立 retained receipt、创建 `cleanup_job`。若同请求还有其他未确认意图，事务把它们置为 `SUPERSEDED`，保留原清单和已发布目录，不留下终态后无法确认的假待办。发布前、发布记录和最终确认都重新检查全请求的未回答阻塞问题；准备交付后出现的新问题必须先处理，不能用旧意图绕过。

取消路径也在业务终态事务内建立对应留存回执和收尾作业；取消不伪造业务交付，不把 UNKNOWN 当成已撤销。留存截止时间取请求冻结配置的 `system.cleanup.receipt_ttl_days`，406准入支持1..36500天；本批只保存截止时间，不执行TTL删除。

`closeout` 只读展示回执、经验、收尾作业及阻塞原因。本批没有实际停止进程、按所有权清单删除工作区、purge 私有数据库行、TTL 删除或严格擦除入口，也不会把待办置 `DONE`。示例 `cleanup.enabled` 继续为 false。完整收尾须在后续满足进程退出证明、唯一成果留存、精确回执、所有适用经验完结、UNKNOWN 核清和批准资源清单后实现。

## 7. CLI 与可重复演示

新命令：

```sh
wls deliveries --db FILE --request ID
wls experience --db FILE --request ID [--task ID]
wls closeout --db FILE --request ID
wls publish-delivery --db FILE --request ID --delivery-dir DIRECTORY [--delivery ID]
wls verify-delivery --db FILE --request ID --delivery ID --delivery-dir DIRECTORY
wls ack-delivery --db FILE --request ID --delivery ID --delivery-dir DIRECTORY \
  --manifest-sha256 HASH --requirements-revision N --plan-revision N \
  --recipient ID --key KEY [--method durable_local|explicit_ack]
```

查询不接管写者；`publish-delivery --delivery ID` 精确恢复该意图。发布不自动确认，只有显式 ACK 才产生 `DELIVERED`。

`tests/fixtures/s15_fake_complete.json` 串起 Dispatcher 计划、TL 说明、Worker 实际写入与提交、TL 检查、Dispatcher 验收、TL 明确经验报告。`s15_fake_accept.json` 可对已有候选单独推进验收，`s15_fake_report.json` 用于后续原经验待办。每次显式 pump 的角色脚本序号从 1 开始，持久 attempt 身份和历史不会重置。

运行器仍是有限同步 Fake pump：默认最多 12 次调用、每任务最多 1 次 Worker 调用；演示显式使用 `--worker-calls 2`。缺脚本、错误和容量耗尽保留具体待办。业务验收与经验各绑定单一捕获对象，事件仅确认真正处理的原 ID。

## 8. 继续推进 S1–S5

下一批补 S1-11 的受控资源所有权清单、停止与退出证据、按清单删除和可恢复 checkpoint，再接 S1-12 本地服务/API。S1-06/07 的强环境正向、残留 cgroup 恢复、输入挂载与后台操作继续独立推进；真实协议和供应商验收按已配置端点与预算执行。

S2 继续承担跨功能独立审核、DAG、审核容量预留和对照；S3 实现记忆版本闭环；S4 接 MCP 与评测；S5 按证据扩展互通和可选严格清除。不以本批包落盘和回执测试把这些阶段提前标为完成。
