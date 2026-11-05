# proc 证据工具 F-02 / F-03 修复验证报告（audit-proc-evidence-fixes-20261105-v2）

日期：2026-11-05。范围：仅本模块诊断工具 `scripts/proc_dso_observation.py`
及其自检 `tests/test_proc_dso_observation.py` 的两项已确认审计缺陷修复。
该工具未注册 CTest/Make/CI 产品门禁；本报告不是产品 sanitizer、兼容性或
CI 结果，也不改变任何产品注册、期限、SQL、历史报告或开放状态。

## 0. 版本纠正说明（v2）

本 v2 报告对（`docs/reports/audit-proc-evidence-fixes-20261105-v2.md` / `.json`）
取代并纠正 v1 原件：`docs/reports/audit-proc-evidence-fixes-20261105-v1.md`
（SHA-256 `829c1ba13478af98a263011b7272fba0ae1c52ebdca85c269864036cb078092d`）与
`docs/reports/audit-proc-evidence-fixes-20261105-v1.json`（SHA-256
`d8d99715f719d0a1741f10b31e19c8f73698fc5d8c859ac1bc7702ae8401093e`）。v1 两件按
原路径、原字节完整保留为被取代原件，后续以本 v2 对为当前报告。

本 v2 仅做两项元数据更正，全部技术契约、运行事实、限制与开放状态不变：

1. v1 JSON `validation.test_count_breakdown.existing_kept` 由 24 更正为 23：基线
   23 项测试方法全部保留，新增 F-02 4 项、F-03 15 项，23 + 4 + 15 = 42（亦即
   23 + 19 = 42），与历史全量收据 "Ran 42 tests" 一致；24 + 4 + 15 = 43 自相
   矛盾，属 v1 笔误，非本次重跑测试的结果。
2. v1 MD/JSON 中测试文件"前"SHA-256 抄写错误更正：旧错误值
   `075dd9446821475dd2a5a1f43b8baa6a42f666370dbaab999c9a6526a147edff`（old incorrect
   value，仅此作出处记录，任何有效身份字段不再使用）应为基线提交
   `537b3aef5d5671325239d626448608041de8b79a` 的
   `tests/test_proc_dso_observation.py` Git blob 真实 SHA-256
   `075dd9446821475dd2a5a1f43b8eaa6a42f666370dbaab999c9a6526a147edff`。原私有
   implementation summary 含同源抄写错误，按不可变证据原样保留，其证据效力由
   本轮私有 correction manifest 指明并取代；其余源/测试前后哈希保持原值。

纠正 UTC：`2026-11-05T12:01:54.758638+00:00`。本轮为纯静态核验（Git blob SHA-256 重算、`ast.parse` 类内
测试方法枚举），未导入/执行任何模块，未执行任何新测试（new_tests_run=false）；
"Ran 42" 等运行结果继续复用 implementation-01 的历史收据与日志，不声称本轮重跑。

状态时间作用域：§4 六状态仍是原实施报告写入阶段的历史快照；原 v1 报告精确创建
UTC 未知，原实施阶段入口收据 `created_at=2026-11-05T11:20:57.240960+00:00` 仅作
阶段证据锚，不是报告写入精确时间。本 v2 不自动升级任何状态，六状态现有值全部
保持；§4 中"独立硬审核尚未执行"是原实施写报告时的描述——后续 hard-review-01
已于 `2026-11-05T11:33:07.259879+00:00` 收据登记 PASS（独立只读会话），在此
单列记录；它不是 PR_REVIEW_COMPLETE，也不是本 v2 的新硬审核结果（针对 v2 的
新硬审核尚未执行）。

## 1. 基线与修复对象

| 项 | 值 |
| --- | --- |
| 工作分支 | `fix/audit-proc-evidence-20261105-adp35vg4`（基线提交 `537b3ae`，porcelain 起始为空） |
| F-02（P2） | `read_proc_stream` 只在 read 前检查单调时间预算：预算后真实返回的零字节 EOF 仍被判 `COMPLETE/EOF`，违反模块自身"预算内以真实零字节读结束"的契约 |
| F-03（P3） | `verify_file_object` 先 `os.stat` 再二次按路径 `open`，`actual` 的 device/inode/bytes 拼自旧 stat 而哈希属于新打开对象；无类型预检（FIFO 会在 open 阻塞）、无 fd 身份、无读后/末次路径核查 |
| 源文件 SHA-256（前 → 后） | `scripts/proc_dso_observation.py`：`795e8bc1…36cd9` → `b9878f2d…e16eb`（全文 `795e8bc135a02e041e8937c6f3834ca82c9f25d8d894451dfa60b9c0b1836cd9` → `b9878f2d3589ee6b23e6814131e43b310d1b040cfbbfa0f0dfeecfedad9e16eb`） |
| 测试文件 SHA-256（前 → 后） | `tests/test_proc_dso_observation.py`：`075dd944…7edff` → `8ba8948b…4b15`（全文 `075dd9446821475dd2a5a1f43b8eaa6a42f666370dbaab999c9a6526a147edff` → `8ba8948b884086acc375320d806c1b8a976cb3aebfbc3039795e14e532fa4b15`） |

修复前经全树检索确认：本模块仅被上述两个文件引用，`verify_file_object`
在当前仓库没有产品调用者；因此 F-03 修复是诊断 API 契约的纠正，不是、
也不得称为产品 DSO 加载认证漏洞的修复。

## 2. 修复后的确定契约

### F-02 `read_proc_stream`

- 默认预算不变（2 MiB 字节、1024 次尝试、0.25 秒、65536 块），仍接受注入
  read/clock；同步循环不对运行中的 read 回调作实时硬中断承诺。
- 每次成功 read 返回后采集一次单调时间，同一次采样同时生成该步判定与
  `elapsed_seconds`。
- 同一次已返回 read 的优先级：`SIZE_LIMIT_EXCEEDED` > `TIME_BUDGET_EXHAUSTED`
  > EOF `COMPLETE`；最后一次许可 read 的 EOF 只有读后采样未超预算才
  `COMPLETE`；恰好等于预算沿用严格 `>` 边界（仍完成）。
- 真实返回零字节但超时：`eof_observed=true` 记录 EOF 事实、
  `read_status=INCOMPLETE/TIME_BUDGET_EXHAUSTED`、保留限内数据与真实计数；
  上层 `maps_eof_complete` / `evidence_complete` 只能由 COMPLETE 推导，故为
  false。超时返回的非空块同样保留限内数据与计数。
- EINTR、EAGAIN、权限/消失及其他 OSError 映射、总尝试计数、ReadError
  异常链保持不变。

### F-03 `verify_file_object`

- 函数名与调用形式 `path, expected, *, max_bytes=DEFAULT_MAX_EXE_BYTES`
  不变，默认 1 GiB 不提高；`max_bytes` 显式验证为正整数（布尔值拒绝）。
- 流程：路径 lstat 类型预检 → 只读 `os.open` 单一固定 fd
  （O_CLOEXEC/O_NONBLOCK/O_NOFOLLOW）→ fstat 类型与身份（device、inode、
  类型、size、mtime_ns、ctime_ns）与预检对齐 → 同一 fd 分块散列，每次请求
  至多 `min(1 MiB, remaining+1)`、按实际读取字节数累计，读到 limit+1 即拒绝
  （不整文件缓冲）→ 真实零字节 EOF → fd 再 fstat → pathname 再 lstat →
  与冻结 expected 比较 device/inode/bytes/sha256；`actual.bytes` 即实际读取
  字节数且须与稳定 size 相同。
- 成功域是普通非符号链接文件：目录、FIFO、符号链接及打开后类型变化明确
  拒绝（FIFO 不再发生阻塞 open）；`matches=false` 携带稳定原因码与 `stage`。
- 保留原因码 `EXCEEDS_HASH_BUDGET`，以 `stage` 区分 `preflight_size`（预检
  stat size 超限，不打开、无 fd）与 `read_limit_plus_one`（实际观察到第
  limit+1 字节，`read_bytes` 恰为 limit+1）；正好 `max_bytes` 的文件仍须取得
  真实 EOF 才可完成。未完成观察不输出可被误当完整哈希的 `actual` 字段。
- EINTR 按 `DEFAULT_MAX_READ_ATTEMPTS` 有界重试，耗尽为结构化拒绝
  `EINTR_BUDGET_EXHAUSTED`；lstat/open/read/fstat 的其他 I/O 异常继续抛出，
  绝不变成 `matches=true`；所有 return/拒绝/异常路径经 finally 关闭唯一 fd。
- 明确限制：读前 lstat、读后 fd fstat 与末次 pathname lstat 是三次独立采样，
  不是原子文件系统快照，末次采样之后路径仍可能被替换（docstring 与本报告
  均如实记录）。该固定 fd 策略不套用于 `RealProcIO.exe_identity` 读取
  `/proc/PID/exe` magic link 的既有路径，其他读取路径未改动。

## 3. 验证（先红后绿，同一份冻结测试输入）

运行环境：普通用户（UID 1000），stock CPython 3.12.3，任务专用数据盘
TMPDIR，`PYTHONDONTWRITEBYTECODE=1`，`python3 -B` 直接入口。三次运行的
完整 argv、cwd、UTC 起止、单调耗时、原始退出码、输入/日志 SHA-256 收于
本模块私有证据目录 `implementation-01/runs/`（run-capture 捕获脚本
SHA-256 `9ca7ba03…`，排他新建输出，失败红例按设计保留、未改写）。

| 运行 | 命令要点 | 源 SHA | exit | 结果 |
| --- | --- | --- | --- | --- |
| red-core-2tests | `StreamReadTests.test_final_eof_over_time_budget FileObjectTests.test_swap_before_open_rejected -v` | `795e8bc1…`（未修改基线） | 1 | Ran 2, FAILED (failures=2)：F-02 断言 `'COMPLETE' != 'INCOMPLETE'`；F-03 断言 `True is not false`（真实 helper 上的确定性反例，非 harness 错误） |
| green-core-2tests | 同上（同一测试文件 `8ba8948b…`） | `b9878f2d…`（修复后） | 0 | Ran 2, OK |
| full-suite | `python3 -B tests/test_proc_dso_observation.py -v` | `b9878f2d…` | 0 | Ran 42, OK，0 skip（含真实 stock 解释器 /proc 夹具，其自身五秒界限与仅回收自有子进程约束保持） |

红例仅限可在原版源上确定性终止的两项核心反例（计划规定）；FIFO/目录/
符号链接、EINTR 风暴等会使旧实现挂起或无意义的边界只在修复源上运行，
未把旧版阻塞当作红例。

### 测试强度分层（如实记录）

- **真实 helper + 注入事件时钟**（F-02 核心/边界/同 read 优先级）：被测
  `read_proc_stream` 本体未 mock，时钟只在 read 边界确定推进；非 /proc 端到端
  计时验证。
- **真实 helper + 确定性 wrapped-OS-call 接缝**（F-03 全部 15 项）：被测
  `verify_file_object` 本体未 mock；替换/变动只发生在自有夹具路径与自有 fd
  的接缝上，原函数 finally 恢复；fd 回收以真实 `os.fstat` EBADF 即时证明，
  不依赖 close spy 或 `/proc/self/fd` 枚举，断言失败也有 harness 兜底回收。
- **合成上层串联**（F-02 传播用例）：FakeProcIO 派生类 mock 整个 /proc 但
  maps 读取转发真实 `read_proc_stream`；明示为合成，不冒充真实 /proc 观察。
- **真实普通子进程**（既有 Group 4）：stock 解释器等待受控管道、真实
  /proc 读取与 EOF/身份绑定；本轮在修复源上通过（无 TOOL_INTEGRATION_BLOCKED）。
- 无真实供应商/模型调用，无产品 Store，无产品 CTest/CI/sanitizer 运行。

## 4. 状态（六项分列，不得合并）

| 状态 | 值 | 依据 |
| --- | --- | --- |
| IMPLEMENTATION_COMPLETE | true | 两个目标文件按 FINAL 计划完成最小修复；写入仅限本两文件与本报告对（plus 模块私有证据/夹具目录） |
| LOCAL_VALIDATION_COMPLETE | true（仅本工具自检范围） | 红绿同输入、全量 42 项 OK；是工具级验证，不是产品 sanitizer/CTest/兼容性通过 |
| PUSH_COMPLETE | false | 本实施会话不做 Git 写入；commit/push/Draft PR 由根协调者按授权另行执行 |
| REMOTE_CI_COMPLETE | false | 新提交尚未产生，远端 CI 未运行；历史基线 CI 不替代 |
| PR_REVIEW_COMPLETE | false | 独立硬审核尚未执行（原实施写报告时的描述；后续 hard-review-01 已单独登记 PASS，见 §0，仍非正式 PR 审核） |
| MERGED | false | 无合并授权与事实 |

## 5. 保留的开放状态与未并入事项

原 sanitizer/Clang/live_workflow/Make IPC 开放状态、strict WAL/SHM
BLOCKED_NOT_COMPLETE、Windows 11 普通用户 NTFS NOT_RUN、私有原托管
BLOCKED 均保持不变。95 个产品 CTest 注册项与期限未动；本工具不注册产品
gate。审计项 F-O1（snapshot_retained 契约疑问）不属于本修复范围，未并入。
`verify_file_object` 修复后仍无产品调用者接入；如未来接入需另行评审。

## 6. 交付物

- `scripts/proc_dso_observation.py`（修改，见上 SHA）
- `tests/test_proc_dso_observation.py`（修改，见上 SHA；基线 23 项全部保留，
  新增 F-02 4 项、F-03 15 项测试，23 + 4 + 15 = 42（亦即 23 + 19 = 42）；
  事件时钟/wrapped-OS-call 夹具；修正 `ListClock` 耗尽后
  末值保持，原 elapsed/状态/数据/次数断言全部保留并通过）
- `docs/reports/audit-proc-evidence-fixes-20261105-v2.md` / `.json`（本对，
  排他新建；取代并完整保留 v1 原件）
- 私有证据：`implementation-01/`（基线源快照、三次运行 stdout/stderr/receipt、
  捕获脚本、实施摘要；含凭据检查，不含仓库外泄露路径）
- 私有证据：`report-correction-implementation-01/`（本 v2 轮私有纠正 manifest
  与静态校验输出）
