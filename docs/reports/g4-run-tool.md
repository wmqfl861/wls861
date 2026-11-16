# G4 命令执行阶段记录（部分完成）

## G4-N4 环境版本检查

本切片在 R6 `b49bbb1` 上实现显式冻结的环境检查；本机 Release 五套通过，ASan 四套通过、一套本机超时。托管原生正向和最终提交门禁待运行；真实模型调用 0，不作为 U3/U4 验收。

新 `environment_checks: sandbox-run-v1` 只使用冻结 environment dependency，在原 ack 后、Worker 预约前经持久 `run` 检查。每项重新取得 binding/task CAS；同 epoch 只读原操作，UNKNOWN、缺失结果和首次派发围栏保留。结果卡从原 BLOB 授权读回，只有完整 exit 0、隔离/回收证明和非空 UTF-8 stdout 才显示版本；stderr 不冒充版本。卡片 SHA 进入 context dependencies，旧缺省无字段、不执行。详细契约及独立 opt-in 示例见 [Worker tools](../implementation/S1-worker-tools.md#g4-n4显式冻结的环境版本检查)。

| 验证 | 当前事实 |
| --- | --- |
| 本机 Release | config_loader、offline_runtime、event_runtime、sandbox、worker_tools 完整 5/5 通过，最终产品摘要和 stop-token 观察加入后 33.31 秒；最后仅调整 deadline 夹具窗口的 worker 25 用例复验通过，20.54 秒 |
| 本机 ASan | config_loader、offline_runtime、event_runtime、sandbox 4/5 通过；worker 在 180 秒本机测试时限超时，无 ASan 错误输出。按 D-39 保留原件，不提高本机时限、不做历史诊断；托管 D-40 才是合并门禁 |
| 首次托管编译与最小修复 | `f1150e27` 的 [PR 三核心](https://github.com/wmqfl861/wls861/actions/runs/43956865312)与 [D-40](https://github.com/wmqfl861/wls861/actions/runs/43956859535)均在编译时发现 `r2a_mkdir_tests` 仍定义二参数私有 phase 宏，CTest 未运行。补三参数兼容且保留原回调后，本机 r2a_mkdir Release 2.65 秒、ASan 27.64 秒均通过；D-40 清单新增该套，独立 N4 为 12 套，合 N3 后为 14 套 |
| 定向源码复核 | 首版发现缺 WorkerTools 可绕过必需检查、相对 timeout 未覆盖 runner 准备耗时两项；均已修复并获同项源码复核 PASS |
| 合成语义 | 最小 note/run/op_output 授权下缺宿主不预约；冻结/旧默认、重复准备、CAS、结果指纹、UTF-8/空 stdout/截断/126/127、AMEND/WAIT/撤权/预算/UNKNOWN/存储故障均有回归 |
| 期限证据 | 复用既有私有 phase seam，在旧 remaining 复核之前观察 execute 的实际 stop token；睡过绝对截止后 token 必须已取消，原 runner 返回无 spawn 取消。Release 回归通过；ASan worker 的本机超时不改期限，未将合成证据写成原生 setup 验收 |
| 本机 native | NOT_RUN，host prerequisites BLOCKED；未使用伪路径、改主机配置或调用 sudo |
| 托管入口 | 既有受委派用户单元先跑 sandbox 原四场景，再运行独立 `worker_tools_tests --environment-native`，检查真实 Python 原输出、optional missing、原 BLOB 和幂等复用；退出 77 保持 BLOCKED |

保留旧函数模板字节，另提供 opt-in 覆盖片段，避免新约束使原 framed-v1 配置失效。产品增删实计 296 行，高于 200 行估值，原因是绝对期限、两驱动入口/摘要及原证据分类的完整守卫；仍低于 400 行升级阈值和 800 行 PR 硬上限，测试/脚本增删 349 行，低于实际产品两倍。存储失败的未知计数用 null 与 `counts_complete=false`，不写成零。

版本可用性与隔离证据分开报告：exit 0 但非文本、截断或 stdout 为空时版本不可用，原始完整隔离证明仍保持其真值；UNKNOWN/缺证明才不报告 verified。这修正 FINAL 负例表将两项事实捆绑的过严文义，不改写原执行事实。

本机原件位于 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/g4-env-evidence/`；最终日志为 `release-final-1794862386737875277.log`、`release-worker-final-1794862531499543604.log`、`asan-final-1794862516246686943.log`。首编测试类型错误、自产 SQL 夹具版本错误和首轮 ASan 夹具失败均原样留存；最终修订的预算回归走冻结一操作额度与真实 `execute(note)`，没有修改 Store 触发器或生产权限。公开只保留摘要，不提交 DB、日志或二进制。

宏兼容修复原件为同目录 `mkdir-release-1794863694477177600.log`、`mkdir-asan-1794863694476866732.log`；远端原始失败留在同任务根 `evidence/n4-pr137-ci/`，编译失败不计作业务红例。修复不改产品代码、原测试断言、期限或 sanitizer。

N3 先合入，随后普通 merge main 到 N4 并拼接本阶段报告，保留双方功能与 CI 项；在最终提交重跑 PR 必需检查、D-40 `asan-affected` 和 `standard-native`，不强推。当前源码检查、本机测试、托管门禁和真实验收分别登记。
