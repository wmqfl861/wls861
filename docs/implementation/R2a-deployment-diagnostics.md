# R2a.3：实际部署路径与有界只读诊断

集成版本 **0.8.5-r3.2** 中的 **R2a.3** 节点；蓝图 **v0.4.1**；依据
[ADR-076](../adr/076-doctor-deployment-diagnostics.md)。本契约描述已落入源码的诊断接口、
执行边界与测试设计，不预先声明测试通过或真实部署验收通过。各环境的实际执行成绩另记于
[验证报告目录](../reports/)。

`wls doctor` 检查当前显式配置及宿主 manifest 指定的部署前提，输出
`wls-doctor-report-v1`。它不打开 Store，不执行 launcher、模板检查或业务程序，不写 cgroup，
也不为后续执行发放许可。实现入口见 [CLI](../../src/cli.cpp)、
[SandboxDoctor 接口](../../include/wls/sandbox.hpp) 与
[Linux 检查实现](../../src/sandbox_posix.cpp)；有类型原因与共享预算见
[内部诊断原语](../../src/sandbox_doctor.inc)。

## 1. 命令、配置选择与返回码

沿用原命令形状。仅检查默认配置与默认部署输入：

```sh
wls doctor --source-root /path/to/wls861
```

显式选择配置、模板与宿主清单：

```sh
wls doctor --source-root /path/to/wls861 \
    --config /path/to/system.yaml \
    --functions-dir /path/to/functions \
    --prompts-dir /path/to/prompts \
    --function general --template default \
    --sandbox-manifest /path/to/sandbox.json
```

以上路径均为占位示例。`--capability` 仍可显式指定配置中存在的能力；省略时记录本次
配置冻结实际选中的 capability。默认 `source-root` 是当前目录，system 为
`source-root/config/examples/system.yaml`，functions 为选定 system 文件同目录下的
`functions`，prompts 为 `source-root/prompts`，function 为 `general`，template 为 `default`。
修改 `--config` 会影响默认 functions 目录，不能把所有默认路径都解释为相对 source-root。

doctor、encode-model、replay-model 在解释选项和读取输入文件前，先校验命令名之后的每个
argv 参数：最多 4096 字节，且必须能作为合法 UTF-8 JSON 字符串表示。doctor 的 source-root、
配置目录/文件及 manifest 路径，还在各自读取前校验非空、原路径/绝对词法路径各不超过
4096 字节及 UTF-8 可表示性；无效参数不会先进入文件读取后再使错误报告无法编码。
这是 CLI 的文本接口限制，不把 POSIX 本身的路径字节规则改为 Unicode 规则。

命令不接受 `--db` 或 `--request`。doctor 分支在数据库命令、SQL 资产读取和 Store 构造前结束；
它不会打开数据库、取得写者锁、创建 SQLite 边文件、接管请求或执行迁移。加载的是本次
`live_config().freeze(...)` 的配置，报告中的配置 SHA 不能代表某个既有请求的原冻结快照。

| 情况 | 输出与退出行为 |
| --- | --- |
| 成功生成诊断，包括有阻塞项或未完成项的报告 | stdout 输出 JSON，返回 0；调用方须读取报告状态 |
| CLI 参数、配置、manifest 解析或策略构造失败 | 沿用 stderr 的 `error`、`message` 和原非零错误协议；不改用默认清单继续诊断 |
| 原 `Error` 异常 | Stale/Conflict 返回 3，其余返回 2 |
| 其他未捕获异常 | `error: runtime_error`，返回 1 |

文件缺失、哈希冲突等已经进入逐项检查的失败会保留在报告内；不能根据进程退出码 0 推断
`strong_prerequisites=true`。manifest 的 JSON/字段错误发生在逐项检查之前，仍是命令错误。
例如 CLI 的运行文件条目必须提供合法 `sha256`；库调用自行构造缺少冻结哈希的 policy，
才会走对应的逐项 `expected_hash_missing` 分支。

## 2. 输入来源与三个不同的 SHA

CLI 的 `inputs` 包含下列对象。`origin` 根据参数或 manifest 字段是否显式出现判定，
不会因为一个显式值恰好等于默认值就改记为 `default`。例如明确提供
`bwrap_path: /usr/bin/bwrap` 是 `supplied`；明确提供空 `cgroup_root` 也是 `supplied`，
但其路径仍未配置，检查仍会阻塞。

| 对象 | 主要字段与含义 |
| --- | --- |
| `config` | `source_root`、`system`、`functions`、`prompts` 的来源和路径；`function`、`capability`、`template` 的来源与实际选择；`snapshot_sha256`、`snapshot_scope: current_explicit_configuration` |
| `manifest` | `origin`、`input_path`、`absolute_lexical_path`、原文件 `raw_sha256`、原字节数 `bytes`、输入中的 `schema_version` |
| `launcher` | 来源、实际 `path`、`default_path: /usr/bin/bwrap`、`expected_sha256` 与路径显示标记 |
| `runtime_files` | 来源、`provided_count`、`omitted_count`、有序 `items`；每项保留原 index、source、destination、expected_sha256 和路径显示标记 |
| `cgroup` | 来源、实际 `path` 或 null；未配置时另列固定 `hierarchy_observation_path: /sys/fs/cgroup` |

未提供 manifest 时，`origin=not_configured`，路径、原始 SHA、schema_version 为 null，
字节数为 0。提供一个空 JSON 对象仍是实际读取的输入，保留其原始字节 SHA；不把它伪装成
已经批准的完整部署清单。示例 [sandbox-manifest.json](../../config/examples/sandbox-manifest.json)
本身没有冻结 launcher 哈希、运行文件或委派根，不能当作就绪部署的正向样本。

配置和 manifest 路径保留 `input_path` 与 `absolute_lexical_path`，并标记
`path_kind: lexical_interpretation`。绝对词法解释不解析符号链接，也不证明目标存在、
可读或已获得执行许可。launcher、运行文件 source 与 cgroup 根继续使用执行器的原绝对路径和
逐级无符号链接校验；被拒绝的路径不会通过 canonical/weakly_canonical 转换成一条“已验证”路径。
运行文件的 `destination` 是沙箱内映射目标，不是另一个已检查的宿主文件。

| SHA 字段 | 绑定对象 |
| --- | --- |
| `config_snapshot_sha256`，亦见 `inputs.config.snapshot_sha256` | 本次选定并冻结的配置内容 |
| `inputs.manifest.raw_sha256` | 一次有界读取取得的 manifest 原始字节，包括空白；解析和原字节哈希使用同一份读取结果 |
| `execution_policy_sha256` | 本次 `wls-worker-execution-v1` 策略的规范 JSON；包括配置 SHA、模板、执行路径、清单、冻结哈希及限额 |

这些值各有用途。观察到某个文件的 SHA 不会自动填入 expected_sha256，不会修改配置、manifest
或既有团队绑定。报告不输出配置正文、凭据、环境变量值或运行文件正文。

直接调用 C++ `sandbox_doctor(policy)` 时，库只知道 policy 值，不知道 CLI 参数来源。
因此 launcher/runtime 的来源为 `policy`，cgroup 为 `policy` 或 `not_configured`；
CLI 在序列化后补充 config/manifest 来源，并按实际字段出现情况细化这三个来源。

## 3. 报告结构、状态与完整性

保留原 `linux_supported`、`bwrap_available`、`cgroup_v2`、`delegated_root_ready`、
`runtime_files_ready`、`strong_prerequisites`、`issues` 和 `evidence`，增加
`report_format`、`inputs`、`checks`、`complete`、`not_checked_count`、`limitations`、
`report_truncated`、`omitted_check_count`、`report_budget_reason`。
诊断格式升级不改变 manifest v1、执行策略 v1 或数据库结构 408。

每个 `checks` 条目包含稳定 `id`、`kind`、`path`、`path_kind`、`destination`、路径显示标记、
`stage`、`status`、`required_for_prerequisites`、`expected`、`observed` 和 `reasons`。
依赖项另有 `depends_on`。每条 reason 包含稳定 `code`、真实 syscall `errno` 或 null、
有界 `message`、`message_truncated` 与供部署人员判断的 `next_step`。

| status | 本项的含义 |
| --- | --- |
| `observed_ok` | 已获得支持本项只读前提的观察 |
| `blocked` | 已定位到策略、路径、类型、权限、哈希或 cgroup 条件不满足；例如路径缺失也属于此类 |
| `unavailable` | 路径不可读或探测发生 I/O/其他检查错误，无法取得所需观察 |
| `not_checked` | 读取预算不足、前置检查未建立，或本次不创建真实 operation 因而不能检查 |

`complete` 表示必需诊断是否完整表达，不表示部署合格。完整报告可以包含 blocked 或
unavailable；必需项的 not_checked、超出运行文件数量的未检查条目或报告截断会使其为 false。
`strong_prerequisites` 还要求所有必需检查均 observed_ok、原准备条件成立且不存在必需项问题；
任意必需项失败或报告不完整都不能通过。

`not_checked_count` 包括可选未执行检查，因此不能用“计数为 0”代替完整性判断。
八项未来 operation 控制默认都是可选 not_checked，它们本身不使 complete=false。
报告预算耗尽时还会记录预算标记，并保守计入被省略的检查；这个计数描述报告中未完整交代的
覆盖范围，不能反推所有省略项都从未发生过读取。`omitted_check_count` 专门记录因报告边界删去的
检查；超过 128 个运行文件的数量另见 `inputs.runtime_files.omitted_count`。

旧布尔值保留原观察含义，例如缺少冻结哈希时，文件本身仍可能成功只读校验，因而
`bwrap_available` 或 `runtime_files_ready` 可以为 true；同一项的 expected_hash_missing
仍会阻止 strong_prerequisites。层级可见造成的 `cgroup_v2=true` 也不等于已获得委派根。
调用方应结合 checks、complete 与 strong_prerequisites 解释报告。

## 4. 检查 ID 与有类型原因

| 稳定 ID | 检查对象 |
| --- | --- |
| `policy.limits` | 原执行器支持的数量和资源限额 |
| `launcher.file` | 指定 launcher 文件 |
| `runtime.manifest` | 显式运行文件清单的非空和数量边界 |
| `runtime.0`、`runtime.1` 等 | 按原清单索引逐项检查 source/destination/冻结字节 |
| `cgroup.root` | 指定的专用 cgroup v2 根 |
| `cgroup.hierarchy` | 仅在未配置根时，对 `/sys/fs/cgroup` 的可选层级观察 |
| `cgroup.mount`、`cgroup.access` | 指定根的挂载与当前有效身份权限 |
| `cgroup.cgroup.procs`、`cgroup.cgroup.subtree_control`、`cgroup.cgroup.type` | 指定根中的对应控制文件；ID 的第二个 cgroup 是实际文件名前缀 |
| `operation.<控制文件名>` | 未来 operation 子目录的八项路径模式 |
| `auxiliary.<完整 proc 路径>` | 三项固定内核辅助观察 |

实际文件校验通过有类型 `DiagnosticError` 携带 reason、stage 与可选 errno；它仍继承原
`Error`，保留原错误类别和生产拒绝行为。不会通过英文 `what()` 猜原因或 errno。
非有类型检查异常统一为 `probe_io_failed`、stage=probe、errno=null；原消息只作有界辅助。

| 原因类别 | 当前逐项稳定代码 |
| --- | --- |
| 路径与读取 | `path_refused`、`path_missing`、`path_unreadable`、`symlink_refused`、`file_type_refused`、`probe_io_failed` |
| 文件与冻结字节 | `owner_mode_refused`、`hash_invalid`、`expected_hash_missing`、`hash_mismatch`、`source_changed`、`read_budget_exceeded` |
| launcher | `launcher_not_elf`、`launcher_not_executable` |
| 策略与运行映射 | `policy_unsupported`、`runtime_manifest_empty`、`destination_refused`、`destination_duplicate` |
| cgroup | `cgroup_not_configured`、`not_cgroup_v2`、`mount_read_only`、`delegation_access_unavailable`、`controller_missing`、`not_domain` |
| 依赖和辅助观察 | `dependency_not_checked`、`operation_not_created`、`auxiliary_value_unknown` |
| 输出预算 | 顶层 `report_budget_reason.code=report_budget_exceeded` |

manifest 输入错误仍由 CLI 原错误协议报告；本实现没有把解析失败的清单变成一个可继续使用的
policy。`next_step` 只说明需人工核对的对象与条件，不包含安装、提权或修改控制器的执行脚本。

## 5. 文件检查与共享读取预算

doctor 与真实执行共享 `pin_file` 的路径、类型、UID/mode、大小、SHA 和读取前后稳定性规则。
执行源逐级使用无符号链接打开；拒绝空、点、父级、过深、过长等不合格路径。源须为普通文件，
拥有者为 root 或当前有效 UID，不得具有 setuid/setgid 或组/其他用户可写位。
launcher 额外要求 root 所有、存在执行位且具有 ELF 魔数。
运行 destination 的原白名单保持不变，重复目标在文件内容读取前拒绝。

成功取得 fstat 后，`observed` 可包含 device、inode、uid、mode、declared_bytes、bytes_read、
sha256 与 identity_verified。字段随实际到达阶段累积：未取得的观察保持缺失或 null，
不会补造一个完整文件事实。SHA 不匹配时仍保留实际 SHA；读前读后大小、mtime、ctime 或读取长度
发生变化时标为 source_changed，identity_verified 保持 false。
identity_verified 表示这次固定文件描述符上的稳定性检查完成，不是未来路径、权限或执行的保证。

诊断采用只读分支，不创建 memfd；生产执行继续复制到 sealed memfd，并只使用已经固定的字节。
launcher 也仅被读取，不运行 `--version`；适用时原 evidence 的 bwrap_version 保持
`not-executed`。doctor 不调用 ldd、shell 或模板环境检查来推测依赖是否齐全。

launcher 独立使用 64 MiB 预算，运行文件共同使用 256 MiB 预算。每个运行文件通过 fstat
取得声明大小后，先检查它能否放入剩余预算，再读取内容；不能先读完整文件再报告总量超限。
已经读过的字节即使随后遇到哈希错误、变化或其他失败，也消耗共享预算。
`evidence.runtime_bytes_read` 与 `runtime_read_budget_remaining` 显示实际计费。
诊断读到初始声明大小即停止，最终 fstat 及总长度检查负责发现变化。

一个运行文件失败会留下自己的状态，随后仍检查其他独立文件。若剩余预算不足，后续文件产生
read_budget_exceeded/not_checked；不清零预算，也不把失败文件的读取量退回。
CLI 在 policy 构造时拒绝超过 128 项的清单；直接 C++ policy 超过该数时，只检查前 128 项，
记录未检查数量并使 complete=false。

## 6. cgroup 与 proc 的只读观察

未配置 `cgroup_root` 时，`cgroup.root` 报 cgroup_not_configured。
可选 `cgroup.hierarchy` 单独观察固定 `/sys/fs/cgroup` 的文件系统类型，并标记
delegation_observed=false。后续五个委派检查均为 not_checked，
`depends_on: cgroup.root`；不会把默认层级当成所需的专用根。

配置根时仍拒绝 `/` 和 `/sys/fs/cgroup`。实际打开的目录必须通过 fstatfs 确认为 cgroup v2；
普通目录即使具有同名控制文件也无法通过。根未建立时不读取依赖控制文件；根已建立后，
挂载、权限和两个控制文件属于可分别观察的项，一个失败不会吞掉其他项。

| 对象 | 本次允许的观察 | 未完成的真实运行事实 |
| --- | --- | --- |
| cgroup 根 | 实际文件系统、fstatvfs 只读标志、当前有效身份 W_OK/X_OK | 后续创建子组、获得独占委派或部署必然成功 |
| `cgroup.procs` | 当前有效身份 W_OK，报告 write_attempted=false | 进程已迁移、已隔离或已退出 |
| `cgroup.subtree_control` | 有界读取已启用 controller，逐项列出缺少的 cpu/memory/pids | 已启用或修改任何 controller |
| `cgroup.type` | 有界读取，并按原执行器精确要求 `domain\n` | 未来子组已创建或限额已施加 |

未来控制路径统一为：

```text
<cgroup_root>/wls-op-<sha256(operation_id)[0:32]>/<control_file>
```

未配置根时，前缀显示 `<configured-cgroup-root>`。八个 control_file 是 `memory.max`、
`memory.swap.max`、`memory.oom.group`、`pids.max`、`cpu.max`、`cgroup.kill`、
`cgroup.procs`、`cgroup.events`。这些条目均为 `path_kind: operation_child_pattern`、
required_for_prerequisites=false、status=not_checked、reason=operation_not_created。
doctor 没有 operation_id，不创建该目录、不读写这些未来文件，也不遍历已有 operation 组、
迁移或杀死进程。模式字符串不能被解释为已经打开的实际路径。

三个固定辅助路径为 `/proc/sys/user/max_user_namespaces`、
`/proc/sys/kernel/unprivileged_userns_clone` 和
`/proc/sys/kernel/apparmor_restrict_unprivileged_userns`。它们各受 128 字节上限约束，
仅解释已知无符号整数或 0/1 形式；未知形式为 auxiliary_value_unknown。
这些检查均非必需项，并显式记录 execution_verified=false。
读取到某个开关值不能证明未来 unshare、挂载、bwrap 或网络隔离成功。

## 7. 成本、显示与截断边界

| 项目 | 边界 |
| --- | --- |
| doctor/离线协议 CLI 的每个 argv 参数 | 读取输入前限制为 4096 字节并校验 UTF-8；路径的绝对词法解释也受 4096 字节及 UTF-8 限制 |
| manifest 原字节读取/严格 JSON 解析 | 256 KiB |
| 运行文件 | 最多 128 项，单个 64 MiB，共享实际读取量 256 MiB |
| launcher | 独立 64 MiB |
| 执行资源路径 | 原 4096 字节、64 个组件、单组件 255 字节限制 |
| cgroup 文本 | 每项最多读取 16 KiB |
| proc 辅助文本 | 每项最多读取 128 字节 |
| 报告 | 512 个检查项，每项 8 条原因，每条辅助错误文本 512 字节，规范 JSON 编码不超过 2 MiB |

配置加载继续使用原文件、总字节、节点与深度边界。文本控制文件必须在读取上限内观察到 EOF；
达到上限仍未见 EOF 即报告 read_budget_exceeded，不再额外读一个块来判断是否超限。
执行器不会为 doctor 扫描系统目录寻找其他 launcher、库或 cgroup 候选。

直接 C++ 库调用仍可接收 POSIX 非 UTF-8 的宿主路径字节，并按原文件系统规则诊断；
它与 CLI 参数的 UTF-8 限制分开。路径显示先截取有界原字节前缀，再进行 UTF-8 显示转换，
避免直接库输入的超长字符串导致无界复制。
`path_truncated`/`source_truncated`/`destination_truncated` 表示显示被截短；
对应 `*_display_transformed` 表示字节经过显示转换。这些标志帮助区分显示文本与原输入，
不改变实际打开路径或生产拒绝规则。

输出超限时设置 report_truncated=true、complete=false、strong_prerequisites=false，
并保留 report_budget_reason 与计数。实现按边界截短辅助错误、限制原因数量，必要时省略尾部
检查、运行输入或旧 evidence 项；相应位置带有 omitted_reason_count、report_items_omitted
或 runtime_file_observations_omitted 等标记。不得把剩余内容声称为完整检查。

`to_json(SandboxDoctor)` 与公开 `bound_sandbox_doctor_report(Json&)` 使用同一输出限制。
库返回值本身同步最终完整性与准备状态；CLI 补完来源后再次应用限制。
一个已标记截断的报告重新序列化或重新限制时仍为不完整，不能恢复 strong_prerequisites。
若调用方追加过大的来源，限制器可把 inputs/evidence 替换为明确省略摘要，同时保留零执行边界；
不受支持的超大扩展字段若仍无法容纳则拒绝输出，而不是无界编码结果。

## 8. 生产守卫与真实验收边界

生产路径的无符号链接打开、UID/mode、冻结哈希、sealed memfd、工作区固定、强隔离要求、
网络 none、首派发、结算和原后代退出证据要求保持不变。cgroup controller/domain 判定复用
原精确条件；诊断新增结构化原因，不允许弱隔离或更宽的运行映射。
后续真实执行仍须重新检查原条件，不能接受 doctor JSON 作为授权、启动或退出收据。

所有正常报告的 evidence 明确为 probe=read-only-prerequisites、programs_executed=0、
namespace_execution_tested=false、weak_fallback_available=false、network=none；
CLI 另保留 environment_checks_executed=false、process_isolation_verified=false。
这里的 network=none 描述诊断没有发起网络请求，不是对一个未执行业务进程的断网证明。

只读表示不主动写文件、数据库、权限、cgroup/sysctl，不执行程序、不领取租约、不推进 epoch、
不创建资源和不发送网络请求。普通文件读取可能触发文件系统 atime 更新；不能承诺所有宿主
元数据完全静止。不存在自动安装、修复权限或写回冻结哈希的路径。

本节点不扫描运行时工作区分配。持久分配历史继续通过
[R2a.2 的 owned-workspaces 与 closeout](R2a-allocation-recovery.md) 查看；
ABANDONED 路径和 filesystem_observed=false 仍是保留记录，不是当前文件系统存在性证明。
doctor 不采用目录、补写 allocation_receipt、清除 UNKNOWN 或生成清理证明。

即使全部只读前提通过，仍未证明动态加载器/依赖完整、namespace/挂载成功、网络隔离、
资源限额执行、真实业务程序行为或全部后代退出。部署正向验收须另按
[S1-worker-tools](S1-worker-tools.md) 与 [S1-process-settlement](S1-process-settlement.md)
保留实际启动屏障、隔离、网络、限额、原输出、工作区前后观察与后代退出证据。

## 9. 回归设计与证据分类

[C++ 专用用例](../../tests/doctor_diagnostics_tests.cpp) 覆盖逐项失败后续进、源/目标分列、
类型和权限、符号链接、缺少或错误冻结哈希、读中变化、EIO、数量/字节/报告边界、路径显示、
可选 operation 模式和截断不可恢复准备状态。故障注入只匹配测试记录的临时 inode/device；
不在生产库添加注入入口。稀疏超限文件验证预读拒绝，小额纯预算用例验证共享预算的耗用规则。

cgroup 类型、只读挂载、访问及 controller/domain 分类中无法由普通用户安全构造的条件，
使用明确标注的固定观察输入测试共享判定分支。普通目录伪装 cgroup 的用例必须被拒绝；
这些固定数据和拒绝用例不构成真实委派环境的正向证据。
launcher 文件属性用例还只读观察固定系统 ELF/非执行文件/脚本，验证文件分类，不运行这些程序。

[CLI 专用用例](../../tests/test_doctor_diagnostics_cli.py) 覆盖默认和显式来源、原字节 SHA、
配置路径、无效或不可读清单、错误返回、后续独立条目、标记式假 launcher 不执行、
未修改夹具文件，以及数据库写者持锁时 doctor 仍无需打开或接管数据库。
测试 runner 保留调用者实际 umask，并要求普通用户；私有夹具权限不放宽生产守卫。

普通用户、双 umask、GCC/Clang、ASan/LSan、UBSan 与原套件的执行结果由集中验收逐项记录。
本文件只说明这些用例验证什么，不将编译、固定分支、拒绝路径或离线检查写成真实部署验收。
