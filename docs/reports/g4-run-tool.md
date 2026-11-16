# G4 命令执行阶段记录（部分完成）

2026-11-16：G4-N3 只读输入切片已由 [#136](https://github.com/wmqfl861/wls861/pull/136) 合入 main `adb89e46`，对应提交 `9355be19` 的核心、D-40 和扩展 native 均通过。G4-N4 环境版本检查在 [#137](https://github.com/wmqfl861/wls861/pull/137) 等待合 N3 后的新提交门禁；F-15 写入/提交扩容和真实 U3/U4 仍未完成。开发基线 R6 `b49bbb1`，Store 410、版本 0.13.0、六冻结格式不变，真实模型调用 0。

## G4-N3 只读输入

新请求显式冻结 `sandbox.input: {mode: read-only-v1, max_total_bytes: N}`，可信宿主 manifest 同时提供 `input_root`，缺一即拒绝；N 为 1..268435456。path/device/inode/mount_id 和 N 进入不可变 execution_policy。旧请求保留空只读 `/input`；模型卡只提示逻辑 `/input`、只读及上限，反馈不暴露宿主来源路径。契约及示例见 [Worker tools](../implementation/S1-worker-tools.md#5-强进程入口与-doctor)。

Store 在输入首次绑定及新 workspace、owned stage/final、delivery 事务中检查双向分离并核对原 policy SHA；使用元数据和固定 dirfd 父链，同设备不同 mount ID 别名保守拒绝。安全树审计拒绝链接、特殊对象、权限/UID/挂载异常，spawn 前累计普通文件逻辑大小。屏障检查实际 `/input` 身份及 private/ro/nosuid/nodev、无子挂载；`input_mount` 参与隔离判定，`runtime_mounts` 仍仅 `/usr`。

只读保证针对沙箱；可信宿主须保持来源稳定，它不是内容不可变快照。输入不授予 owned 清理权，不是 `/work` 配额；Strict 默认、Standard `strong_isolation=false` 和 owned-capacity 的 file-tools-only 限制保留。合成跨设备叶根配合真实 dirfd 父链的测试没有创建宿主挂载，不能当作真实 bind mount 验收。

## G4-N4 环境版本检查

新 `environment_checks: sandbox-run-v1` 仅取冻结 environment dependency，在既有 ack 后、Worker 预约前通过持久 `run` 执行。每项更新 binding/task CAS；同 epoch 读取原操作，PREPARED/RUNNING/UNKNOWN/缺结果不重派发。每项至多 5 秒/1024 字节，绝对截止的 stop token 覆盖 runner 准备及实际执行；完成后重新 capture。缺 WorkerTools 宿主时 opt-in 在只读 capture 守卫拒绝预约。

结果卡经原 `op_output` 授权读取原 BLOB；完整 exit 0、隔离/回收证明、非空 UTF-8 stdout 才显示版本，stderr 不冒充版本。required 失败停止下一预约，optional 确定失败可继续，UNKNOWN 保持围栏。卡片 SHA 进入 context dependencies；实际启动、复用及未运行分列，存储失败的未知计数保留 null 和 `counts_complete=false`。契约和独立 opt-in 片段见 [Worker tools](../implementation/S1-worker-tools.md#g4-n4显式冻结的环境版本检查)，旧默认文件及行为不变。

版本可用性与隔离证据独立：非文本、截断、空 stdout 使版本不可用，已完整取得的隔离证明仍按原事实报告。此处修正 FINAL 负例表把两项事实捆绑的过严文义。Standard 的版本观察不证明宿主未来包状态，检查仍可对 `/work` 产生受原 run 围栏约束的效果，不自动安装软件。

## 实际验证

| 对象 | 结果与证据 |
| --- | --- |
| R6 main `b49bbb1` | [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43946016276) 10/10 作业、450/450 CTest、Make 60+25 通过；[`v0.13.0`](https://github.com/wmqfl861/wls861/tree/v0.13.0) 注释标签已推送，无 Release |
| N3 提交 `9355be19` | [三核心](https://github.com/wmqfl861/wls861/actions/runs/43954620405) 270/270，通过的 synthetic merge `19d1dfb5` 与 head 同树；[D-40](https://github.com/wmqfl861/wls861/actions/runs/43954617821) 精确 head 11/11；[native](https://github.com/wmqfl861/wls861/actions/runs/43954623427) 精确 head 三前置与四场景通过，含 19B CSV、覆盖/追加/删除/新建拒绝和输入 SHA 保持。N3 main 新矩阵另记 |
| N3 本机 | Release 7/7，ASan 五个修改套件 5/5；末次 sandbox/worker_tools Release/ASan 各 2/2 重验通过，ASan 116.62 秒。祖先别名缺口修复后同项源码复核 PASS |
| N4 合 N3 前本机 | Release config_loader/offline_runtime/event_runtime/sandbox/worker_tools 5/5，33.31 秒；末次仅调整夹具窗口后 worker 25/25，20.54 秒。ASan 前四套通过，worker 在本机 180 秒超时，无 ASan 错误输出；按 D-39 不提高期限、不重复诊断 |
| N4 首次托管失败与修复 | `f1150e27` [核心](https://github.com/wmqfl861/wls861/actions/runs/43956865312)、[D-40](https://github.com/wmqfl861/wls861/actions/runs/43956859535) 因 mkdir 私有 phase 宏仍为二参数而编译失败，CTest 未运行；`e97d076c` 补三参数兼容，保留原回调/断言/期限，本机 mkdir Release 2.65 秒、ASan 27.64 秒通过；`e97d076c` 的[核心](https://github.com/wmqfl861/wls861/actions/runs/43958501428) 270/270 已通过，仍不是合 N3 后提交的门禁 |
| N4 历史 native | `f1150e27` 的 [native](https://github.com/wmqfl861/wls861/actions/runs/43956863385) 原四场景与独立环境两项实际通过；不能替代合 N3 后输入扩展和环境检查同提交的验证 |
| N4 定向源码/语义 | 缺宿主旁路及相对 timeout 未覆盖准备期两项均修复且同项源码复核 PASS；冻结/旧默认、原 BLOB、幂等/CAS、UTF-8/空输出/截断/126/127、撤权/AMEND/WAIT/预算/UNKNOWN/存储失败均有合成回归 |
| N4 期限证据 | 私有 phase seam 在旧 remaining 复核前观察 execute 实际 token；绝对截止后必须已取消，原 runner 无 spawn 返回取消。Release 通过；不把它写成 native setup 验收 |
| 合 N3 后本机 Release | 完整 10/10 通过，158.71 秒：acceptance_store、config_loader、offline_runtime、event_runtime、sandbox、worker_tools、process_evidence、r2a_mkdir、allocation_recovery、doctor_diagnostics；含组合 native 入口的 worker 测试程序已重新编译，入口本机未执行 |
| 合 N3 后本机 ASan | 选取七套，六套通过；event_runtime 在 180.79 秒本机时限超时，无 ASan 错误输出，原件保留，不加期限或重复诊断。此前 worker 超时单独保留且本轮不重跑；不能写成本机全绿。最终提交仍须 PR 必需检查、14 套 D-40 和 native，中间 `e97d076c` 结果不迁记 |

native CI 保留 sandbox 原 ordinary/background/timeout/cancel 四场景及 N3 的 CSV/input 检查，再独立运行 `worker_tools_tests --environment-native`，同一环境请求冻结 8B 只读输入，probe 先无输出核对 `/input/version.txt` 再读取真实 Python 版本；同时检查输入 SHA 保持、两操作输入挂载身份/只读证明、卡片及上下文无宿主路径，保留 optional missing、原 BLOB 和幂等断言。缺前提退出 77 仍为 BLOCKED；本机未运行原生入口，环境长期状态见 [STATUS](../STATUS.md)。

N3 产品增删 258 行、测试/脚本 260 行；N4 相对合入后 main 产品增删 296 行、测试/脚本 372 行（含组合 native 回归）。超过 N4 的 200 行估值用于绝对期限、两驱动接入/摘要和原证据守卫，仍低于 400 行升级阈值及 800 行硬限，测试低于产品两倍。

## 原始证据与接手

私有任务根 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/`：N3 在 `g4-input-validation/evidence/`，最终摘要 `validation-complete.json`、`tested-source-final.json`；N4 在 `g4-env-evidence/`，原日志 `release-final-1794862386737875277.log`、`release-worker-final-1794862531499543604.log`、`asan-final-1794862516246686943.log`、`mkdir-release-1794863694477177600.log`、`mkdir-asan-1794863694476866732.log`。合 N3 后日志为 `merge-release-tests-1794864488134299077.log`、`merge-asan-tests-1794864488056552663.log`，对应同名 stamp 的 build/result 原件及 `merge-tested-input-1794864877748449692.json` 保留准确源 SHA。远端原件在 `evidence/n3-pr136-ci/` 与 `evidence/n4-pr137-ci/`。

早期测试编译/夹具失败与本机超时原样保留，编译错误不计业务 RED；预算回归用冻结额度及真实 `execute(note)`，没有改 Store 触发器或生产权限。只公开摘要，DB、长日志和二进制不入库。接手先核对普通 merge 后新提交的全部适用门禁，再决定 N4 合并；输入或环境切片完成均不等于 F-15 剩余能力或真实 U3/U4 通过。
