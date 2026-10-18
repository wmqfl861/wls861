# S1.2 Worker 工具实现契约（0.3.0-s1.2）

更新：2026-10-18（UTC）。目标蓝图仍为 [v0.4.1](../spec/v0.4.1/01_实施规格_v0.4.1.md)，开发基线为 `c0206fcd02a35f83218691cd34c50637621e88d1`。本增量沿 S1-06/07 接入真实受控文件工具、强进程执行入口、持久结果和控制分支。实际测试与未完成的验收见 [S1.2 验证报告](../reports/s1-worker-tools-validation.md)。

[S1.1 契约](S1-foundation.md) 继续说明原基础层；本文件说明新增行为及其替代的范围。原 `wls861` 0.1.1-m1、Journal、两套交接档案和历史报告保留。`wls` 使用 FakeModel，真实供应商调用仍为零。文件操作成功、进程退出零、数据库 `accepted=1` 均不等于业务验收或交付，输出继续为 `business_delivered=false`。

## 1. 本批入口与范围

| 入口 | 行为 |
| --- | --- |
| `wls run-fake`，未指定 workspace | 保留一个独立任务及持久 note 的兼容路径；按当前状态最多 Dispatcher/TeamLead/Worker 各一次，已有当前任务可跳过 Dispatcher，取消任务不进入后续角色 |
| `wls run-fake --workspace DIRECTORY` | 宿主显式选择现有目录，绑定团队；声明当前获准且已有实现的 Worker 工具 |
| `--worker-calls N` | 一次显式运行允许 1–16 次 Worker Fake 调用，默认 1；每次仍独立预约、结算及检查工具权限/版本 |
| `--sandbox-manifest FILE` | 可选受信宿主部署输入，固定运行文件、SHA-256、cgroup 路径和资源上限；不来自模型参数 |
| `wls tool-query` | 只读 Store 和只读 Workspace，调用注册的文件诊断、op_status 或 op_output；不迁移、不接管、不提升代次 |
| `wls doctor` | 只读观察 Linux/bwrap/cgroup/配置文件等前置条件；不运行模板检查、不安装依赖、不修改系统政策 |

当前仍是同步、有界、单任务宿主流程。持久事件消费、管理角色循环、跨进程取消通道、后台 run、恢复后的完整对话重建、DAG、业务成果包、回执、经验和清理作业还没有接入。`ask/progress/submit` 与 memory 工具不在本批静态实现中，不会向模型声明。

同次运行的每个后续 Worker 上下文保留之前的 `role_call_index`、`attempt_id`、完整模型响应（含工具名与全部参数）和完整工具结果。诊断结果无需自行重复全部输入；原调用信封提供对应关系。所有反馈仍受上下文总容量限制，超限明确拒绝，不静默截断。

## 2. 静态工具与信任边界

| 分类 | 工具 | 副作用与约束 |
| --- | --- | --- |
| 业务 | `note` | SQLite 持久说明；`side_effect=none`，不是文件或业务交付 |
| 业务 | `fs_write`、`fs_patch` | `side_effect=workspace`；准备意图、原子派发领取、文件操作和持久结果 |
| 业务 | `run` | `side_effect=workspace`；仅强隔离路径，argv 明确、不隐式解析 shell |
| 只读诊断 | `fs_list`、`fs_read`、`fs_search` | 有界目录观察、分页读取、字面搜索；不消耗业务操作额度 |
| 只读诊断 | `op_status`、`op_output` | 本团队操作与原结果、stdout/stderr 无损分页；没有任意命令执行能力 |
| 控制 | `ack_change`、`op_cancel` | 独立身份、授权、状态和精确重放检查；不要求普通业务已 ready |

宿主签发 `StoreActor` 和原始十字段 ExecutionBinding，模型只能填写各工具的严格业务参数。模型不能提交身份、类别、side_effect、宿主工作目录、挂载、环境变量或 cgroup 参数。所有业务调用最终取冻结模板、请求 allowlist 和当前撤销政策的交集；声明工具时也应用当前政策，撤销一个工具不会让其它获准工具因过期声明而一并失效。

新业务在 prepare 和首次 start 各核一次当前 tuple、需求/计划/任务/说明应用、AMEND 处置、暂停、UNKNOWN、权限及预算。诊断/控制有单独的权限与限额；当前实现按团队在进程内共享一分钟窗口，最多保存 4096 个窗口。它不是跨多个 CLI 进程的全局限速服务；数据库所有者的本机 CLI 也不是远程身份认证接口。

控制精确重试保留原 actor、binding、expected version、参数和 command_id。Store 先识别已成功的同一命令，再允许无效果重放；只有新控制动作需要当前完整版本。旧 ack 重放不会应用后来发布的新说明。取消重复请求不重新读取一个新 op version 来改变原命令身份；换参数、越团队或撤销权限仍被拒绝。

## 3. 工作目录与文件边界

`Workspace` 在宿主选定的现有目录上固定 canonical path、device、inode。Store 第一次绑定后禁止改路径、换 inode、改执行策略；同一 Store 的不同团队不得绑定相同、嵌套或同 inode 的目录。控制数据库及其租约、WAL/SHM 不允许出现在本 Store 绑定的业务目录内。不同数据库之间的全机目录登记和外部租户隔离仍属于部署层。

每次文件操作重新检查根目录身份，按 dirfd + `openat2` 的 `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV` 解析模型路径。拒绝绝对路径、`..`、内部 `.`/空组件、符号/魔法链接、跨挂载、硬链接和特殊文件；缺少相应内核调用直接拒绝，不退回字符串拼接。目录/普通文件必须属于当前 UID，且 group/other 不可写；文件必须只有一个硬链接。

本批文件读写和搜索由宿主中有界的 C++ 文件代理完成，权限由上述目录句柄与检查限制；它们没有启动 bwrap 子进程。进程工具另走强沙箱。两类工具使用同一团队目录和合作 `flock`；不把文件代理的通过说成任意命令的 OS 隔离通过。任意同 UID 宿主忽略合作锁、移动目录或修改 inode 的攻击，不在这套单用户宿主信任边界内。

### 文件容量与分页

| 边界 | 当前上限（未另外说明时） |
| --- | ---: |
| 被读取/修改的单个文件 | 16 MiB |
| 文件工具写入后的逻辑总容量 | 256 MiB |
| 路径长度 / 深度 | 4096 bytes / 32 层 |
| 枚举范围 | 20,000 个条目 |
| Worker 写入/替换参数 | 每段最多 65,536 bytes，另受严格 JSON 总容量限制 |
| Worker 读取页 / 列表页 | 65,536 bytes / 256 项；列表默认 100 项 |
| 搜索范围 | 最多 1,000 个文件、16 MiB 内容 |
| Worker 搜索返回 | 最多 100 个匹配，字面字节搜索 |

完整 WorkspaceLimits、额外工具上限及实现版本进入团队 execution_policy SHA。总逻辑字节限制不是磁盘配额，不能约束任意宿主程序或 sandbox 进程的磁盘写入；原子暂存期间还会额外占用一个受写入上限限制的副本。

`fs_read` 返回完整文件 SHA-256、当前页 offset/next_offset、`content_base64`；该页不能作为 UTF-8 展示时 `content=null`。跨页是实时观察，调用者需比较 SHA，不能当成持久快照。搜索发现超过返回上限的匹配时明确 `complete=false/truncated=true`；遍历范围超预算直接拒绝，不悄悄漏文件。

### 创建、覆盖和 patch

`fs_write` 不传 `expected_sha256` 表示只允许新建；已存在文件拒绝。覆盖必须匹配完整旧 SHA。`fs_patch` 同时要求完整旧 SHA、明确字节 offset、该位置的 expected 原字节，以及整个文件内唯一匹配。父目录必须已存在，本批不隐式创建目录。

暂存文件由运行时独占创建，写入后同步文件，再用 `renameat2` 提交并同步父目录。根/父目录、目标和暂存 inode 在提交边界再次检查。只有完整成功才返回 `Succeeded`。rename 前失败且已确认自有暂存文件清理后为 `EffectNotStarted`；rename 尝试后的失败，或不能证明清理完成，保守为 `OutcomeUnknown`。暂存路径进入证据，后续清理执行器尚未接入，未知残留不会被假称已清除。

## 4. 操作意图、唯一执行权与原结果

业务先写 PREPARED、参数哈希、business key 和完整原始 tuple。`start_tool_operation` 以数据库事务返回 `dispatch_claimed`：只有首次 PREPARED→RUNNING 得到 true，命令重放与已有领取均为 false。WorkerTools 只有取得 true 才执行；同 key/同 command 的并发请求不因“重放了 RUNNING 结果”而执行第二次。

当前有权 Worker 可以用相同 business key、相同工具和参数读回自己团队的旧操作，即使当前说明或执行代次已经变化。返回的是原操作及原结果，`replayed=true/executed_now=false`；参数或工具改变仍冲突。重试 UNKNOWN 也不会触发第二次副作用。`run-fake` 的新显式模型 attempt 是新业务尝试，不会跨两次 CLI 调用自动复用旧业务 key；文件覆盖条件仍独立生效。

文件/进程返回后，Store 原子保存结果、证据、原始十字段 binding、原始 stdout/stderr、哈希、计数、截断标记和事件。`operation_results.accepted` 只说明录入时原绑定仍有资格且结果不是 UNKNOWN；它不表示业务验收。AMEND、取消或恢复已使操作 UNKNOWN 时，迟到事实保留，但不会自动解决 UNKNOWN，也不会重新盖成当前版本。

结果持久化若失败，WorkerTools 立即阻断当前团队/代次的新业务，尝试写小型 UNKNOWN 事实或 UNKNOWN 状态；即使数据库完全不可写，同进程共享故障标记仍保留。首次派发领取与结果保存/故障登记使用同一准入互斥边界，早先通过检查但尚未领取的并发调用不能绕过后来生效的围栏。查询/控制可用，重开时仍按 Store 恢复规则围栏未决操作。此保护不是物理磁盘预留或断电保证；完整恢复和人工/宿主核对仍必需。

## 5. 强进程入口与 doctor

生产 `ProcessRunner` 没有不隔离执行入口。Worker `run` 接收绝对 sandbox argv[0]、明确 argv、相对 `/work` 的 cwd、1–600 秒超时和有界输出；WorkerTools 再收紧到冻结系统上限。shell 只有在清单明确提供对应二进制、且 argv 明确调用它时才可能执行，权限与其它 run 相同。

宿主 manifest 的字段如下；省略 manifest 时文件工具仍可用，run 会拒绝缺少强执行前提：

| 字段 | 含义 |
| --- | --- |
| `schema_version` | 当前为 1 |
| `bwrap_path` / `bwrap_sha256` | 显式 launcher 文件与冻结 SHA；必须是 root 拥有、非 setuid/setgid、不可被 group/other 写的 ELF |
| `cgroup_root` | 已由宿主准备、可委派且启用 cpu/memory/pids 的独立 cgroup v2 domain；不允许直接用层级根 |
| `runtime_files` | 最多 128 个 `{source,destination,sha256}` 普通文件；不接受目录树挂载 |
| `memory_max_bytes` / `pids_max` | 不得超过请求冻结的资源上限；还受执行器自身边界限制 |
| `cpu_quota_us` / `cpu_period_us` | 有界 CPU 带宽参数 |
| `cleanup_timeout_ms` / `max_output_bytes` | 收尾等待上限、保留 stdout+stderr 的合计上限 |

运行文件来源逐级拒绝符号链接，先检查字节与 SHA，再复制到 sealed memfd 后以只读文件挂载，避免检查后宿主原路径改指向别的内容。目标限于 `/bin`、`/usr/bin`、库目录及少量明确 `/etc` 文件；不挂载宿主 home、整个 `/etc`、控制数据库或 cgroup 文件系统。

非默认 manifest 的团队，后续 `run-fake` 和 `tool-query` 仍需提供产生相同 execution_policy 的 manifest；CLI 会比对已冻结内容，不自动从绑定生成部署文件。要使用不同清单需新的受控团队/工作目录或后续显式迁移机制，不能在原团队上悄悄换版本。

进程进入每个 operation 的独立 cgroup 后才 exec launcher，设置 memory、禁 swap、pids、CPU；清单、版本哈希和资源设置进入证据。`/work` 是与文件代理一致的目录，`/tmp` 和临时 HOME 独立，`/input` 当前为空的只读目录，没有真实输入目录挂载。envp 由宿主从零构造，WorkerTools 仅提供固定 PATH/LANG，runner 设置 HOME/TMPDIR；不继承宿主密钥或允许模型注入加载器环境。

launcher 通过固定 fd exec，关闭非白名单 fd。bwrap 建立 mount/user/PID/IPC/UTS/network/cgroup 命名空间，禁嵌套 user namespace、丢弃能力。业务启动屏障释放前，宿主核对实际 `/work` inode 和七类 namespace，拒绝读取不到的证明；仅 exec 成功或输出 child-pid 不等于隔离成功。最终另保留 bwrap setup/exit 记录，缺证明不会报告成功。

挂载整个目录前，runner 在合作锁内以 dirfd/statx 审核现有树，最多 20,000 条目、32 层及 4096 字节路径；拒绝预存硬链接、符号链接、FIFO/socket/device、非当前 UID 或 group/other 可写的对象，以及不同 mount ID。bwrap 的目录挂载还需在启动屏障内核对实际 mountinfo：`/work` 必须是唯一的 private、rw/nosuid/nodev 挂载，不允许子挂载或 shared/master/propagate_from 传播。树审计和屏障证明均失败即拒绝；不会修改宿主挂载传播。放行前再次检查取消和截止时间。

stdout/stderr 有界保留，达到可保留范围外的输出会终止操作并标记截断。取消/超时/异常通过 cgroup.kill 与受控 launcher 的 SIGKILL 请求停止，并等待 `populated=0` 和直接子进程回收；本批没有先 TERM 的宽限阶段。单个 PID/进程组退出不能代替后代证明。root 实例的合作锁和同实例忙状态避免同目录重叠进程。

最后一次后代查询失败时，不能复用先前的空组观察；证据写为未确认并保留 UNKNOWN。fork 后异常展开（例如分配失败）目前只尽力终止 cgroup，不能据此声称直接子进程及全部后代已回收；对应持久 UNKNOWN 和后续恢复责任仍保留。对象销毁会关闭目录 fd，其合作锁不能替代跨进程恢复围栏。

只有退出零、强隔离证明与全部后代退出均成立，WorkerTools 才保存 SUCCEEDED。任何已产生子进程的其它结果保守为 UNKNOWN，包含超时、取消、非零码、输出上限及无法确认收尾；发现原 op_id 残留 cgroup 或其它 CleanupUnknown 即使本次没有新 spawn，也保存 UNKNOWN。确定未启动且没有残留不确定性的拒绝可保存 FAILED。进程退出证明不等于副作用回滚。cgroup 名可由原 op_id 追踪，但启动后扫描/终止全部历史残留 cgroup 的恢复执行器仍待完成。

doctor 只报告前置条件。`strong_prerequisites` 不是一次 sandbox 成功运行，`process_isolation_verified=false`、`programs_executed=0` 明确保留；内核支持 cgroup v2 与已经获得可写委派分开报告。本批环境未完成强模式正向验收，不能由拒绝路径或内部监督测试推断 SBX-01/SEC-01/V3-020–023 整体通过。

## 6. SQLite 403 与兼容

CLI 写者传入已分发的 `005_worker_tools.sql`，在已知 402 数据库上以事务升级到 403；非空 S1.1 请求、快照、调用和操作保留。新增 workspace_bindings、operation_results、operation_output_bytes 和迁移指纹。迁移失败整体回滚；403 打开必须匹配 005 指纹。原 `schema.sql` 与 004 资产字节不改。

只读连接不迁移已有 402；原查询仍可读取，新增工具持久化要求先由显式写者升级。旧 C++ 调用者不传 005 时继续使用 402 行为。缺完整冻结快照的非空 401 参考库仍拒绝自动迁移，不能把 402→403 测试说成所有旧数据迁移已解决。

原始输出单独以 BLOB 保存，不经 UTF-8 改写。每 stream 存储最多 1 MiB，S1.2 Worker run 进一步把合计限制到最多 64 KiB；`op_output` 每页最多 64 KiB，返回 base64、offset、next_offset、total_bytes、eof 和截断标记。结果 JSON 不重复塞入全部展示文本/base64，避免控制字节转义后无法结算。

## 7. 后续任务与验收门槛

S1-06/07 继续补强环境的实际正向命令、凭据/网络/路径/后代逃逸测试、完整恢复残留 cgroup、真实输入目录与资源清理；无能力的环境保持明确拒绝。本批内部固定程序监督测试只验证 pipe、exec 错误、超时/取消与回收机制，不作为弱生产模式。

下一开发增量沿 S1-03/09/10 接持久事件消费、Worker 等待/唤醒、跨调用历史与多任务 AMEND；再接 S1-11/12 的成果包、验收回执与可恢复收尾。真实协议和花费继续等对应配置与实现。每项能力按实际测试独立记录，不把文件已写入或 Schema 合格替代完整 S1 产品验收。
