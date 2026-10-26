# S1.6：受控工作目录清理与恢复

实现版本 **0.7.0-s1.6**；目标蓝图 **v0.4.1**；承接 S1.5
`3d0fc858442254d059128a9032f8a60b7f023df6`。本增量是 S1-11 的工作目录阶段，
不是整个 S1，也不是 S1–S5 全部完成。实际命令和结果单独记录在
[验证报告](../reports/s1-owned-cleanup-validation.md)。

后续 R2a.2 以 [独立契约](R2a-allocation-recovery.md)扩展分配协议及408迁移：新意图使用暂存收据，
旧 v1 原意图保留；新增不可逆的元数据弃置、全部历史查询和有效清理目标选择。下文为原S1.6契约。

### 0.8.6-r3.2 审核修订：工作树名称贯通

受控 `work` 树的名称校验现与既有 Workspace 文件/目录工具兼容：允许有界 UTF-8 POSIX
文件名中的字面反斜杠、换行、制表符和 DEL，清单的规范 JSON 保留其准确字节。清理只将
这些名称作为固定目录描述符下的路径组件传给系统调用，不将其交给 shell 或按 Windows
分隔符解释。此前工具可成功创建这类文件，清理却因共享资源标识校验报错；本次拆分两种
校验，避免已获准的业务文件名阻塞后续目录清理。

运行时生成的 `res_...` / `stage_...` 标识继续使用原严格命名规则；工作树组件仍拒绝空名称、
`.`、`..`、斜杠、NUL、非法 UTF-8 和超长组件，路径保留原长度限制。无链接/无跨挂载解析、
当前 UID 与非共享写权限、原代次、原清单及逐次删除门禁均不放宽。原始清单和收据不改写。
新增回归使用 Workspace 创建上述名称，保留规范 JSON 后分批清理；同时验证不安全清单路径
和不合法资源标识仍在任何删除或创建前拒绝。结果归入本次审核验证报告，不追记到历史成绩。

## 1. 默认与作用域

`cleanup.enabled` 默认仍为 `false`。新请求可显式冻结 `true`，但它只是必要政策条件，
不是删除授权。已有请求不读取后来改过的配置；原普通/外部 workspace 不会被收编。
只有受信宿主新建、登记并验证的目录可进入本入口。模型没有资源登记或清理工具。

每个团队的布局是 `parent/res_ID/work`，所有权收据为 `parent/res_ID/owner.json`。
Worker 的工作边界仅是 `work`。最小 envelope 和收据不删除，交付目录、私有数据库行、
原始调用、提交字节与经验记录继续保留。取消/失败请求还没有失败成果保留包的政策，
本增量拒绝删除它们，不能用一次普通取消充当销毁授权。

## 2. 创建、登记和目录身份

先在 SQLite 保存 PREPARED 意图，再创建专用目录并同步，最后登记 OWNED 身份。
意图包含请求、团队、资源 ID、父目录身份、幂等键和 nonce；同键同目标重试返回同一资源。
不同键不能给同一团队再分配目录；已有 workspace binding 不允许自动采用。

受控目录记录设备、inode、mount ID、birth time 和规范路径。父目录由既有 Workspace
接口固定。平台缺少所需 `openat2`/`statx` 字段时拒绝，不降级为纯字符串前缀判断。
`RESOLVE_BENEATH/NO_SYMLINKS/NO_MAGICLINKS/NO_XDEV` 限定逐级访问。
对象须属于当前 UID 且不允许组/其他用户写；普通文件必须只有一个硬链接。
符号链接、硬链接、特殊文件、目录替换和跨挂载边界都拒绝。

所有权 envelope 不能嵌入另一业务目录或已有资源 envelope；业务目录不能覆盖别人的
所有权边界；分配和交付两端都检查与控制数据库、交付根的重叠。仅允许运行时生成的新叶子。
中断留下没有完整收据的目录时保留 PREPARED 并阻塞，绝不凭相同名称采用未知文件。

## 3. 删除前门禁

宿主使用当前 Root 身份、Store 独占写者租约，且必须同时满足：

- 冻结配置明确允许 cleanup；请求已 DELIVERED，存在精确的 CONFIRMED 包和成功回执。
- 每个经验作业 DONE；不存在待派发、运行中、取消待定或 UNKNOWN 操作；模型用量及费用已结算。
- 无未完成的资源创建；每个已绑定工作目录都对应相同请求/团队的完整 OWNED 身份。
- 所有 `run` 操作都有原始、哈希一致的进程观察；或有从未派发即取消的明确事实。

人工核对操作结果为 FAILED/SUCCEEDED 不生成进程退出证明。已启动的进程必须由原始记录
证明 `all_descendants_exited`；未启动的 REFUSED 记录可证明没有启动子进程。
缺失记录、只有取消/核对结果、兜底 UNKNOWN 或原始哈希不一致都不能放行。
这里消费既有执行器的原始证据，没有新增按任意 PID 杀进程或盲清 cgroup 的路径。

发布目录新增 `verify_and_sync_existing`：重新校验精确文件集、原字节、根身份和清单，
然后同步现有目录。它与 `publish` 不同，**不会重建缺失的包**。每个资源删除边界及最终
检查点都会复核交付。CLI `verify-delivery` 仍是只读，不顺便签回执或执行清理。

## 4. 固定清单、有限执行和恢复

第一次执行先对每个资源生成完整清单，再在同一短事务中保存全部目标、清单哈希、原身份、
进程证据指纹和确认的交付身份。保存成功前不调用 unlink。清单一经记录不能扩展或重新采样。

每个资源每次执行先检查全部尚存条目，再按子项优先顺序删除。条目的原身份、类型、权限、
大小和字节哈希必须一致；新增条目、修改内容或相同内容的新 inode 也拒绝。已经缺失的条目
只可依原持久清单恢复，不能将“目录没了”解释为可以处理新路径。每次 unlink 后同步父目录，
最后删除 `work` 自身并同步 envelope。达到上限时保留阶段状态，后续调用接续同一清单。

| 限制 | 默认值 |
| --- | ---: |
| 每请求资源数 | 64 |
| 每资源条目数 / 最大深度 | 4096 / 32 |
| 单文件 / 每资源读取字节上限 | 16 MiB / 256 MiB |
| 单次 CLI 删除步数 | 默认 1024，可选 1–4096 |
| 清单编码上限 | 8 MiB |

单次步数跨资源累计，删除 `work` 自身也占一步。资源之间不是共同文件系统事务：前一资源
可能已删除，后一资源检查失败；此时原清单和检查点保持可恢复，不能声称原子全删。
普通单次分批使用 `pass_removed_entries` 报告本次实际次数。

`cleanup_jobs` 的 lease_epoch/state_version/claimed_by 与不可变 manifest 哈希绑定领取权。
重开围栏旧执行和旧 claim。物理删除后、SQLite 记录前退出或写入失败时，重开只核验原目录
的缺失与原收据，补记同一清单的证据；不重新执行任何业务操作。

## 5. 阶段完成不等于私有数据清除

所有资源均提供新鲜的 durable absence proof 后，`workspace_cleanup_runs.status=DONE`，
但全局 `cleanup_jobs.status=PENDING`，并保留 `private_row_purge_pending`。
`cleanup_complete` 始终为 false，`private_data_retained` 始终为 true。

`workspace_cleanup_recorded` 是历史检查点，不是之后任意时刻的文件系统断言；只有清理执行
本次成功返回的 `workspace_absence_verified_now` 表示刚完成再次核验。只读 closeout 在中断期
用 `deletion_performed=null` 和 `deletion_may_have_started=true` 表示不确定，不把未记账的删除
错误报告为“从未发生”。完整阶段成功后返回 `WORKSPACES_REMOVED_PRIVATE_RETAINED`。

不执行 VACUUM、TTL 到期删除、Git 历史擦除、备份擦除或介质安全删除。本阶段的独立表和
文件不存在证明不代替将来的私有行聚合 purge、残留进程恢复与严格清除政策。

## 6. CLI

以下是接口形式，参数必须来自实际请求/任务和受控测试目录，不能套用到生产目录：

```sh
wls allocate-workspace --db STATE --request REQUEST --task TASK --parent PARENT --key KEY
wls owned-workspaces --db STATE --request REQUEST
wls closeout --db STATE --request REQUEST
wls cleanup-workspaces --db STATE --request REQUEST --confirm-request REQUEST --max-unlinks 1024
```

先正常提交请求并生成计划，再分配目录；将返回的 `identity.workspace_identity.path` 用作该
任务的 workspace 映射，完成已有执行、检查、验收、交付/回执和经验流程，最后显式清理。
`--confirm-request` 必须和请求 ID 相同。错误确认或其他请求的 task 在取得写租约前拒绝。
`owned-workspaces`/`closeout` 保持只读。配置关闭时默认保留，不提供“force”跳过门禁。
可运行的完整跨进程夹具见 `tests/s16_cli_smoke.py`，只在自建临时目录里执行。

## 7. 存储与信任边界

009 事务升级已知非空 406 到 407，保留 schema/004–008 的原始字节。新增
`owned_cleanup_metadata`、`owned_workspaces`、`workspace_cleanup_runs`。身份/清单/证明
首次写入后不可变，更新须推进 state_version；迁移哈希不匹配拒绝。CLI 使用新资产；旧版本
库接口仍可读取其支持的旧格式，但不接管不认识的 407。ReadOnly 不执行升级。

Store 是受信宿主 API。文件观察由 cleanup_runtime 调用 Linux 模块后传入，不向模型暴露
“提交任意证明即删除”的接口。父/资源 flock 保护合作进程；任意同 UID 宿主能够直接修改
数据库或与路径操作竞争，不在本阶段强隔离保证内。不能把检查点哈希称为外部身份认证，
也不能把单机临时目录测试当作真实部署的整机掉电、恶意同 UID 或强沙箱验收。

## 发布验证补充

初次开发分支CI未完成，故障注入夹具在非root环境下先被交付包的只读权限拒绝。
修复只涉及临时测试目录/文件的显式故障注入，恢复原模式后再检查运行时；生产发布和清理
权限不变。完整门禁按非root用户重跑，并验证成功清理不改交付包字节及只读模式。
见[发布复验报告](../reports/s1-owned-cleanup-followup-validation.md)。

## 8. 下一依赖

S1 仍需：私有行 purge 与独立残留清单、失败成果保留政策、原执行器退出证明的强环境正向
验收及残留 cgroup 接管、本地 wlsd/API、真实协议适配与获授权供应商测试。
S2 的独立审核/DAG/审核容量，S3 的记忆版本闭环，S4 的 MCP/评测和 S5 的按证据扩展仍按
[路线图](../ROADMAP.md)推进，不因本批工作目录删除通过而改为完成。
