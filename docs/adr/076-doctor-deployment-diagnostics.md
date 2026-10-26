# ADR-076：doctor 的实际部署路径与有界阻塞诊断

日期：2026-10-26。状态：**决定已固定，实施中；实测结果另记**；目标增量 **R2a.3**；蓝图 v0.4.1。
承接 [R2a.2 分配恢复契约](../implementation/R2a-allocation-recovery.md)。本文件提出下一增量，
不把新增报告字段、原因代码、测试或部署能力列为当前成果。

## 1. 问题、现状与本增量目标

当前 [CLI doctor](../../src/cli.cpp) 从显式配置与可选宿主 manifest 得到
`worker_sandbox_policy`，调用 [sandbox_doctor](../../src/sandbox_posix.cpp)。它不打开 Store、
不执行模板环境检查，也不运行进程。已有 `programs_executed=0`、
`namespace_execution_tested=false`、`process_isolation_verified=false` 等边界必须保留。
现有强执行与原始进程证明的含义见 [S1-worker-tools](../implementation/S1-worker-tools.md)
及 [S1-process-settlement](../implementation/S1-process-settlement.md)。

当前缺口是部署失败不容易定位到具体输入：launcher 观察主要显示 SHA，运行文件主要显示
destination，部分错误只保留一条通用异常；一个运行文件出错还会中断后续文件的报告。
用户无法从这些信息可靠区分“没有提供 manifest”“实际检查了默认路径”“指定文件不可读”
“冻结哈希不匹配”与“已发现 cgroup v2，但指定子树尚未满足委派前提”。

本增量只完善现有 doctor 的输入来源、实际路径、逐项原因与下一步说明。保持当前命令形状、
manifest v1、`wls-worker-execution-v1`、冻结配置和工具授权；不新增 SQL 迁移，不改工作区绑定。
既有布尔值和 `issues` 保留，增加可供程序读取的诊断结构。不得借此安装软件、修改权限、
启用控制器、运行 launcher、调用真实模型、开启付费请求或启动服务。

## 2. 输入来源与命令边界

沿用现有入口，例如：

```text
wls doctor --source-root /path/to/wls861 --config /path/to/system.yaml \
    --functions-dir /path/to/functions --prompts-dir /path/to/prompts \
    --function general --template default --sandbox-manifest /path/to/sandbox.json
```

以上路径为占位示例。省略参数继续使用当前 CLI 的默认选择，报告必须明确说明哪些值来自默认。
本增量的 doctor 仍只诊断当前显式配置，不新增 `--db`、`--request` 或接管入口；也不能把
本次 `live_config().freeze(...)` 的哈希描述成某个既有请求的原冻结配置。既有团队是否使用同一
manifest 仍由原 execution policy 指纹与绑定规则判断，doctor 不为团队重新绑定。

报告须分别保留：

| 来源 | 拟报告内容 | 不得推断 |
| --- | --- | --- |
| 配置 | 实际 config/functions/prompts 输入路径、默认来源、function/capability/template、本次配置快照 SHA | 配置形状合格不代表所有配置控制已执行 |
| 宿主 manifest | 是否提供、原输入路径、实际读取路径、原始文件字节 SHA、schema_version | 没有 manifest 时不能虚构一份已批准清单 |
| launcher | 实际 `bwrap_path`，默认时明确为 `/usr/bin/bwrap`，预期与观察 SHA 分列 | 观察到的 SHA 不能自动写回清单或变为冻结许可 |
| 运行文件 | 每个 source、sandbox destination、预期与观察 SHA、读取字节数 | destination 是沙箱内路径，不是已存在的宿主文件 |
| cgroup | 实际 `cgroup_root` 或明确未配置；缺配置时另列 `/sys/fs/cgroup` 的层级观察 | 层级可见不代表已获得专用可写子树 |

相对输入路径可报告其绝对词法解释；只有实际打开并验证后才报告对象身份。缺失、不可读、
被拒绝或含符号链接的路径，不得通过 `canonical`/`weakly_canonical` 成功字符串伪装成
已经检查的对象。执行源仍复用原逐级无符号链接路径规则，不能为改善诊断放松执行入口。
manifest 原字节 SHA 与 execution policy SHA 含义不同，必须使用不同字段。
不输出配置正文、凭据、环境变量值或运行文件正文。

## 3. 报告结构、原因与兼容

拟增加独立报告格式标识 `report_format: "wls-doctor-report-v1"`。它是诊断格式，
不提升 manifest、数据库、工具或执行策略的版本。原布尔字段、`issues` 与原 evidence 继续存在；
原含义不能偷偷改为一次运行验收。新结构至少包含：

- `inputs`：上述来源与实际路径，区分 `supplied`、`default`、`not_configured`。
- `checks`：稳定 ID、对象种类、path/destination、检查阶段、状态、稳定原因代码、
  `required_for_prerequisites`、预期事实、实际观察、简短说明和所需人工动作。
- `complete` 与 `not_checked_count`：明确是否因某阶段错误或预算不足而没有完成检查。
- `limitations`：仍未执行的 namespace/挂载/网络/业务进程/后代退出证明。

逐项状态为 `observed_ok`、`blocked`、`unavailable`、`not_checked`。
`observed_ok` 仅指本项只读前提得到支持；必需项 unavailable/not_checked 不能让
`strong_prerequisites` 成为 true。独立可检查项应继续产生结果；前置项失败后的依赖检查须
写 not_checked 并指向原因，不能补一个成功值或把后续文件悄悄漏掉。
未来 operation 子目录与真实隔离运行属于明确未执行的验收项，不标为本次只读前提的必需检查；
辅助 proc 观察也不擅自提升为隔离正向证明。哪些项属于必需前提须与原执行器逐项对应并固定。

拟议原因代码示例：

| 类别 | 稳定代码示例 | 指向的具体事实 |
| --- | --- | --- |
| 来源/策略 | `manifest_not_configured`、`manifest_invalid`、`policy_unsupported` | 清单缺失、输入不合格或冻结网络/隔离模式不受执行器支持 |
| 文件 | `path_missing`、`path_unreadable`、`symlink_refused`、`file_type_refused`、`owner_mode_refused` | 精确 source/launcher 及失败阶段 |
| 固定字节 | `expected_hash_missing`、`hash_mismatch`、`source_changed`、`read_budget_exceeded` | 预期与实际分离；检查中变化不算通过 |
| launcher | `launcher_not_elf`、`launcher_not_executable` | 原 root 所有、非 setuid/setgid、权限和 ELF 条件 |
| 运行映射 | `destination_refused`、`destination_duplicate`、`runtime_manifest_empty` | 原白名单与完整显式清单条件 |
| cgroup | `cgroup_not_configured`、`not_cgroup_v2`、`mount_read_only`、`delegation_access_unavailable`、`controller_missing`、`not_domain` | 指定根与具体控制文件，缺哪个 controller 必须列明 |

代码与结构化字段由实际检查分支生成，不从英文 `what()` 文本猜测 errno 或错误类别。
保留有界原错误文本作为辅助，缺 errno 时明确为 null。`next_step` 只说明应由部署人员
确认的文件、哈希或委派条件，不生成可执行安装/提权/控制器修改脚本，也不自行执行修复。

完成一次含阻塞项的诊断继续沿用当前 doctor 的成功返回码 0；调用方查看报告字段决定是否继续。
CLI 参数、配置或 manifest 解析失败沿用现有非零错误协议，不为兼容结构化报告把无效输入
改报为通过。诊断格式的解析失败与某个已识别对象的检查失败需要在测试中区分。
若在 doctor 专用错误输出附加 `reason_code` 和输入路径，必须保留原 `error`/`message`
及返回码；不得为输出完整报告而继续使用解析失败的 manifest 或静默改用默认策略。

## 4. 复用实际执行约束，避免副作用探测

launcher 与运行文件共享 [现有文件校验](../../src/sandbox_posix.cpp) 的路径、普通文件、
UID/mode、大小、SHA 和前后身份变化规则；doctor 使用只读校验分支，不创建 sealed memfd。
launcher 不执行 `--version`，版本继续明确为未执行。运行文件不通过 `ldd`、shell 或任意
模板检查程序补全依赖；清单内所有文件检查通过仍不能证明动态加载器及运行时依赖齐全。

指定 cgroup 根的只读检查及展示必须精确对应原执行器：

| 实际路径/角色 | 允许观察 | 不能由此证明 |
| --- | --- | --- |
| `cgroup_root` | 文件系统类型、只读挂载标志、当前身份的访问权限观察 | 将来 mkdir 一定成功或获得独占委派 |
| `cgroup_root/cgroup.subtree_control` | 有界读取已启用的 cpu/memory/pids | doctor 已启用任何 controller |
| `cgroup_root/cgroup.type` | 有界读取是否为 domain | 新 operation cgroup 已正确创建 |
| `cgroup_root/cgroup.procs` | 当前身份的写权限检查，不写入也不迁移进程 | 子进程已经加入、隔离或退出 |
| 新 operation 子目录中的控制文件 | 只报告派生路径模式及运行时用途，状态 not_checked | 文件已经存在、可写，或限额已经施加 |

最后一行的路径模式由当前实现产生：
`cgroup_root/wls-op-<sha256(operation_id) 的前 32 位>/...`。
涉及 `memory.max`、`memory.swap.max`、`memory.oom.group`、`pids.max`、`cpu.max`、
`cgroup.kill`、`cgroup.procs` 和 `cgroup.events`。doctor 没有 operation_id，也不创建该目录，
所以必须标为 `path_kind: "operation_child_pattern"`；不能编造一个“实际已检查”的子目录。
不能遍历既有 operation cgroup、杀进程、清理残留或把当前空组观察变为原进程退出证明。

继续有界读取现有 user namespace/AppArmor 相关 proc 文件作为辅助观察；缺失、不可读或值未知
分别报告。它们不是对未来 `unshare`、bwrap、挂载或网络隔离成功的证明。所有执行入口仍须
在真实运行时重新验证配置、字节、身份、隔离屏障、网络、工作目录及后代退出；doctor 报告
没有可被 Store 消费的授权令牌，也不得解除 UNKNOWN 或绕过首派发和结算门禁。

## 5. 有界成本与“只读”的精确定义

沿用 manifest 256 KiB、运行文件最多 128 个、单文件最多 64 MiB、运行文件总量 256 MiB
以及原路径规则。launcher 单独受原 64 MiB 上限约束。实施时为批量读取建立共享剩余预算，
不能以“先读完再报超限”无界消耗 I/O。cgroup 文本每项最多 16 KiB，既有 proc 辅助项最多
128 字节；保留原配置加载的文件/总字节/节点/深度边界。

新报告最多 512 个检查项、每项最多 8 条原因、每条辅助错误文本最多 512 字节、编码总量
最多 2 MiB。预算不足应产生明确 incomplete/not_checked 结果和计数；不得截断后声称完整。
不遍历系统目录寻找 bwrap、运行库、cgroup 候选或用户文件；只检查显式输入和原固定辅助路径。

这里的只读表示不主动写文件、数据库、权限、cgroup/sysctl，不执行程序，不领取租约、
不推进 epoch、不创建资源、不启动网络请求。普通读取可能触发文件系统自身 atime 变化，
因此不能承诺所有宿主文件系统元数据逐字节静止。首增量根本不打开 Store，不应生成 writer-lock、
SQLite 边文件或恢复事件。

## 6. 所有权分配诊断的边界

首增量沿用已有 `owned-workspaces`、`closeout.retained_allocations` 查看持久历史，
不把分配扫描塞入新的部署 doctor。ABANDONED 的 path 是保留意图，
`filesystem_observed:false` 不是“存在”或“不存在”的观察。

若后续单独设计请求限定的 doctor 模式，只能使用 Store ReadOnly，并把 `retained_record`
与带本次时间/资源 CAS 的 `current_observation` 分开。观察不能补写 allocation_receipt、
采用目录、放弃资源、执行 init 或重新解释旧 inventory/proof；当前路径缺失也不能代替
清理所需的原始证据。该模式、对象枚举上限和原证据/即时观察的具体格式不属于本增量验收。

## 7. 实施步骤和验收

1. 先固定报告结构、原因代码和版本兼容规则，从实际 sandbox 校验分支产生诊断；不要先做
   一份与执行器逻辑分叉的检查器。保留当前 CLI/宿主策略语义及原报告关键字段。
2. 接通 manifest/config 来源和实际路径；按对象汇总结果，在一个文件失败后继续其余独立项。
   输出必须在普通用户、两种 umask 下相同地守住权限边界。
3. 补回归与实施报告后再更新 STATUS/ROADMAP。只读诊断完成后继续 R3 单协议离线回放；
   强沙箱部署正向验收仍是独立任务，不由此 ADR 自动通过。

验收至少包含：

- 未传 manifest、明确默认 launcher、不同真实输入路径、不可读/缺失清单、格式错误；
  报告来源与本次配置 SHA 准确，不声称读取了旧请求快照。
- launcher 缺失、非 ELF、权限/所有者不合格、冻结 SHA 缺失/错误、校验中替换；
  runtime source 与 destination 分列，单个文件失败不隐藏其他文件，不输出正文。
- 禁止路径、符号链接、重复 destination、白名单外目标、128 项/单文件/总量/报告预算边界；
  超限或未完成项不能成为 prerequisites 通过。
- 普通目录伪装 cgroup 文件、非 v2/只读/访问不足/缺 controller/非 domain；
  路径与具体原因相符，层级探测与实际委派条件分开。环境无法构造的 cgroup 条件明确标为
  固定输入或内部分支夹具，不能改生产检查以制造通过。
- 使用有标记副作用的假 launcher/环境检查输入，验证 doctor 没有执行；同时验证无新目录、
  文件内容/权限修改、进程迁移和网络请求。故障注入只作用于临时夹具。
- 数据库写者正在持有锁时 doctor 仍无需接管；对照数据库全表、原事件和配置快照不变，
  也不把未传入的数据库路径作为诊断目标。
- 新旧关键 JSON 字段、成功诊断/无效输入返回码、完整与未完成报告、失败后续独立项，
  以及 GCC/Clang/ASan/UBSan 和原套件回归分别记录。

即使所有只读前提都通过，报告仍须明确 `programs_executed=0`、
`namespace_execution_tested=false`、`process_isolation_verified=false`。
真实强进程测试必须另行保留启动屏障、挂载/命名空间、断网、限额、原输出与所有后代退出
等原始证据；拒绝路径、只读观察、固定测试数据和同模型复核均不能代替这一正向验收。
