# R2.1 拒绝成果纠正与用户升级：实际验证

日期：2026-10-25。实现 **0.7.6-r2.1**，目标蓝图 **v0.4.1**。
[设计ADR-070](../adr/070-rejected-task-resolution.md)与[实现契约](../implementation/R2-rejected-task-resolution.md)先定义边界，
本报告只记录已执行的结果，不将本地验证写成远端CI或真实模型验收。

## 来源、分项提交与发布状态

恢复精确远端基线 `19ea603dd52bbcd0f34d29fb4b07f088d4f2693c`，源码树 `38b9dc4440b5939b193d835bdbfe4604f4a4d3f5`。
从Actions源码快照恢复后核对全部文件与Git树，恢复精确base commit为浅仓库，不宣称取回完整祖先。
未使用旧R1未完成检查点覆盖已发布代码。

| 提交 | 内容 |
| --- | --- |
| `b3d0708dc96c92831bfde2ea7e1cc53600474db8` | 先定义当前拒绝目标、共享返工计数、用户升级及事件消费规则；更新蓝图和ROADMAP |
| `1c5c45c758002b2cf5be0576e32a75d924c42c3d` | Store/运行器/Schema、提示词、文档和回归；冻结代码树 `1d400cab21e826c958317b1452f11ff87c14b78b` |
| 本报告所在提交 | 仅增加实际验证报告、机器结果与执行输入清单 |

**本批尚未推送。** 连接器实际只提供GitHub读取动作；终端 `git push --dry-run` 失败为
`Could not resolve host: github.com`；已连接Remote Desktop设备离线。没有向main或审核分支写入，
没有使用修改共享历史、降低权限或临时写权限工作流绕过发布限制。新提交的远端CI尚未运行。
完整本地提交、补丁和源码以会话交付包保全；最终提交后的普通推送检查保存在日志包的 `publication-check.json` 和 `push-final.log`，不能把dry-run当作实际推送。发布后必须重新核对远端SHA及其CI，旧SHA的绿色结果不可代替。

## 基线先失败与新增回归

在未修改的基线构建上，新增CLI反例先失败：否定验收后再次pump，期望一条Dispatcher后继，
实际 `calls=[]`、`rework_blocked`、原任务 `BLOCKED`，退出1。原日志为 `baseline-rejection-red.log`。
实现后新增 **22项C++、10项实际CLI、1项Python参考Schema检查**，同时接入Make和CMake/CTest。

| 范围 | 实际断言 |
| --- | --- |
| 捕获原目标 | 当前拒绝成果、原失败判断、原文件字节/哈希、两份CAS在调用前冻结；模型不能传入身份、task/submission ID、新TaskBrief或更改额度 |
| 有限纠正 | 保留原目标、验收项、约束及旧说明附加约束；task_revision增加，applied清零；TL另发新说明，Worker重新ack |
| 次数与升级 | TL/Dispatcher共用冻结计数；0额度只上报一次用户问题，换command ID、回答或重开不能增加次数 |
| 恢复与并发 | 原响应精确重放，双CAS竞争只有一个胜者；重开/AMEND/政策变化隔离旧响应并保留用量/反馈；UNKNOWN和取消不能被纠正绕过 |
| 原子性和待办 | 事件写入失败回滚任务、计数、问题和等待；缺脚本、错误响应、预算不足不吞掉待办；等待alpha不阻塞独立beta |
| 兼容 | v1/v2冻结格式、原始SQL/vendor、原问题/提交/交付/清理/窗口回归全部保留；无新依赖或迁移 |

## 实际CLI复跑

使用普通用户、真实临时SQLite和文件、固定FakeModel决策，另行执行
`measure-rejection-flow.py`（包含在日志包），结果保存为 `rejection-flow-measurement.json`：

- 冻结额度1：先产生REJECTED，再顺序执行change_task、issue_work_order、fs_write、submit、
  check_submission、accept_candidate、report_experience，共7次后继调用；累计13次调用，任务ACCEPTED，
  rework_count=1。原提交body/binding/artifact_snapshot/hash不变，原REJECTED与新ACCEPTED记录均保留。
- 冻结额度0：只创建1个用户问题；Root接收答案后该等待为AWAKE，任务仍BLOCKED，
  再pump为0次调用，计数仍0、验收仍REJECTED。回答不等于通过或扩额。

这是流程与持久化验证，不测试真实模型纠正质量；以上是重复复核，不增加独立用例数。

## 冻结执行输入的完整验证

实际执行用户 **oai / UID1000**，GCC14.2、Clang17、CMake3.31.6、Python3.13.5/jsonschema4.26.0。
不将本地Clang17/GCC14结果描述成托管Clang18/GCC13结果。

| 门禁 | 实际结果 |
| --- | --- |
| GCC Release / umask0002 | 49/49组，通过 |
| 同一Release / umask0022 | 49/49组，通过 |
| GCC Debug UBSan / umask0002 | 完整复验49/49组，通过；首次超时单独保留 |
| GCC Debug ASan / umask0002，含泄漏检测 | 首轮47/49组通过、2组超时；同一二进制隔离复跑2/2通过，合计覆盖49组，不称一次全绿 |
| 严格Make / umask0002 / 用户CPPFLAGS | 完整目标退出0 |
| Clang17 Release / umask0002 | 49/49组，通过 |

独立计数：**S1 C++398、S1 CLI122、M1内核52及演示3、参考契约37**。
同一组测试跨构建重复执行不累加；档案完整性检查不算新增产品验收。
227个执行输入全量复核一致，清单见 [SHA256SUMS](r2-executed-inputs.sha256)，清单哈希
`917df26b9cf01e35d83a44ca9abe9ee45f214a53beb3ca4b23a54fb69a3bbb2c`。本报告的文档提交不改变这些输入。
机器结果见 [r2-rejected-task-results.json](r2-rejected-task-results.json)。

```sh
# 普通用户，从源码根目录执行；Python必须具备测试依赖。
cmake -S . -B build-r2-release -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-r2-release --parallel 2
(umask 0002; ctest --test-dir build-r2-release --output-on-failure -V -j2)
(umask 0022; ctest --test-dir build-r2-release --output-on-failure -V -j2)
# UBSan/ASan分别配置Debug及WLS_ENABLE_UBSAN=ON或WLS_ENABLE_ASAN=ON，完整构建结束后再执行测试：
(umask 0002; UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-r2-ubsan --output-on-failure -V -j2)
(umask 0002; ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-r2-asan --output-on-failure -V)
# 本轮ASan首轮event_runtime/acceptance_runtime超时；构建负载结束后按原上限复跑：
(umask 0002; ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-r2-asan --output-on-failure -V -R '^(event_runtime|acceptance_runtime)$')
(umask 0002; make -j2 test BUILD_DIR=build-r2-make CPPFLAGS=-DWLS_R2_USER_CPPFLAGS=1 PYTHON=/opt/pyvenv/bin/python)
# Clang独立配置CMAKE_CXX_COMPILER=clang++、Release，再完整构建/CTest。
```

## 失败过程与复核

基线拒绝后的死路按预期复现。初次新增测试的编译/CLI运行修正了辅助函数拼写、
数据库列名和等待状态名假设；未放松生产断言。定向复核进一步先复现两项失败：
用户答案待办在缺脚本时被提前确认；纠正后新说明丢掉旧说明附加约束。
对应修复是延后事件完成判定、在Store核对保留原约束。`edge-red.log`记录18通过/2失败，
最终专项22/22及10/10全部通过。所有失败/修复日志保留，不删除反例、不改超时或关闭sanitizer。

首次两路sanitizer测试与Make/Clang编译任务并行，容器只有4核CPU额度，出现资源争用。
UBSan验收套件、ASan事件/验收套件触及原90秒上限，这三项首轮结果均为失败，不算通过。
停止编译负载后，UBSan验收套件单独复跑40.53秒通过，随后同一冻结代码完整49/49复验通过。
ASan首轮其余47组均通过；两项超时套件在同一未改变二进制、原90秒上限、地址/泄漏检查开启的条件下
隔离复跑2/2通过。因此报告ASan为两次运行合计覆盖49组，明确不描述成单次49/49全绿。
没有因此修改源码、优化级别、用例或测试超时；两份首轮失败日志和复跑日志均在交付包。

复核来自同一实施者，**没有独立第二模型审核**。使用固定FakeModel及自建临时数据，真实供应商调用、
生产数据操作和付费调用均为0；强bwrap/cgroup部署正向没有在本批执行。

## 原始日志指纹

完整原始日志、环境、测量脚本、源码清单和发布检查在会话附件 `wls861-r2-validation-logs.zip`。

| 日志 | SHA-256 |
| --- | --- |
| `release-full.log` | `8632b8a4d6f6c55fae1b1cac28c33716fb56b79be495d9b47ee5c04965c035c8` |
| `release-0022.log` | `f9b626287274d6087234d899ff816c93fdaff4b0301a947a732e52df41bbe3b7` |
| `ubsan-clean-full.log` | `abe074ce8a72169018f3cc600bd818cfe2da16b9459091eae7aafa18d9319ac7` |
| `make-full.log` | `c8415290d0eac2787043f285bc2842c2651177066ef71c6a87c028bde4d268ea` |
| `clang-full.log` | `ab12eaaac042b3a4accffa23a0c91bf34de1f4ad5a4506b3713ff195ee120c45` |
| `asan-full.log` | `88344ea50e445926c31055519800abc18612394a787240c22ebeb9508bfa3705` |
| `asan-isolated-timeout-recheck.log` | `b3237f0bd0253871ef6d647cea2332a5f0f3d8121c0265730b528049e643a72d` |
| `ubsan-full.log` | `512f07d688b441fef24f7fde576d4e7de8ec9dd9d7ecff02e8ed26f97d4326f2` |
| `ubsan-isolated-timeout-recheck.log` | `37191636896b78ae19dbfb7c620d5b0862af228f088dc586052877fee28651cf` |

## 后续范围

R2.1只开启原TaskBrief目标内的有限纠正，不实现任意任务重定义或自动放宽验收。
返工额度扩充审批、按请求隔离恢复、模型AMEND处置仍开放；R2a目录工具/PREPARED恢复、
R3协议回放/受控真实调用及强环境部署按ROADMAP继续。本轮不扩展私有行清理或daemon/API。
