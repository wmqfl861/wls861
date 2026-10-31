# wls861：独立审核后的定向整改任务

你接手的是A0–A5后续整改，不重做A0–A5，不自动启动全部B0–B6。
仓库：https://github.com/wmqfl861/wls861
审核基线：99c9063f3e29cd6d69c56fb15815715d499a374a
集成PR：https://github.com/wmqfl861/wls861/pull/10

主协调Agent已用该精确提交的未修改生产源码/库，独立复现两项P1问题：R-A3-01命令重放取消新调用；R-A4-01跨池资源FIFO死锁。它们阻断A3/A4采纳，不只是覆盖缺口。远端12/12绿色仍保留，不将绿色理解为否定这些反例。
本轮允许你实现这两项最小修复、回归、诊断工具改进、正常commit/push和Draft PR。主协调者负责最终复核；不合并main或任何已有PR，不强推、不放宽安全、不调用真实模型、不产生付费请求。

## T0：精确接手

先核验数据盘实际挂载、当前UID、磁盘空间、工作区、未提交改动和相关进程。保留原/mnt/d任务资产；不要因为目录叫/mnt/d便假定挂载存在。
git fetch后核验PR10的当前head/base及该SHA祖先关系。新建数据盘独立worktree/修复分支，例如fix/a3-a4-review-followup-<唯一后缀>，不要覆盖原分支。若远端已前进，先审新增差异，不退回或强推。
阅读AGENTS、ADR092/093、A3/A4契约、现有测试，以及本次REVIEW.md和independent_async_repros.cpp。报告中的ChatGPT容器路径不是你的服务器路径。
先用99c9063f自己的headers、源和库编译两个复现；原Scheduler9例、Async18例通过与新反例失败可以同时成立。保持夹具前置和真实Store路径，不删守卫、不用fake Store。两个反例都应确认命中目标断言，不把编译错误/超时算红例。
新复现代码可以纳入原async套件；它只是夹具复用方式，不要求长期保留include整个测试cpp的布局。不要修改原18例语义。

## T1：R-A3-01，控制命令重放必须无新执行副作用

故障路径：AsyncCoordinator::process_control在Store返回历史AMEND结果后，仍停止当前同request的所有jobs。
复现：执行AMEND -> 捕获并启动修改后的新调用 -> 原Root/原CAS/原参数/同command_id重放AMEND。原结果、request、budget都没变，但新调用stop被置位。

修复必须区分实际新状态转换与精确重放；停止目标只涵盖此次实际转换废止的原捕获工作。优先在协调层或不破坏旧格式的Store结果机制实现，不修改持久历史结果/指纹。明确为何判断与Store事务、单宿主写者顺序一致。

验收至少包括：
1. 同源红例修复后通过，后续合法模型未收到旧重放的stop，能正常结束和结算。
2. 首次真实AMEND仍及时通知旧在途任务，迟到响应保留原身份，不能应用旧业务。
3. 立即重放、迟到重放、多次重放、同键异参拒绝、合法新命令分别验证。
4. cancel精确重放不得错误停止后来合法的终态经验/收尾；不为测试建立不存在的业务权限。
5. 旧Actor/旧CAS、UNKNOWN、预算和原事件前缀不变性继续有效。

## T2：R-A4-01，资源等待不能占住前驱所需的执行槽

最小合法配置是manager2/worker2，各reserve1。
先让manager请求B与worker请求A占住普通槽；依次排M_A(X)、W_B(Y)、M_B(Y)、W_A(X)；释放原屏障。
本池公平轮转会先选M_B、W_A，两者占槽等待另一池队列中的前驱，形成循环。模型未进入complete，不能靠模型timeout解决。

架构约束：跨池资源就绪判定必须在占执行槽之前生效。建议在现有host维护有界资源头/待提交状态，只把具备资源资格的job交给现有M1公平队列；也可提出等价共享资格方案。不要再创建第二个独立公平调度器。
保持FIFO到原结果提交为止；资源资格不是Store权限，真实首次执行仍核原Actor/session、CAS/epoch和唯一claim。
保留普通与审核双容量、独立控制、每次已准入的结算保留槽、持久UNKNOWN、失败收证和关闭生命周期。

禁止：提高线程数凑通过、借审核槽、删除FIFO、轮询sleep、刷新旧身份、放宽deadline、取消正常请求来打断死锁。

验收：
- 以上同源反例红转绿；四个调用实际完成，调用/效果/结算计数正确，无人为取消解锁。
- 2/2和默认4/8容量下的有界饱和变体。
- normal/review/handoff交错，同资源跨池先后顺序，不同资源兄弟推进。
- 排队取消/AMEND、前驱结果写入失败、资源保留、异常/close/drain均能有界收尾。
- 等待资源不占据可执行槽；确实在途的模型仍占真实执行资源，不提前释放未知账目。
- 原公平性红绿、18个异步用例和M1兼容保持。

两个修复分别形成小提交；T2的接口设计由非作者交叉复核。记录实际发现，不声称同模型协作等于第二模型审核。

## T3：最终输入验证与发布

正式验证前冻结最终产品输入；运行原有与新增回归、Release两种umask、Clang、UBSan、GCC ASan/LSan及原Make-ASan，明确哪些在本机、哪些在CI。
原产品注册数92；若只在原async套件增子例，不机械改成94。若真实增加注册项，审阅名字、命令和deadline目录后同步修改runner默认/CI/自检，不能自动接受当前计数。
不同编译器各用独立构建目录。同源红绿各用自己的头/库，保存命令、退出码、日志、源和二进制身份。保留首次失败，不能重复直到绿色后隐藏历史。
已知本机无法通过的项仍如实报告；先完成针对两项缺陷的绿例和适用矩阵，不能借历史环境问题跳过新修复验证。最终正式采纳交主协调Agent，不自改PR_REVIEW_COMPLETE。
推送独立Draft PR，以实际直接依赖为base；回报完整SHA、修复对应文件和CI链接。不移动main，不合并PR1–10，不改已有堆叠关系。

## T4：A1诊断只对确认分歧做有界实验

保持历史GCC48pass/2failed/39timeout、Clang29pass/10timeout和旧live_workflow原因未闭环。
这两个新A3/A4问题不能用于解释早于它们的历史失败。
优先诊断两个真实FAILED套件，而不是无计划重跑全套：
- https_transport/test_partial_request_upload_deadline_is_not_free_retry_authority：peer.prefixes为0而期望1。增加测试专用阶段观察，区分准备、TLS握手、实际发送/接收与4096字节前缀保存；prefix0不等于零上传，也不授予重发。
- live_model_store_tls：effective_live_capacity的18秒子进程TimeoutExpired，以及large_escaped_valid_output的accepted0。保存原失败返回中的deadline/状态/拒绝原因与必要脱敏计数，区分业务/结算拒绝与运行期限，不记录秘密。
保留原断言、3500ms/18s/300s等各场景原界限；诊断插桩独立，不能把延长期限后通过写成正式结果。
先形成可验证假设，再做同输入对照。没有根因证据则标UNRESOLVED，不把不同机器的绿色或单项重跑解释为历史失败消失。
不要同时运行重编译和多组sanitizer影响诊断。

## T5：A5证据补强可独立推进

保留Windows Server2022/elevated的组件构建事实，不写成Windows11普通用户安全通过。
先让原生CI在失败和成功时保存完整LastTest及逐方法输出，避免只保留被截断的JUnit正文。不要靠把skipped当通过补齐证据。
已有Windows11普通用户机器可用时再运行明确支持组合，记录OS/build、token、NTFS、工具链、资源与拒绝路径；没有环境则保持NOT_RUN，不新购资源。
本卡不扩展到未验收的Store/VFS、任意run、强沙箱或工作区删除。

## 完成回执

逐项返回T1/T2的修复前真实红例、修复后绿例、当前提交与最小差异；T3各平台矩阵；T4根因已证/仍未知清单；T5实际证据范围。
状态分开：IMPLEMENTATION_COMPLETE、LOCAL_VALIDATION_COMPLETE、PUSH_COMPLETE、REMOTE_CI_COMPLETE、PR_REVIEW_COMPLETE、MERGED。
当前决定：A3/A4 CHANGES_REQUIRED；A2/A5可保留候选成果与独立推进；不批准当前集成合并。
修复先于B1 DAG/独立审核运行器扩张。架构主方向继续采用单Store写者、事务外执行和现有M1公平调度，不因这次发现推翻整个项目。
