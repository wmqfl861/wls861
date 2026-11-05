# 审计修复实施报告：ASTRA-A-001 与 D-CLI-001（2026-11-05，v1）

时间范围：2026-11-05T11:24Z 至 2026-11-05T13:00Z（UTC），实施会话为 wls861 本地流程
implementation-02 阶段（ZCode，全新会话；implementation-01 为基础设施失败证据，不计入产品 RED）。
计划输入：`plan-final-v1.md`（SHA-256 `7b24abc84a745464e71d82ab8ab6451ba4af10c7b47cfa91b28e2b6f55b83d5f`）
与 `planner-record-final-v1.json`（SHA-256 `c7acc09dc8d33c52171d68cf1226b38b2137a71692f6d87196024680e31bb652`），
本次调用实际读取并复核一致；WORKFLOW SHA-256 `f49ff366cacb15df96ade89e4aa3d4412cb50836e4f11f3be407713a844bf990`。

## 1. 基线与门禁身份

| 项 | 值 |
| --- | --- |
| 工作区 | `/mnt/d/codex-workspace/wls861-audit-fixes-20261105-adp35vg4/product`（分支 `fix/audit-product-20261105-adp35vg4`） |
| 基线 HEAD | `a5675e1245bebba41c499e56bd2c8a16be2d5dae`（实施起点与结束时一致；本阶段未做任何 Git 写入） |
| 旧基线产物 | f6141569 Release 构建：`libwls_s1.a` `6cf0984c…7c9ff`、`libwls_kernel.a` `a780803a…1767f`、`wls` `fab23641…9673`，本次会话重算哈希与审计 provenance（existing-library-provenance-02 / existing-cli-provenance-03）一致 |
| f614→a567 差异 | 仅 `docs/reports/n5-persistent-workflow-coverage-20261104-v1.{md,json}` 两个新增文件；production/header/build/resource 字节相同（`git diff --stat` 复核） |
| 执行环境 | ubuntu UID 1000、umask 0077、GCC 13.3.0（/usr/bin/g++）、系统 Python 3.12.3 + jsonschema 4.10.3、数据盘专属 TMPDIR（红 `…/implementation-02/tmp-red` 100 字节、绿 `…/tmp-green`） |
| 起始工作区状态 | 仅遗留 implementation-01 的 4 个测试文件改动，哈希与 continuation-input-v1.json 逐一相符后继续，未重置、未覆盖 |

## 2. ASTRA-A-001（保留路径被工具名 256 字节域误拒）

**原基线与反例等级**：审计 `A_store/tmp/path-probe-01`（真实执行，parent=346、final=377、stage=379 字节）：
prepare+preflight 成功而 abandon 报 `invalid tool identifier`，记录仍 PREPARED、新键冲突；证据等级为 compile/run/stdout，
run 未独列 binary hash（保留该限制描述，不提升原件等级）。

**最小修复**（`src/store.cpp`、`src/store_cleanup.inc`，diff 见第 6 节）：
新增局部 `owned_path_strings` 助手——对保留路径应用既有路径域上限 `text_field(value, "owned path", 4096)`，
保留非空/NUL 检查、稳定字典序（`std::set`）与原重复拒绝语义（`ErrorCode::InvalidInput`，文案 `duplicate owned path`）；
仅替换 `abandon_owned_workspace` 的 `retained_paths` 与 `cleanup_status` 的 retained-allocation `paths` 两处调用。
路径域验证失败返回 `ErrorCode::InvalidInput`/`invalid owned path`，不再使用工具名文案。`strings` 工具名域原样保留
（admission 257 字节工具名仍按 `Unauthorized` 拒绝、重复项仍 `InvalidInput`，新增回归覆盖）。
持久化仍经原 `json_text` 的 UTF-8/JSON/字节预算；prepare 的 `protected_path + "/work" < 4096` 原门禁未动；
`owned_info`/`exposed()` 8 MiB 回读预算不变；未做任何路径规范化或文件系统观察。

**本次真实 RED/GREEN**（红绿对使用相同最终测试字节 `tests/allocation_recovery_tests.cpp` = `173b2389…79b8`）：

| 运行 | 对象 | 结果 |
| --- | --- | --- |
| RED（旧库） | 新测试源码按原 compile_commands 旗标精确重放编译（仅改输入/输出路径，旧树 include 保留，-Werror 下 exit 0），链接已核验旧 `libwls_s1.a`/`libwls_kernel.a`（库路径改绝对，其余不变） | exit 1，31 通过 / 3 预期失败：`retained final and stage path byte boundaries abandon exactly` 与 `long UTF-8 multicomponent retention keeps evidence slots and display` 均为 `invalid tool identifier`；`owned path domain failures use path-specific errors` 失败于路径专用文案断言 |
| RED（旧 CLI） | `test_s1_allocation_recovery_cli.py` 对旧 `wls`（fab23641） | exit 1，9 通过 / 1 预期失败：长路径 abandon 报 `invalid_input / invalid tool identifier`（经真实 CLI 入口的动态复现；原审计未动态复现该分支，此为本次新增证据） |
| GREEN（候选） | 同一测试源码链接候选库 | exit 0，34 通过 / 0 失败 |

覆盖内容：final/stage 各自 256/257 精确字节边界（按绝对路径字节长度构造真实多组件父目录，组件 ≤96 字节，
TMPDIR 真实根约 117 字节、目标父长 224/226 字节，无需退回 `/tmp`）；>256 字节多组件 UTF-8/空格合法路径的
prepare→stage→abandon 全链（marker/receipt/`work` 内容逐字节保留、无 stage 弃置不产生分配文件、ABANDONED、
proof=null、filesystem_observed=false、deletion_performed=false、原键精确重放不新增事件、不同参数冲突、
同键不复活、新键 OWNED 且 active=1/history=2 槽位语义）；`workspace_cleanup_status`/`closeout`/
`owned-workspaces` 展示两个完整保留路径；嵌套分配拒绝（置于替换分配之前，见第 5 节顺序说明）；
伪造 5000 字节父路径的路径域专用错误；工具名 257/256/重复原域保护。

## 3. D-CLI-001（run-fake disposition 枚举与适用性校验过晚）

**原基线与反例等级**：审计 `D_protocol_config/repro_disposition_fresh_v2`（真实执行）：fresh 请求
`run-fake --disposition invalid-enum` exit 0、stderr 空，attempts 0→3、events 1→17、tasks/plans/ops 0→1，
request RECEIVED→RUNNING（REPRODUCED 等级，exact-source provenance 保留于审计原件）。

**最小修复**（`src/cli.cpp`、`src/offline_runtime.cpp`）：
- CLI：`run-fake` 读取 disposition 后、打开任何 Takeover Store 前要求空或 `update/keep/cancel` 三个字面值，
  使用既有 bounded error JSON，不回显任意枚举原文。
- 运行器：`OfflineRuntime::run` 入口先验证同一枚举；随后取得 tasks/request 只读快照，
  `pending_amend = !tasks.empty() && task.based_on_requirements_revision != request.requirements_revision`，
  有 pending AMEND 必须提供三个有效值之一、否则必须为空——该验证先于任何 `rebind_task`、`reserve_call`、
  模型 complete、ack 与业务效果；后段既有 require 保留为防御。pump/单任务/独立 AMEND（模型 dispositions）路径未改。

**有意行为收紧（非纯无变化修复）**：原有 fresh 分支接受任意非空 disposition；本修复把“无 pending AMEND 时必须为空”
的既有规则扩展到 fresh——fresh + `update/keep/cancel` 虽为合法字面值，今后也被拒绝。三种合法值的既有 AMEND 正例
（update/keep/cancel）回归全部保留并通过。

**语义边界（按 FINAL）**：非法枚举由 CLI 在 Store 打开前拒绝，测试据此断言无 writer-lock/DB/WAL/SHM；
合法枚举但不适用状态的拒绝发生在调用者已打开 Store 之后，不改变调用者合法 Store 打开的既有恢复语义
（absent DB + 合法枚举保持既有 `storage` 失败，不在本次断言范围）。

**本次真实 RED/GREEN**：

| 运行 | 对象 | 结果 |
| --- | --- | --- |
| RED（旧 CLI） | 新 `s1_cli_smoke.py`（12 例，字节 `8993bd78…b852`）对旧 `wls` | exit 1，10 通过 / 2 预期失败：fresh 用例 `AssertionError: 0 == 0`（非法枚举被接受并完整推进，即审计反例）；absent-DB 用例 `'storage' != 'invalid_input'`（旧 CLI 先开 writer） |
| RED（旧库） | 新 `offline_runtime_tests.cpp`（字节 `e33a1db1…5075`）链接旧静态库 | exit 1，11 通过 / 2 预期失败：fresh 用例 `CHECK(caught)`（旧代码不抛异常）；rebind 用例 `store.task(...) == task_before` 失败（旧算法先 rebind 后拒绝，单列同条件 RED→GREEN） |
| GREEN（候选） | 上述两套对候选 | exit 0：offline_runtime 13/13；s1_cli_smoke 12/12 |

覆盖内容：fresh、已有 task 无 AMEND、pending AMEND 三态的非法枚举；fresh 与非 AMEND 态三种合法但不适用值；
absent DB 非法枚举不创建 DB/WAL/SHM/writer-lock；CLI 侧全用户表排序逻辑快照（BLOB 无损）前后一致；
直接运行器在 rebind/预约前拒绝且 task/request/budget/events 不变；published-plan=2/task=1 可恢复重绑 fixture
传不适用 disposition 后先拒绝再空值恢复；AMEND keep/cancel 正例（含 CANCELLED 与调用数断言）、AMEND 缺值、
合法空值、published-plan 恢复、既有 frozen budget/UNKNOWN/重放回归保留。

## 4. 定向套件结果（候选构建 `build-audit-product-release-v1`，Release GCC，95 注册项不变）

`ctest -R ^(allocation_recovery|allocation_recovery_cli|offline_runtime|s1_cli_smoke|r2_amend|r2_amend_cli_smoke|cleanup|cleanup_runtime|owned_capacity|owned_capacity_runtime|r24_recovery|r24_cleanup_recovery)$`
→ **100% tests passed, 0 tests failed out of 12**（exit 0，最终测试字节上的合并运行；`ctest -N` 确认候选构建注册 95 项）。
方法级子例计数：allocation_recovery 34、offline_runtime 13、s1_cli_smoke 12、allocation_recovery_cli 10。

## 5. 相对遗留测试改动的修正（同作者遗留、本次按 FINAL 校正）

1. `s1_cli_smoke.py` absent-DB 用例原把合法字面值 `update` 也纳入“不创建 writer”循环，超出 FINAL 语义边界
   （该要求仅限非法枚举）；改为仅非法字面值 `invalid-enum`/`unknown-disposition`。
2. `allocation_recovery_tests.cpp` 长 UTF-8 用例三处期望与冻结检查顺序不符：新键 NotReady 拒绝改传当前资源版本
   （`abandonment_target` 先 CAS 后状态，此顺序为原始代码）；同键重放改用原 UTF-8 父目录身份
   （`prepare_owned_workspace` 比较父代次）；嵌套拒绝移到替换分配之前（team 活跃槽位检查先于嵌套保护）。
3. `allocation_recovery_tests.cpp` 边界用例补充“无 stage 弃置不产生分配文件”显式断言（FINAL 红绿项 4 首句）。
4. `test_s1_allocation_recovery_cli.py` 长路径用例嵌套拒绝同样移到替换分配之前（同 2c 原因）。
   以上均为测试期望/顺序修正，未放宽任何安全断言；每处修正后重跑红绿对保持同字节一致。

## 6. 最小 diff 与最终源码身份

改动仅限 FINAL 允许的 8 个路径（`git status --porcelain` 仅此 8 项，`git diff --check` 干净）：
`src/store.cpp`（+13）、`src/store_cleanup.inc`（2 行替换）、`src/cli.cpp`（+5）、`src/offline_runtime.cpp`
（+12/-1）、四个测试文件（新增子例与修正）。完整补丁存
`…/implementation-02/logs/final-working-diff.patch`（SHA-256 `e4fb890e389220b03fe91be9c57c7ca85552dbd9225eab913a5239e7fd3ae8e1`，对应最终源码身份见
`…/implementation-02/logs/final-identities.txt`；源码最终哈希：store.cpp `807b2fd7…d5b3`、store_cleanup.inc
`4ab028e5…cbec1`、cli.cpp `b426f64c…68a04`、offline_runtime.cpp `717602a5…258f`）。
候选产物：`wls` `6330616f…7e6dd`、`libwls_s1.a` `5bc7ee89…93fb`、`allocation_recovery_tests` `c7fe98e7…5c8f`、
`offline_runtime_tests` `33657f68…e3c1`。

## 7. 状态与未关闭项

| 门禁 | 状态 |
| --- | --- |
| 计划审核 | PASS（sess_068223e5…，既有） |
| 实施（测试先行、RED/GREEN、最小修复、定向套件） | COMPLETE（本报告） |
| 独立代码/证据硬审 | NOT_RUN（首次 push/Draft 前由根启动全新会话） |
| 完整本机矩阵（双 umask GCC、Clang、UBSan、ASan/LSan、原 Make ASan） | PENDING（根路由的后续阶段） |
| 远端 push / Draft PR / CI | NOT_RUN（实施阶段禁止 Git 写入，未提交、未推送） |
| 最终 SHA CI 与最终结果硬审 | NOT_RUN |
| 正式 PR review / MERGED | NOT_RUN / false |

保持 OPEN：历史 Clang/live_workflow/Make IPC/sanitizer 问题（`historical_clang_asan_failures_closed=false` 不变）、
Win11 普通用户 NTFS NOT_RUN、私有原始托管 BLOCKED。本阶段零真实/付费供应商调用（工程 ZCode 会话本身不计为
WLS 真实验收）。报告缺失字段一律 `unknown`，未补造 0。
