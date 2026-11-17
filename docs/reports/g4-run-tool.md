# G4 命令执行阶段记录（部分完成）

2026-11-16：G4-N3 只读输入切片已由 [#136](https://github.com/wmqfl861/wls861/pull/136) 合入 main `adb89e46`；G4-N4 环境版本检查的最终提交 `d36573b9` 已通过核心、源码、D-40 和组合 native 独立复核，由 [#137](https://github.com/wmqfl861/wls861/pull/137) 合入 main `87c31473`（同树）。F-15 写入/提交扩容未实现，G4 尚未达到阶段出口。开发基线 R6 `b49bbb1`，Store 410、版本 0.13.0、六冻结格式不变，真实模型调用 0。

## G4-N3 只读输入

新请求显式冻结 `sandbox.input: {mode: read-only-v1, max_total_bytes: N}`，可信宿主 manifest 同时提供 `input_root`，缺一即拒绝；N 为 1..268435456。path/device/inode/mount_id 和 N 进入不可变 execution_policy。旧请求保留空只读 `/input`；模型卡只提示逻辑 `/input`、只读及上限，反馈不暴露宿主来源路径。契约及示例见 [Worker tools](../implementation/S1-worker-tools.md#5-强进程入口与-doctor)。

Store 在输入首次绑定及新 workspace、owned stage/final、delivery 事务中检查双向分离并核对原 policy SHA；使用元数据和固定 dirfd 父链，同设备不同 mount ID 别名保守拒绝。安全树审计拒绝链接、特殊对象、权限/UID/挂载异常，spawn 前累计普通文件逻辑大小。屏障检查实际 `/input` 身份及 private/ro/nosuid/nodev、无子挂载；`input_mount` 参与隔离判定，`runtime_mounts` 仍仅 `/usr`。合成跨设备叶根测试没有创建宿主挂载，不能当作真实 bind mount 验收。

## G4-N4 环境版本检查

新 `environment_checks: sandbox-run-v1` 仅取冻结 environment dependency，在既有 ack 后、Worker 预约前通过持久 `run` 执行。每项更新 binding/task CAS；同 epoch 读取原操作，PREPARED/RUNNING/UNKNOWN/缺结果不重派发。每项至多 5 秒/1024 字节，绝对截止的 stop token 覆盖 runner 准备及实际执行；完成后重新 capture。结果卡经原 `op_output` 授权读取原 BLOB；完整 exit 0、隔离/回收证明、非空 UTF-8 stdout 才显示版本，stderr 不冒充版本。required 失败停止下一预约，optional 确定失败可继续，UNKNOWN 保持围栏。卡片 SHA 进入 context dependencies；实际启动、复用及未运行分列，存储失败的未知计数保留 null 和 `counts_complete=false`，旧默认行为不变。

版本可用性与隔离证据独立：非文本、截断、空 stdout 使版本不可用，已完整取得的隔离证明仍按原事实报告。Standard 的版本观察不证明宿主未来包状态，检查仍受原 run 围栏约束，不自动安装软件。native CI 先保留 N3 的 sandbox 四场景和 CSV/input 检查，再运行同一环境请求的 input/version 组合入口；本机 native 未运行，长期环境边界见 [STATUS](../STATUS.md)。

## 实际验证

| 对象 | 结果与证据 |
| --- | --- |
| R6 main `b49bbb1` | [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43946016276) 10/10 作业、450/450 CTest、Make 60+25 通过；[`v0.13.0`](https://github.com/wmqfl861/wls861/tree/v0.13.0) 注释标签已推送，无 Release |
| N3 合入 main `adb89e46` | [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43959157815) 10/10 作业、450/450 CTest、Make 60+25 通过，0 CTest skip/fail |
| N3 PR `9355be19` | [核心](https://github.com/wmqfl861/wls861/actions/runs/43954620405) 270/270、[D-40](https://github.com/wmqfl861/wls861/actions/runs/43954617821) 11/11、[native](https://github.com/wmqfl861/wls861/actions/runs/43954623427) 精确 head 通过；含 CSV、覆盖/追加/删除/新建拒绝和输入 SHA 保持 |
| N4 最终提交 `d36573b9` | [核心](https://github.com/wmqfl861/wls861/actions/runs/43965203812) 270/270，synthetic `a83658cf` 与 d365 同树；[源码](https://github.com/wmqfl861/wls861/actions/runs/43965203937) 通过；[D-40](https://github.com/wmqfl861/wls861/actions/runs/43965197619) 精确 d365 **14/14 全通过**，worker 79.35/180 秒；[native](https://github.com/wmqfl861/wls861/actions/runs/43965206860) 独立复核 PASS：精确 d365、uid 1001、三前置及原四场景通过；同请求 8B input→Python stdout 14B、optional missing stderr 54B，原结果 hash/binding/proof/cleanup 与无宿主路径均核对，真实调用 0 |
| N4 合入 main `87c31473` | #137 于 2026-11-16 22:34:04Z 合并，树等于 d365；[main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43971296505) 10/10 jobs 成功。五配置 CTest 共 450/450：strict 90、strict-umask 90、Clang-18 90、ASan 45+45、UBSan 45+45；CTest 注册项 failed/timeout/skipped/not-run 均 0。Make-ASan shard 1/2 有 60 条、shard 2/2 有 25 条命令成功（非 CTest），命令失败/超时/跳过/未运行均 0。Actions 的 53 个条件跳过步骤不计测试跳过；source [身份与字节核对通过](https://github.com/wmqfl861/wls861/actions/runs/43971296444)。D-20：ASan 90 套无 >50% 项，最慢 worker_tools 64.53/180 秒（35.85%）。无上传 Actions artifacts；原始完整日志保留于私有证据 |
| 合 N3 后本机 Release | 10/10 通过，158.71 秒：acceptance_store、config_loader、offline_runtime、event_runtime、sandbox、worker_tools、process_evidence、r2a_mkdir、allocation_recovery、doctor_diagnostics；组合 native 入口已编译但本机未运行 |
| 合 N3 后本机 ASan | 选取七套，六套通过；event_runtime 在 180.79 秒本机时限超时，无 ASan 错误输出。合 N3 前 worker 在 180 秒本机时限超时，前四套通过；该原件单独保留且本轮未重跑；按 D-39 不加期限或重复诊断 |
| N4 首次托管失败与修复 | `f1150e27` [核心](https://github.com/wmqfl861/wls861/actions/runs/43956865312)/[D-40](https://github.com/wmqfl861/wls861/actions/runs/43956859535) 因 mkdir 私有 phase 宏仍为二参数而编译失败，CTest 未运行；`e97d076c` 补三参数兼容，保留原回调/断言/期限，核心 270/270、本机 mkdir Release/ASan 通过；历史 `f1150e27` [native](https://github.com/wmqfl861/wls861/actions/runs/43956863385) 四场景加环境两项通过，这些结果不迁记为 d365 门禁 |

N3 产品增删 258 行、测试/脚本 260 行；N4 相对合入后 main 产品增删 296 行、测试/脚本 372 行（含组合 native 回归）。超过 N4 的 200 行估值用于绝对期限、两驱动接入/摘要和原证据守卫，仍低于 400 行升级阈值及 800 行硬限，测试低于产品两倍。

历史 scheduled 补记（2026-11-17 核验）：[run 44078731658](https://github.com/wmqfl861/wls861/actions/runs/44078731658) 在旧提交 `4c3704b38bef016243335628a4dfe633b85c9cbe`、UID 1001 上通过 **Standard** 四个沙箱场景和两项环境检查；原始 stdout/stderr、结果 hash、隔离/cleanup 证明及 8B 输入均核对通过，真实模型调用 0。私有原件索引为 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/run-intake/scheduled-native-44078731658/verification.json`，SHA-256 `dd155fdd8b87b99c862367a2e55767b4f29a085c269a3700261976801f8a757f`。这条旧运行不代表 Strict、当前 main native 或真实 U3/U4 验收；本机 BLOCKED 和 Windows 11 NOT_RUN 保持。

## F-15 容量候选停止

W128 候选的正式 CTest 在 3.90 秒实际失败：128 KiB 写入、reopen+ack、下一 Worker 保留原 parameters/reasoning、提交 131072B 均通过，随后 TL check 的 capture 被既有 1 MiB guard 拒绝。候选停止、未合 main；这些局部成功不能写成 F-15 已实现，也不提高 guard 或删除断言。

随后仅一次私人 guard 前 stderr 计数诊断，使用不同 ELF：`logical_bytes=1358927 > 1048576`，prefix 20792、task_contract UTF-8 668083 / JSON 669324、current_state JSON 667121。诊断不替代正式失败门禁。候选 `b4355406` 已按 D-23 先建立并验证保险 bundle，再推送及核对注释标签 [archive/g4-artifact-capacity-w128-20261116-k48kol3a](https://github.com/wmqfl861/wls861/tree/archive/g4-artifact-capacity-w128-20261116-k48kol3a)，明确 NOT_FOR_MERGE、无 PR、未合 main；清洁分支及工作树已删除，构建和原始证据保留，归档记录在任务根 `w128-archive-BdqFEMRT/`。

## G4-D CSV 准备候选（2026-11-17）

[#156](https://github.com/wmqfl861/wls861/pull/156) 初始基于 main `2213c27`，现正常整合 `67f52f0`，增加第一个完整准备切片：FakeModel 经原 CLI 完成 Dispatcher 计划、TeamLead 工单、Worker 写入 Python、实际 `run` 和不可变提交；核对 `/input/orders.csv` 的 12 条订单聚合、原输入写入拒绝、隔离和进程回收证明、原调用/操作/反馈绑定及提交中的 `summary.csv`。这里只走到提交，其他两项 G4-D 场景仍未形成完整准备记录。

准确候选 `0ea6ca9` 的全新 Release 构建已完成；两个聚焦 CTest 通过：解析/oracle 0.23 秒、Worker 预启动拒绝 1.22 秒。拒绝夹具保留合法 input manifest、使用不存在的 launcher，真实原结果为 `REFUSED`、`spawned=false`，没有成功汇总或提交。原四个 sandbox 和两个环境检查继续保留。

首次本机检查曾使用封存的 B1 `f9da424` 二进制，已纠正其身份归属，不计作准确候选验证。原件、`binary-identity-correction.json`、准确候选构建/二进制身份及新测试日志在当前私有任务根 `g4d-csv-evidence/`；没有覆盖 B1 ELF。该候选产品/C++ 改动 0，脚本/测试/集成增删 295 行（292 新增、3 删除），另有固定夹具 181 行。公开 CI 制品只选取小型合成输入、配置、结果和 stdout/stderr，排除数据库、工作目录和构建产物。没有真实模型调用。

旧 head `2a28951` 的[核心 291/291](https://github.com/wmqfl861/wls861/actions/runs/44168598933)、[源码](https://github.com/wmqfl861/wls861/actions/runs/44168599111)及[native](https://github.com/wmqfl861/wls861/actions/runs/44168595942)已完成核验。native 精确 checkout、UID 1001、五次 FakeModel、真实模型调用 0；150B input 的写入被 EROFS(30) 拒绝且前后 SHA 相同，原 run/feedback/submission hash、binding、Standard 无网络隔离和 cleanup 证明均核对，提交 CSV 62B 与独立 oracle 一致。终态是 task SUBMITTED、request RUNNING，TeamLead check 待处理，不能写成业务验收或交付。原件在当前任务根 `run-intake/hosted-pr156-2a28951/`，native artifact SHA-256 `297f64952b223a05e0df06c04d36fff30f8e3fb1c1248f00555545d657e2275f`。整合后的最终 head `586eaea` [核心](https://github.com/wmqfl861/wls861/actions/runs/44173217114) 294/294、[源码](https://github.com/wmqfl861/wls861/actions/runs/44173216920)与[native](https://github.com/wmqfl861/wls861/actions/runs/44173270997)均通过并独立核验；核心/源码 synthetic `16f4716` 与 head 同树，780 个 blobs/modes 一致，native 精确 head。新的 native 仍为五次 FakeModel、输入拒写及原字节保持、62B 不可变提交，原四沙箱/两环境场景均保留；业务状态仍是 SUBMITTED，验收/交付 false。最终收据在 `run-intake/hosted-pr156-586eaea/verification.json`，#156 已正常合入 main `f31647d`，保险备份核对后删除清洁分支和工作树。函数+测试场景从该 main 开发，中断/AMEND 场景尚未准备完成。

## G4-D 函数与测试准备候选（2026-11-17）

`test/g4d-function-workflow-20261117` 从 main `f31647d` 独立增加原第二场景：六次 FakeModel 内完成 Dispatcher 计划、TeamLead 工单、Worker 两次写入、实际 `run` 和恰好两份源码的不可变提交。自编 `clamp(value, lower, upper)` 与五个 unittest 覆盖区间下界、内部、上界、相等边界和非法上下界；原生执行最多五秒，预期终点仍是 SUBMITTED/check pending，业务验收和交付未执行。

两场景共用已有 CSV 驱动，明确限于 csv/function；保留 CSV 默认路径、原 input 身份/只读检查、四沙箱和两环境案例，再接函数场景。成果检查器不执行捕获源码，只比对独立固定字节和原 unittest 结果；本机仅执行自编已知夹具，错误实现必须使同一组测试失败，链接/额外成果、非零退出和错误测试数均拒绝。捕获源码用独占创建的 `submitted-*.py` 与原夹具分开保存，上传范围排除数据库和工作目录。

实现 `37d352a` 的产品/C++ 改动为 0；执行/测试/集成新增 179、删除 45（总增删 224），另有固定夹具 188 行。沿用既有驱动而不复制第二套是本切片的复用选择；没有新增通用 helper。独立 GCC13 Release ELF 编译于 `e1d341e`；最终两行 Python 证据命名改动未改变任何运行库构建输入，最后 build 调用通过、ELF 相同，不把它重标为新编译。

准确候选的 CSV/函数 oracle 与预启动拒绝四项 CTest 全通过（0.24/1.23/0.28/1.26 秒），两项直接拒绝记录保留，实际 FAILED/REFUSED、spawned=false、无提交；AST/YAML、shell 参数/顺序/语法及 artifact allowlist 通过。PR158 原 head `69c4b9b` 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44179943762) 300/300、[源码](https://github.com/wmqfl861/wls861/actions/runs/44179943723)与[native](https://github.com/wmqfl861/wls861/actions/runs/44180013843)已核验通过，源码 788 个 blobs/modes 一致；native 保留原四沙箱、两环境及 CSV 五调用，新增函数六调用，原 run 68ms、exit0、五个 unittest 通过，两份原提交及其全部绑定/hash 相符。独立源码复核无发现。正常整合后的 `5405d27` [core](https://github.com/wmqfl861/wls861/actions/runs/44184695373) 300/300、[source](https://github.com/wmqfl861/wls861/actions/runs/44184695244) 789 blobs/modes 与[native](https://github.com/wmqfl861/wls861/actions/runs/44184891518)全部核验通过；实际 run100ms、五测试通过，原 CSV/隔离/回收证明保留。已由 #158 合入 main `59c568f`；原件在 `run-intake/hosted-pr158-5405d27/verification.json`。原始证据在当前私有任务根 `g4d-function-evidence/result.md`，构建在 `g4d-function-release/`，旧 B1 ELF 未覆盖。中断→AMEND→同命令恢复在独立工作树准备，尚未通过完整场景。


取消前置修复：三文件16行产品补丁将既有 batch token 贯通到 WorkerTools；在未修改生产 main `8018622` 上，新回归准确失败于 executor token 不可停止，原执行结果为 REFUSED。相同62行测试在修复后通过，原执行器保存 CANCELLED、spawned=false、all_descendants_exited=true；三次原调用、一次首次派发及原绑定/反馈均核对。相关六套 Release 通过（完整 live_workflow27方法），原始红绿 ELF/日志分开保存在 `g4d-cancel-evidence/` 与独立 build 目录。此结果是预启动取消，完整活动进程 SIGINT/AMEND 仍未执行；该补丁作为 E9 执行候选的独立提交，最终 ASan 尚待验证。既有托管 live_workflow 为184.55/600秒，新增回归约1秒 Release；最终候选须用实测重新判 D-20，不延长期限。


## G4-D 中断与 AMEND 联合准备候选（2026-11-17）

`test/g4d-amend-workflow-20261117` 已实现原联合场景的合成 TLS 入口与成果检查器，等待托管原生运行：stable 任务只执行一次有计数器的真实效果；changed 任务实际运行并留下 ready 标记后，只向父 wls run 发送 SIGINT；必须取得原 CANCELLED、隔离与完整回收证明，才执行真实 wls amend，再恢复原 argv 和冻结配置。验证同请求、update/keep、版本/工单/确认链、原业务操作不重派，以及最终不可变提交、验收、回执和经验；任何缺失均失败，不绕 UNKNOWN。全场景≤24次本机合成调用、≤90秒。

作者实现 `4c1dee7` 及持久输出检查修正 `a914a23` 的本机 Release 构建、旧 AMEND9/9、原 settled-SIGINT/恢复单例通过。新场景缺 launcher 时保留真实 FAILED/REFUSED、spawned=false、三次合成调用，未进入 AMEND；检查器的一个正例和29个反例通过。最初夹具错误包含已从 process 持久结果移走的内联输出字段，现依据原 Store/BLOB 与旧 native 原件修正；原25反例PASS记录保留，不计原生证据。

本切片测试/脚本新增246行，最小CI接线新增6/删除2；252新增比250目标多2行，用于保留完整命令及三种原件上传，不压缩断言。固定合成夹具另列（含15行Worker程序），完整事实形状只用于自测，不能冒充原生记录。取消前置的16产品/62测试仍在本候选历史中，合并须等其进入main及最终差异/门禁合规；当前没有合并许可判定或 native PASS。源码有界复核与托管运行另行核验，私有原件在 `g4d-amend-implementation-evidence/`，数据库、工作区及配置不上传。

## 原始证据与接手

私有任务根 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/`：N3 在 `g4-input-validation/evidence/`，N4 在 `g4-env-evidence/`；合 N3 后日志为 `merge-release-tests-1794864488134299077.log`、`merge-asan-tests-1794864488056552663.log`，验证收据为 `merge-validation-complete-1794866189673341120.json`。main 87c31473 的原始日志为 `evidence/main-87c3147-ci/full.log`，无 Actions artifact，原件保留于私有证据目录。远端原件在 `evidence/n3-pr136-ci/` 与 `evidence/n4-pr137-ci/`，N4 独立核对为后者 `independent-n4-d365-native-summary.json`。W128 原件在 `g4-capacity-validation/`：正式失败 `original-capacity-recovery-fixture-1794867610732020611.log`，派生诊断 `diagnostic/run-result.json`、`diagnostic/run.log`，源/ELF 绑定见 `root-summary.md` 与 `candidate-archive-binding.json`。早期测试编译/夹具失败原件保留，不计业务 RED；已修复的缺宿主旁路与准备期 deadline 两项源码问题获同项复核 PASS。只公开摘要，DB、长日志和二进制不入库。

接手先为大成果上下文重复设计引用/分页或减少重复的方案，再重测 F-15 容量，本轮不继续扩大。G4-D 真实 run 等合规原生宿主可用后再启动，CI 不调用真实模型；具体环境前提见 [STATUS](../STATUS.md)。当前实现版本见 [STATUS](../STATUS.md)；不宣称 G4 出口或升为 0.14。
