# R1.4 管理窗口节点验证

版本 `0.9.0-r3.3b`；基线 `56185a48303deb1c72b2ec53d896e8c0b32055f0`。
本报告记录本轮新实现的实际本地执行。交付源码身份、最终联合回归和 CI 以
[五节点验证报告](five-node-validation-20261027.md) 为准；本页不引用前一版本 CI 代替新代码成绩。

最终修复后，R1.4 的 **14 项测试在 Release 和 ASan/LSan 下均全部通过**，包含 Dispatcher 和 Worker
的 live 用量投影联合回归。下文保留早期候选、真实失败和修复过程，最终成绩单列于末节。

## 实现与审查

新增冻结管理窗口和两个只读模型控制，保留完整当前契约及原管理捕获/CAS。原问题/提交/尝试仍在
原 Store 中，窗口只改变当前输入的历史投影，所有模型回读继续正常计量并留下不可变反馈。

联合审查发现初始管理视图仍依据原三项 unknown 标志判断 live 用量。这会把已完成追加对账的原 null
永久视为未决，也无法准确表达只缺 cache 证据的状态。现改读独立五维投影，附原用量与投影两种事实；
不改原结算，加入实际预取消传输和逐步对账的联合测试。已知超预留保持可见，与“未知用量”区分。
同一判断用于原 Worker 历史尾部的 Store 410 live 记录；旧 schema 和 fake 保持原字段含义。

## 聚焦测试

| 用例组 | 观察对象 |
| --- | --- |
| 冻结与兼容 | 显式新策略、外部配置更新不影响原快照、旧完整历史依旧触顶 |
| 130 次管理流程 | 交替 TeamLead/Dispatcher，6000 字节原问题，中途重开，完整当前目标仍一致 |
| 原证据回读 | 模型分页与分片回读窗口外问题，拼接 SHA、原 JSON 与 Store 原记录一致，逐次普通计费 |
| 权限和版本 | 跨请求/角色、旧 hash、旧 record_version、旧 epoch 拒绝 |
| 完整事实和容量 | OPEN、UNKNOWN、缺失原反馈不遗漏；12 个大 OPEN 问题超限时零新增预约 |
| 当前政策 | 新预约、已捕获查询和读取进行中的撤销均拒绝，错误反馈不包含被撤销的原字节 |
| 频率与重放 | 失败查询计入持久 60 秒限制，重开不清零，精确响应重放不再收费 |
| 管理提交 | AMEND 与独立 task CAS 变化拒绝旧决定；完整候选文件保留 |
| 事件连续性 | 历史查询后问题和待办仍保留，重开后可继续回答 |
| live 联合边界 | Dispatcher 和通过原计划/说明/ack 建立的 Worker 均以实际预取消传输保持零网络；仅 cache 未知仍完整可见；填齐后退出未决，原 attempt 不变 |

上述用例已加入 `tests/r1_management_tests.cpp`；测试声明不自动等同实际运行通过。
联合运行由根任务统一记录，以下保留实际开发执行与修正。

## 本地执行记录

严格 GCC 语法检查已执行并退出 0：

```sh
g++ -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -Iinclude -Itests -isystem third_party/valijson/include \
  -isystem third_party/libyaml/include -DWLS_SOURCE_DIR='"/workspace/scratch/8587bb1893aa/wls861"' \
  -fsyntax-only src/management_context.cpp src/offline_runtime.cpp tests/r1_management_tests.cpp
g++ -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -Iinclude -isystem third_party/valijson/include -isystem third_party/libyaml/include \
  -fsyntax-only src/store.cpp
```

执行测试使用普通 UID/GID 65534；首轮调用 umask 为 0002，下面列出的最终 Release 调用 umask 为
0022。所有 SQLite、问题、提交文件和故障注入均在该用户的一次性临时目录。没有放宽生产目录权限，
没有读取或调用真实供应商/凭据/付费服务。

首轮共享 Release 二进制包含前 13 项，实际 **12 通过 / 1 失败，11.677 秒**。失败在新增测试对 AMEND
目标的比较：运行器原本捕获 `Store::tasks()` 的完整团队联表行，测试误与 `Store::task()` 的单任务投影
作全对象比较。两者在当前 team/lead/worker/execution_epoch 字段上不同。修正为与原完整捕获源
`Store::tasks()` 精确相等，保留原 AMEND 及 CAS 安全断言，没有修改产品语义或放宽门禁。
原失败日志保留于 `/tmp/wls861-r1-management-first.log`，执行命令为：

```sh
setpriv --reuid=65534 --regid=65534 --clear-groups sh -c \
  'umask 0002; id; exec /tmp/wls861-five-validation/build-release/r1_management_tests'
```

此首轮已实际通过 130 次管理问答/重开/原字节回读、旧无窗口触顶、政策读取中撤销、超限零新增、
事件待办及完整候选原字节等用例。首轮 12/13 不表示最终整套通过。

修正并重建后，根任务以普通用户对候选输入运行聚焦 CTest：`r1_management` 实际 **14 通过 / 0 失败，
13.21 秒**，包含 Dispatcher live 投影联合用例。原 CTest 输出独立保存到
`/tmp/wls861-r1-management-14.log`，SHA-256 为
`5532c10b89c4bf9a15495d61ee5b06f07ffd6d23b4eb468a1cf44f7a516ae7c1`。
此后同一第 14 项增加了 Worker 原 Store 预约/传输/核对断言，严格语法检查退出 0；该扩展已随下述
最终统一输入执行。此前 13.21 秒只表示较早候选，不是包含 Worker 扩展的最终成绩。

### ASan 发现的真实内存缺陷

随后联合 ASan 聚焦执行中，`r1_management` 在 1.39 秒明确失败，报告
`AddressSanitizer: heap-use-after-free`；本次 7 套聚焦测试为 6 通过 / 1 失败。故障在产品实现
`Store::management_view` 写入问题目录的 `omitted_completed_count` 时发生，独立于前面的测试投影比较修正。
右侧直接引用同一个 Boost.JSON object 已有的 `completed_count`；左侧 `operator[]` 为新增键扩容，
使右侧 value 引用在复制赋值前失效。提交目录的同式赋值具有相同风险。

修复先把问题、提交、尝试三个计数读取为独立 `int64_t` 标量，再插入省略计数字段。没有改变目录数值、
历史选择、测试断言或 sanitizer 设置。对本轮新增 JSON 插入逐处复核，其余赋值来源为独立对象、
已完成构造的临时值或标量，不存在相同的直接同对象 value 引用写入。严格 GCC 语法检查再次退出 0。
已有管理窗口用例在 ASan 下即可稳定触发该缺陷，修复后仍运行同一套断言。

原始失败输出保留于 `/tmp/wls861-five-validation/asan-focused.log`，SHA-256 为
`622e926561d49dbd355f390173b03d6db699b2a1927e88cb644d81e8c7de5e61`。
持久失败诊断另见 [five-node-asan-initial-failure.txt](five-node-asan-initial-failure.txt)，保存时仅去除行尾空格。
此前 Release 通过不替代这一失败；下面的最终成绩来自修复后重新构建的输入。

## 最终修复后执行

最终 281 项执行输入清单的 SHA-256 为：

`42b2f25aba2bc04b13777d9b5a5518beeb2a898e118eb37031a4e29a6c9cb59b`

对应 [five-node-inputs.sha256](five-node-inputs.sha256)。该清单包含 Worker 联合断言和 JSON 引用生命周期
修复，实际执行副本与交付源码逐字一致。构建、测试均为普通 UID/GID 65534；保持原断言、权限规则和
sanitizer 检查，未通过改变门禁规避此前错误。

| 最终门禁 | R1.4 实际结果 | 同批联合结果 |
| --- | --- | --- |
| Release，umask 0022 | 14 通过 / 0 失败，13.46 秒 | 完整 70/70 套件通过，110.97 秒 |
| ASan/LSan，`halt_on_error=1:detect_leaks=1` | 14 通过 / 0 失败，167.49 秒，无 sanitizer 诊断 | 聚焦 7/7 套件通过，167.50 秒 |

这里只列本节点原始输出的独立摘录
[r1-management-final-detail.txt](r1-management-final-detail.txt)，SHA-256 为
`e0b35d1839139d0bc5cc8d49669b6db5b5b75f6fbe64f9608d39240445558cd6`。
摘录保留 CTest 命令、目录、逐项 PASS、总数和耗时，没有复制其他套件；源日志分别为
`/tmp/wls861-five-validation/release-0022-detail.log` 和
`/tmp/wls861-five-validation/asan-focused-detail.log`，各自的完整日志 SHA-256 写入摘录头部。
最终其他 umask 和远端 SHA 对应 CI 的结果统一见[五节点验证报告](five-node-validation-20261027.md)。

## 非证明范围

该测试证明确定性窗口、原证据、权限、容量和状态连续性；不证明真实 LLM 的回读选择或管理质量，
也不证明无限历史的时间复杂度、数据库维护、远程鉴权或并发服务。运行器仍为 Fake，本节点不提供
真实模型自动业务执行入口。
