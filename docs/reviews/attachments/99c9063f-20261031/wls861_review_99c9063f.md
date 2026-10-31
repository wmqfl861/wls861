# wls861 A0–A5 独立复核：99c9063f

审核对象：`99c9063f3e29cd6d69c56fb15815715d499a374a`，PR #10。
日期：2026-10-31（UTC）。审核结论：**A3/A4 当前实现需修改；不得以 CI 绿色采纳或合并。**
本轮没有修改仓库生产代码、合并 PR、调用真实模型或操作用户服务器。

## 1. 方法、真实执行和限制

通过 GitHub 连接器读取 PR #10、依赖 PR #8、精确源码及 Linux Push/PR 作业；下载最终源码 artifact 13042985151，并验证归档 SHA-256。
重新从全部 tar 文件及 Git 执行模式计算 Git blob/tree：777 文件、28,414,682 bytes，tree 为 `108499ced6e7a907eb060d125ee907d93b61789d`，与提交归档一致。
另外逐项核验已提交文本证据包的 74 份正文和成员清单，没有执行归档中的代码。

重点直接阅读 AsyncCoordinator、Scheduler、Store::claim_offline_call、OfflineRuntime 预约/响应接入及现有异步测试；A1/A2/A5作限定范围契约、日志及源码检查。这不是整仓逐行无缺陷证明。

独立执行环境：本会话隔离 Debian Linux 容器，普通 UID 1000，GCC 14.2.0，C++20，Debug -O0 -g0，未启用 sanitizer；Boost 1.83、SQLite 3.46.1。不是用户 Ubuntu 服务器，也不是其 GCC13/Clang 门禁。
首次链接因 CMake 选择 /usr/local/lib/libcurl.a 缺传递依赖失败；随后显式选择系统共享 libcurl，链接成功。这个环境准备失败原样留存，不计作产品反例。生产源码、头文件和现有测试未修改。

实际运行：
- 原 scheduler CTest：9 子例通过。
- 原 async_coordinator CTest：18 子例通过。
- 两项 CTest 总计2/2，exit0，11.28秒，原期限不变。
- A2 离线契约：9/9，exit0。
- 即时失败日志自检：4/4，exit0。
- GCC runner 自检：24/24，exit0；不是产品 ASan 运行。
- 两个新增独立反例分别 exit1，命中下面的业务断言；不是构建失败、外层超时或删守卫变异。

已独立下载并核验最终 Push GCC ASan artifact 13043386509：ZIP SHA-256 `9835fe4765149ff0cd3a14db65b25ea112ba5762d8d21de29ec3d9528e57e4b8`，head99c9063f，92项JUnit无 failure/skipped，777源码内容与最终树一致，61构建身份前后不变，UID1001，detect_leaks=1。Linux Push/PR的12项作业状态均为success。现有测试成功与新增反例失败并不矛盾。

## 2. R-A3-01 — P1：AMEND 精确重放错误停止新调用

位置：`src/async_coordinator.cpp:258–266`，关联 `src/store.cpp:3021–3040`。
Store::amend在原命令精确重放时只返回历史结果，不进行新的状态转换；但是 process_control 无条件对同请求全部 jobs 调用 request_stop。

最小复现：
1. 在当前 Store 中捕获 Root和CAS，执行一次AMEND并取得原结果。
2. 在AMEND后的当前需求修订下捕获、预约并启动新的Dispatcher调用。
3. 用第一步原Root、原CAS、原参数和同command_id精确重放。
4. Store返回相同结果，请求和预算保持不变，但新调用收到停止请求。

实测：
```
AMEND_REPLAY original_result_equal=true request_unchanged=true budget_unchanged=true new_call_stop_requested=1
FAIL ... !model->observed_stop.load()
RESULT 0 passed, 1 failed
```
最后一次对应源/库身份冻结的运行 exit1，0.164712秒。可见 `final-repro-amend.log` 与 `independent-reproduction.json`。

影响：重放具有额外执行副作用；正常响应丢失后的幂等重试可能中止后续合法工作。未证明数据库越权或历史账目损坏，不能扩大结论。

修复约束：区分此次实际生效的状态转换与只读命令重放；停止范围绑定被这次转换废止的原捕获身份/需求修订/epoch。不得通过禁止合法精确重放、修改旧事件字节、刷新旧Actor权限或禁止全部后续任务修复。补充立即重复、迟到重复、同键异参、首次真实AMEND、取消与终态收尾场景。

## 3. R-A4-01 — P1：跨池FIFO与公平轮转形成循环等待

位置：`src/async_coordinator.cpp:178–194,202–211,366–395` 与 `src/scheduler.cpp:214–252`。
Callback已占用M1执行槽后，才进入host级跨池资源FIFO等待。M1的本池公平轮转并不知道另一个池中的资源前驱。

合法最小配置：management_workers=2、worker_workers=2，分别保留1审核槽；两个池的普通执行槽各1。
先用manager请求B、worker请求A的正常模型屏障占住普通槽。再按全局顺序提交：
- M_A：request A，resource X；
- W_B：request B，resource Y；
- M_B：request B，resource Y；
- W_A：request A，resource X。
全部是Normal。释放前两个原模型屏障后，本池轮转先选择M_B和W_A；它们进入AwaitingClaim但分别必须等全局更早的W_B和M_A。

循环：`M_B(占manager槽) -> 等W_B(排worker队列) -> W_A(占worker槽) -> 等M_A(排manager队列) -> M_B`。
两个审核槽不能被普通任务占用。四个新模型均未进入complete，因此模型自己的deadline不会解除这个等待。

实测：
```
CROSS_POOL_FIFO cycle=1 awaiting=2 queued=2 executing=0 resource_blocked=2 managers_normal_running=1 workers_normal_running=1
FAIL ... !cycle
RESULT 0 passed, 1 failed
```
最后一次运行exit1，0.816298秒。反例先观察AwaitingClaim，再给host额外完整poll机会，确认仍无进展；并非仅凭短时间没完成推断死锁。代码在正确实现上允许四个合法调用完成后通过，不要求修复版也进入故障状态。析构close负责释放等待并收尾，本轮无遗留测试进程。

修复方向：在占用执行槽之前做跨池资源就绪判断。可在host维护有界资源头/待提交状态，仅把资源可执行的工作交给现有M1 submit_fair；或者采用同等安全的共享可运行性仲裁。继续复用M1公平算法，不引入第二套业务授权。
不得增加线程数、借用审核预留、放弃FIFO、睡眠重试、增加deadline或取消正常请求来掩盖循环。保留Store首次claim、当前Actor/CAS、原结果和预算、结算保留与控制可达性。

## 4. 其他保留事项与采纳边界

A3/A4方向可保留，但当前代码需修改，两项均为新确认的生产行为缺陷，不只是测试覆盖不足。
A2离线包与A5原生组件可以独立继续；本轮未发现要求撤销这些成果的确认问题，但不宣称其全部安全性或完整产品验收通过。
Windows实际报告是Server2022 elevated token的组件测试，不等于Windows11普通用户、Store/VFS、文件/进程/强隔离/清理完成。需要补原生完整LastTest或逐方法即时日志，不能仅依赖截断JUnit正文。

历史本机GCC 48pass/2failed/39timeout及Clang29pass/10timeout仍开放。证据包中两项GCC FAILED是https_transport与live_model_store_tls，具体包含：prefixes长度0而预期1、18秒TimeoutExpired、accepted0而预期1。另有整组超时前的FAIL/ERROR，不能概括为纯性能超时。新的GCC结果也否定了“只对齐编译器就能认定根因关闭”的推断。
本次两个新缺陷来自后继异步/公平代码，不能解释早于它们的772feaee历史失败。

## 5. 交付

`independent_async_repros.cpp`：新编写的可移植复现源，包含当前tests/async_coordinator_tests.cpp作为夹具，使用该提交自己的头和库。通过`-I<repo>/tests`、`-I<repo>/include`及`-DWLS_SOURCE_DIR="<repo>"`构建；使用对应生产库/原链接依赖，不能混用旧ABI。
`independent-reproduction.json`：完整编译/运行argv、退出码、日志摘要和前后源/库/二进制身份。
`source-verification.json`、`ci-asan-verification.json`、`committed-evidence-verification.json`：本轮实际核验。
原有套件及自检日志、首次环境链接失败和修正日志均保留。

尚未执行：本轮全部92项、用户服务器新矩阵、Windows实机、sanitizer复现、修复候选绿例。没有宣称修复提交或GitHub批准/合并。
