# G4 命令执行阶段记录（部分完成）

日期：2026-11-16。开发基线为 R6 `b49bbb1`（[#135](https://github.com/wmqfl861/wls861/pull/135)、已推送注释标签 `v0.13.0`）。本次只做 `/input` 只读来源和输入树总字节限制；Store 410、版本 0.13.0、六个冻结格式不变，真实模型调用 0。F-15 的 `fs_write`/提交扩容、G4-N4 和真实 U3/U4 仍须分别完成。

## 实际行为

新请求显式冻结 `sandbox.input: {mode: read-only-v1, max_total_bytes: N}`，并由可信宿主 manifest 提供 `input_root`；两项缺一即拒绝。N 为 1..268435456，目录的 path/device/inode/mount_id 和 N 进入团队不可变 execution_policy。默认配置不加字段，旧请求继续使用空只读 `/input`；模型环境卡只提示 `/input`、只读和字节上限，进程反馈的输入挂载身份不含宿主来源路径。配置示例和边界见[Worker 工具契约](../implementation/S1-worker-tools.md#5-强进程入口与-doctor)。

Store 在首次输入绑定以及新 workspace、owned stage/final 意图和 delivery 的事务中检查双向分离，先核对已有 execution_policy SHA。检查只用元数据和固定 dirfd 父链；同 device、不同 mount ID 的别名布局保守拒绝。安全树审计拒绝链接、特殊对象、权限/UID/挂载边界异常，并在 spawn 前累计普通文件逻辑大小。启动屏障验证实际 `/input` 身份及 private/ro/nosuid/nodev、无子挂载；`input_mount` 证据参与最终隔离判定，`runtime_mounts` 仍只保留 `/usr`。

只读保证针对沙箱；可信宿主须保持来源稳定，它不是内容不可变快照。输入没有 owned 清理权，也不是 `/work` 磁盘配额；Strict 默认、Standard `strong_isolation=false` 和 owned-capacity 的 file-tools-only 限制保留。

## 验证与限制

| 项目 | 实际结果 |
| --- | --- |
| 本机 Release，uid 1000、数据盘新构建、并行最多 2 | 7/7 完整 CTest 套件通过：sandbox、worker_tools、config_loader、acceptance_store、allocation_recovery、process_evidence、doctor_diagnostics；0 跳过 |
| 本机 ASan，五个修改的 C++ 套件 | 5/5 通过、0 跳过；最后结果保密/结算变更后的 sandbox/worker_tools Release 与 ASan 均重新通过，ASan 2/2 用时 116.62 秒 |
| 定向源码复核 | 首版发现跨设备 protected leaf 的祖先别名缺口；补固定父链后该项复核 PASS |
| 本机边界回归 | 等于限额、超 1 字节、多文件合计、计数溢出、符号/硬链接/FIFO、身份替换、显式 opt-in/旧冻结兼容、Store 正反登记及预检后新增登记均通过 |
| 结算形状回归 | 复用 test-only observer 的明确 synthetic standard+input 证明，检查 process_operation_outcome、Store 原样持久化和幂等重放；不计 native 正验 |
| 别名测试性质 | 合成跨设备叶根配合真实 dirfd 父链；未创建宿主挂载，不能当作真实 bind mount 验收 |
| 托管 native 扩展 | 待本次 PR 实际提交执行；保留 ordinary/background/timeout/cancel 四操作，在 ordinary 内增加 CSV 精确读取、覆盖/追加/删除/新建拒绝、输入 SHA 保持及独立输入挂载证据 |
| D-40 | `asan-affected` 保留原六套，加入上述五个修改的 C++ 套件，共 11 套；待本次 PR 实际提交运行 |

本机 native 仍 BLOCKED：既有 `uid_map: Operation not permitted`，缺受信 bwrap 与专用 cgroup 配置。没有修改主机设置或调用 sudo；没有配置不等于已证实无 delegation。Windows 11 NOT_RUN，U1/U2 未通过，旧 R2/R3/R5 不重放。

产品非移动增删实计 258 行（含新增 `input_guard.hpp`），较 N3 的 250 行估值多 8 行，用于完整祖先守卫及反馈来源保密；测试/脚本增删 260 行，测试增删低于实际产品的 2 倍，未改既有期限或删除断言。初次开发的三次测试编译错误分别为 JSON/字符串比较、范围循环复制告警和缺少 WorkspaceAccess 构造参数，已修正；新增结算断言还修正了 Store `accepted` 为整数 1 的类型预期。原失败日志保留，不计为业务 RED。

## 证据与接手

本机原件位于 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/g4-input-validation/evidence/`：`release-library-build.log`、`release-focused-build*.log`、`release-first-tests.log`、`release-remaining-tests.log`、`release-final-tests.log`、`release-worker-final-tests.log`、`asan-tests.log`、`asan-feedback-tests.log`、`tested-source-final.json`；最终校验与哈希见 `validation-complete.json`。只公开本页摘要，构建产物、SQLite fixture 和长日志不入 Git。

接手者在 N3 最终提交上核对 PR 必需检查、11 套 D-40 和扩展后的 native 四操作，再合 N3；N4 普通 merge 最新 main 后重测，不强推。N3 输入切片的成功不能替代 F-15 剩余写入/提交扩容或 G4 的真实 U3/U4 验收。
