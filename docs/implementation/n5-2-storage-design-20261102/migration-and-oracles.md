# N5-2 v1：迁移、旧程序拒绝和验收事实

状态 **PROPOSED / NOT_IMPLEMENTED / NOT_RUN**。以下命令意图、查询和 oracle 是未来实施要求，
本包只完成文档静态检查；没有生成 014，没有构建 412，也没有运行以下生产测试。

## 版本注册和兼容选择

| 资产／版本 | 当前授权与本包建议 | 禁止的兼容捷径 |
| --- | --- | --- |
| 001–012 / 既有 401–410 | 发布字节与注册身份保持 | 修改原 SQL、Schema、迁移 hash 或历史 manifest |
| 013 / 411 | N5-1 已批准的事件关联；其实际源码／运行证据由独立实现 PR 给出 | 在 013 偷加 DAG／ticket，或将设计批准写成验证通过 |
| 014 / 412 | 本包**拟议**：graph/node/edge/head/proof/acceptance receipt、candidate 数据对象与映射、metadata/限额/admission barrier | 本文不预占生产 DDL 文件，也不认为协调者已批准 412 |
| 后续版本，编号待分配 | 本包**拟议**：双边 scope、review-only admission/pair/ticket/verdict/check、父预算授权／借记／投影 | 不将 ticket 与 014 默认捆绑，不跳过 G 的独立审核，不能把 old411 视作懂新协议 |

真正生成新资产时，原 MigrationRegistry 的连续版本、完整 SHA-256、runtime resource catalog、
资源 manifest、install/capture/verify、CLI/native detached bundle 和历史测试应一起更新；
不能只提升 user_version。每个迁移在原 AllRequests 显式维护边界单事务执行；已填充库的
OneRequest/ReadOnly/ReviewPair 不做隐式升级。空库安装也须完整经过批准资产序列。

014 只能创建追加表、trigger、索引和精确 metadata，不更改旧表列值、Plan 原文或 request
snapshot。迁移时冻结全库 admission sequence barrier；所有既有请求默认 legacy，不得
依据“当前还没有 task”把旧请求转换成新图。mode enable 仅允许 barrier 后的明确新准入请求，
原 key 重放继承原 mode，不能重放准入绕 barrier。旧独立请求继续走原接口／守卫。

仅 schema 新版而该 request 未启用协议时，不读取缺省 graph/head 当作权限。启用 graph 的
请求缺必要新事实应 Corrupt/HOLD；未知 format、未知失败策略、未知 migration hash 必须拒绝。
图模式请求不能走独立计划／无图验收回退；审核模式不能走普通 child attempt 回退。

## 旧 411 拒绝与回滚

必须保存真正旧 411 的源码输入与 ELF SHA-256，用这个旧 binary 分别 ReadOnly 和 Takeover
打开一份实际迁到 412 的临时库。两次都应明确 Unsupported/未知受支持版本，在任何恢复、
Actor 签发、graph/review/预算／业务写入前停止。用新 binary 指定 target=411 只能验证资源
配置，**不是旧 411 兼容测试**。也不能把编译失败、缺 SQL 资源或错误 fixture 当成拒绝成功。
测试需先用新 binary 确认 fixture 的实际 user_version、完整 metadata/hash 和原行有效。

旧库解释测试保留现有 402–410 历史路径；新 binary 打开真正 411 且不升级时必须维持 legacy
调用及未启用模式的拒绝。迁移后的旧请求 bytes/hash/budget/epoch/原 attempt/response/feedback
逐项对照，维护恢复产生的合法新增 recover/UNKNOWN 另外保留，不伪称所有可变行永远不变。

回滚建议是**新数据停用＋保留新版读取／结算**，不提供 DOWN SQL：停止新模式准入和派发，
将开放 link/票据置 HOLD；保留原 attempt、未知费用、候选、图与响应。已经实际升级的活跃库
不能交给旧程序继续写，不能手工把 PRAGMA user_version 改回 411。

结构迁移前在无 writer、drain/受控关机之后制作一致备份，保存主库及正确 SQLite checkpoint／
WAL 处理方式、文件权限、全部资源输入和 SHA；不要只复制仍在写的 db 文件。
只有确认升级后**没有任何新业务、副作用或新证据需要保留**，协调者明确选择恢复该一致
备份，才可退回旧 binary。升级后已经派发、产生账目或收到迟到证据时，恢复老备份会丢事实，
必须走新版修复／受限结算，不以回滚删除事实。SQLite 恢复成功不是进程退出或清理许可。

## 故障点与事务原子性

| 注入边界 | 成功时必须共同存在 | 中断／失败后允许状态 |
| --- | --- | --- |
| graph publish 各 INSERT 与 commit 前 | 原 Plan、全部三个 task/team、两个 edge、graph/head、初始待办、原 command event | 全无；不能留单独 plan、团队目录所有权或醒错 ID |
| source ACCEPTED 与 receipt/map 之间 | 原 acceptance/task/submission CAS、experience job、新 receipt/map | 全部原子；不能原 ACCEPTED 成功但未来依赖凭据缺失 |
| T1 proof 与 reserve/link 之间 | c 原 reservation/budget、两边 proof、原 C/S/link | 全无或完整 RESERVED；重启不补造 dispatch |
| T2 unique claim commit 前后 | 原唯一 dispatch 与相同 proof/hash | commit 前无 callback；commit 后缺观察为 HOLD/UNKNOWN，不重复 callback |
| T3 与 T4 之间 | 原响应／nullable usage／settlement 可追溯 | 业务仍未提交；旧图/旧双方身份的 T4 拒绝仍保留原 T3 |
| ticket / round / pair / 待办之间 | 全部事实和一次 round 增量 | 全无或完整票据，重放不再次扣 round |
| child budget T1 父借记之间 | child 原 attempt/本地预算、唯一 parent debit、link/ticket | 同事务；无免费 child call、无空父假 attempt |
| child T3 父观察之间 | child 原 settlement 与对应 parent projection | 同事务；父子一边已知另一边释放失败不能默许下一次业务 |
| verdict business + feedback + ID 完成之间 | 原有效 verdict/check 投影、原反馈、精确 S 的 DONE | 回滚保留观察与 HOLD；没有反馈／选择外 ID 不得 DONE |

文件候选来自此前受控提交；graph/ticket 事务没有 filesystem publication，因此不引入
“SQLite rollback 会撤销文件效果”的假设。涉及真实工作区的纵向测试必须用原 WorkerTools
的真实文件事实和 existing owned workspace/exit/cleanup 守卫。

## 逐事实验收 oracle

每行是计划用例组，**不等于独立测试套件数量**。将来注册几个 suite 按实际 catalog 报告。
计数、hash、CAS、原库行和 callback effects 都要查，不能只断言函数返回值。

| ID | 确定性交错或输入 | 必须观察的完整事实 |
| --- | --- | --- |
| G01 | 合法 a→c←b；重复端点/边、自环、间接环、未知节点、跨请求、非法模板、team上限2 | 合法一次全量发布；每个坏输入前后原 plan/task/team/graph/edge/inbox/budget 快照相同；原错误证据不隐藏 |
| G02 | a ACCEPTED，b 未提交；再分别 b FAILED/CANCELLED/SUPERSEDED/UNKNOWN | c unique dispatch count=0、callback count=0；不同持久 reason 和原待办；UNKNOWN 不变 FAILED、不释放预算 |
| G03 | 两源真实提交和原验收后，c 连续 ready/T1/T2/replay | 原两 acceptance/map/receipt hash及验收前后CAS正确；一个原 dispatch、一个实际 callback；仅原 S 完成 |
| G04 | ready后与T2前、T2后与T4前各发生AMEND/新plan/revocation | 前者零 callback，后者一份原 response/usage 仍在但无 c 业务提交；旧 graph/proof不改字节；原新图遗漏处置仍暂停 |
| G05 | 强制恢复；旧Actor/proof、缺失／篡改recover见证；显式当前Dispatcher重捕获 | 旧会话在 replay 前拒绝；旧 proof 永不派发；仅合法连续见证可产生新 proof，原 instructions/acceptance 原字节不变；否则 HOLD |
| G06 | 普通publish/create/accept/reserve/tool路径直接访问启用图的c | 每个公开业务入口同样守卫，无 CLI-only ready 绕过；控制cancel/settlement可达且不授业务权 |
| R01 | 从真实 submission 固定候选；同内容另一submission、改path/size/hash/base64/顺序 | 映射绑定原提交和完整文件；同内容不同ID不共享ticket；损坏零 ticket/round/budget变更；不读取当前文件替代 |
| R02 | producer冒充reviewer、OneRequest越界、第三请求、不同root/project/tenant、重绑child | 全部拒绝，两个原请求和第三请求字节／预算不受副作用；真正ReviewPair仅允许既定方法 |
| R03 | producer/reviewer分别更改十域任一版本、各CAS、当前scope或取消 | 在T2和T4各拒绝；原ticket不刷新；迟到原observation/usage可保存；同候选不得复用原 pass |
| R04 | pass，逐项必需host checks缺失/fail/not_run；model自报pass | 只有独立host checks全部满足后当前Dispatcher才能原子接受；旧accept入口仍拒绝必需review |
| R05 | changes_requested、inconclusive、最后一轮、同键精确replay | 原findings留存；新task/instruction/ack；两个额度分别约束且不归零；replay零增量，耗尽明确升级 |
| B01 | 父普通reserve与child审核reserve在同剩余额度交错；不同调用同时借记 | 同一父聚合额度只有可容纳者成功；每个child attempt恰好一个debit，无parent假attempt；本地和聚合verifier一致 |
| B02 | child usage局部null、全null、input/output/cost已知但仅cache_read或cache_write为null；对照已知cache=0；零预留但unknown、超预留、迟到、结算故障；未启用新review的原Fake对照 | 新review的cache-only-null也使父子HOLD，已知0本身不触发该HOLD；三维原账本正常投影、未知三维保留原预留，不虚构cache预留或双计input/output；原Fake三维unknown门禁兼容不变；所有原usage不改写，超额全量保存、attempt计数不退还、精确重放零费用增量 |
| B03 | ticket准备后崩溃、T1未dispatch崩溃、DISPATCH后无结果、T4 commit失败 | 逐边故障表全部满足；无重复claim/callback、无无证DONE；新恢复ID不能绕开原未结算call |
| S01 | 双池普通满载，审核Dispatcher/Worker各入队，同资源FIFO | 两种原review reserve仍有效；producer等待不占活跃槽、不释放UNKNOWN／保留结果／冲突资源；无同步父等子死锁 |
| C01 | 旧411 ELF开412、412原未启用请求、真实历史库、迁移断点 | 旧程序明确拒绝；旧协议原字节保持；原子metadata/资产hash；无静默upgrade/新模式回退 |
| C02 | parent终态、child仍UNKNOWN／在途，旧清理claim、新scope重开 | 不提前交付/清理，无新业务许可；所有权、原进程exit、结算、cleanup CAS原条件仍同时满足 |

测试使用独立临时目录／DB、FakeModel、真实本地受控文件、确定性屏障。不得用 SQL 造成功
submission/ACCEPTED，只有损坏／故障注入允许直接改自己的临时库，并保留前后损坏证据。
未实现能力是 capability gap，旧版本合法拒绝不称旧安全漏洞；计划反例不伪称已经红绿运行。

## 未来实施退出

G 和 R 分别需要自己的批准、独立非作者代码复核、实际反例／新能力负例、适用完整 Release
两种 umask、ASan/LSan、UBSan、严格 Clang 与原计费／历史／recovery/workflow/cleanup 集成。
新增名称／deadline／catalog/default/CI 同步，不能把一个 suite 的方法数当 suite 数。
测试以普通用户运行，数据盘 TMPDIR；冻结最终输入、二进制和真实退出码，进程仍在运行不算通过。
再经正常分支／commit／push，核最终 SHA 的远端 CI，交协调者正式审核；不自动 merge。

本设计包的文本检查不满足这些生产退出条件。尚未批准的 D2/D4/D5 或执行相位连接会保持
对应功能 Unsupported/HOLD，不能由实现 Agent 用猜测替代架构决策。
