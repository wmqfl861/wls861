# N5 存储方案：崩溃窗口与可执行验收规格

状态 **PLANNED / NOT_RUN**；此文件是设计辅助，不是已运行测试、生产闭环或已复现缺陷。
权威设计见 [事务契约](storage-contract.md)。本阶段未运行产品、编译、SQL 迁移或测试。
下表描述推荐 B 的预期；若选 A+411，须用同名 typed 事件证明同样状态，不能减少 oracle。

## 测试宿主与统一 oracle

未来 harness 使用独立私有临时目录／真实 SQLite Store、原编译注册资源和 FakeModel。
仅宿主受控 fault hook 可以在指定事务步骤停住、返回 storage error 或终止子进程；用双向 barrier
观察阶段，不用 sleep 猜时间。原例外、权限、deadline 和 sanitizer 不放宽；不手写 accepted、
settlement、feedback 或业务成功行来使流程通过。开库前保存源 SHA／资源 hash，测试结束核身份。

每例先从 API 得到原 request/actor/claim/C/S/call/attempt/target；只读核对至少包括：

- 原 `event_actor_claims` 与不可变 `n5_event_claims`、原 inbox 每个 ID 的 scope/status/claim。
- `call_attempts` 完整原 envelope/binding、reservation/settlement hash、nullable usage、预算五维
  或原 Fake 三维投影；`offline-dispatch:<attempt>` 精确条数。
- 新 link/items/receipts/open projection 的原字节和 hash；逐 ID DONE/INVALIDATED/DEFERRED/HOLD
  的理由，原目标和业务对象版本。对账只读，不能刷新 captured tuple。
- note 的原 `ops/operation_intents`、所有 prepare/start/finish 原 command/event、原 feedback；
  callback 计数通过父进程 barrier receipt 单独记录，不能用一份临时 future 推断重启后次数。
- 每次重放／恢复前后对原字节取 hash，验证无覆盖、无新增业务／callback；新 recovery 控制事实
  单列。UNKNOWN、缺原件和故障本身保留，不算测试框架失败后直接跳过。

崩溃后先用新版本 ReadOnly 观察已提交状态，再按明确 request scope 打开执行 Store；恢复事务本身
可能推进 epoch、把原 PENDING attempt 置 UNKNOWN 并释放 claim。报告必须区分“崩溃提交状态”与
“接管后状态”，不能把后者当成原来没有派发／没有业务效果。旧 Actor 和 claim 的负例必须实际调用
并证明拒绝发生在任何新 effect 前。

## 每一个原子边界的崩溃判定

`L` 表示原 link；`E` 是原业务效果数，`F` 是原 feedback 数；`D` 是完成 IDs。
原状态推进事件和随后只读观察不是新增模型调用。恢复不保证自动继续，只保证不重新派发／不猜成功。

| 窗口 / hook | 崩溃后可证明的持久状态 | 必须拒绝／保留的行为 |
| --- | --- | --- |
| W00：结果槽／脚本预检前，T0 前 | 无新 claim snapshot、attempt、L 或预算递增 | 不可消耗事件或预支模型次数 |
| W01：T0 claim 更新后、snapshot 写入前（事务内） | T0 整体回滚；原 inbox/claim 与基线一致 | 不能只剩 CLAIMED 而无 snapshot |
| W02：T0 commit 后、T1 前 | snapshot 保留 C；无 attempt/L；接管释放旧 claim | 旧 token 拒绝；可新捕获尚未关联工作，但不复活旧 Actor |
| W03：T1 attempt／预算写入后、link 或 projection 前 | T1 整体回滚；attempt/L/预算均无新增 | 不能留下孤立预约或空 link |
| W04：T1 commit 后、T2 前 | 原预约和 L/S 及 open projection 全在；无 DISPATCH | 不续旧 Actor／旧 attempt；保守 HOLD，不把无 dispatch 当免费重试许可 |
| W05：T2 原唯一 event 写后、receipt 或 commit 前 | T2 整体回滚，原 offline-dispatch 条数为 0 | callback 从未获准；仍按原预约未决处理 |
| W06：T2 commit 后、callback 获知许可前 | 唯一 dispatch event 与 receipt 同时在，计数 1；可能从未进入 adapter | 与响应丢失不可区分，绝不第二次 dispatch；缺响应 HOLD |
| W07：callback 已进入、响应尚未返回 | 同 W06；原 reservation 保持，接管按原规则 UNKNOWN | 不以取消／超时猜零 usage，不重跑 adapter |
| W08：原响应已到内存、T3 前 | durable 响应可能缺失；L 与一次 dispatch 仍在 | 活宿主 Retained 原件／槽；进程崩溃则缺响应 HOLD，不能重建模型文本 |
| W09：T3 settlement 写后、SETTLEMENT receipt 前 | 同事务回滚，两者均未新增；原预约仍在 | 不出现 settlement 与关联脱节；原响应在活宿主保留 |
| W10：T3 commit 后、T4 前 | 原 settlement/response/usage 与 receipt 完整，E=0、F=0、D=空 | accepted=1 也不授权旧响应重应用；保持未完成和收尾围栏 |
| W11：T4 note prepare 或 start 后（事务内） | 整个 T4 回滚：无新 op/intent/事件／工具计数；settlement 不回写 | 不能留下新 PREPARED/RUNNING 半写，也不能重调 callback |
| W12：T4 最终业务结果后、feedback 前 | 整个 T4 回滚：E=0、F=0、D=空；原 source bytes 不变 | 验证该新模式没有可持久观察的“E=1/F=0”窗口 |
| W13：T4 feedback 后、逐 ID proof 前 | T4 整体回滚，E=0、F=0；无半个 CLOSED | 不把 feedback 到达当 ID 已完成 |
| W14：proof 后、finish 更新中／commit 前 | T4 整体回滚，全部 C 的原 claim 状态未部分推进 | 不允许 A DONE、B/投影还留在未提交的一半状态 |
| W15：T4 commit 后、future 通知前 | E=1、F=1；完整原 CLOSE 与 D；C\D 已释放／围栏；原 claim CLOSED | 重放只返回原 receipt，E/dispatch/预算不增加；后到 C_new 完全不变 |
| W16：AMEND/cancel 在 T3 后、T4 前提交 | 原控制事实和原 settlement 都保留，T4 原 binding/CAS 失效 | 新效果 E=0；不得因旧 accepted=1 绕版本；旧响应不能盖新 tuple |
| W17：owner close 或异线程析构与 callback 返回交错 | 原 join receipt 与实际原响应／缺失分别记录；Store 存活到 callback 退出 | callback 不访问 Store；异线程不持久化；缺原件保持 HOLD，无伪造退出证明 |
| W18：反馈／proof 存储返回错误但宿主尚活 | T4 回滚，原响应与槽 retained；settlement_halted=true | flush 仅保存原观察，E 仍 0，不自动再次跑业务，不释放资源后继绕围栏 |
| W19：旧 legacy 路径已有 E=1/F=0 后升级 | 无新 L／CLOSE，原 op/event 可见但历史关联未知 | `UNLINKED_HISTORY/HOLD_MISSING_FEEDBACK`；迁移不补造 receipt、不把猜测 note 标 DONE |

W10/W18 的显式收尾可以保存原错误／证据、释放与该失败明确无效果的未选项；它不获得业务重应用
许可。需要继续业务时先由协调者批准一个独立的原未决处置／新调用协议；初版不得凭“这次看起来没有
效果”自动新建 attempt。对于在 T4 原子事务中明确未处理的 partial AMEND B，则已有原不可变
DEFERRED_NO_EFFECT 证明，下一次针对 B 的新捕获和正常预算不是旧 attempt 重派。

## 正反例与固定断言

下列 ID 是设计用例，不是新增 CTest 注册数。拟加入原 `event_store/event_runtime/async_coordinator`
等受影响套件，保持原注册目录和 deadline；新增注册如确有必要须协调者统一维护清单。

| ID / 拟议用例 | 固定构造 | 必须断言 |
| --- | --- | --- |
| E01 two_requests_two_nodes | R1/R2 各 a/b，真实 plan/instruction/ack/note；两 FakeModel callback 同时等待，释放其中一个 | 等待确有重叠，第三个独立可运行项提交；所有原 L/attempt/feedback/ops 和逐 ID 证明完整，无跨请求完成 |
| E02 partial_and_late_ids | claim C={A,B}，目标只处理 A；T1 后生产同 actor 新 ID C_new | A DONE，B PENDING，C_new 原状态不变；空集合只释放原 C；外来／重复／另一 request ID 拒绝 |
| E03 partial_amendment | 捕获 A/B；原模型只显式决定 A，下一次原 same-plan 决定 B | A 完成、B 原 task/CAS 未变且 DEFERRED；B 新调用正常计费；不重发 A 的说明／stop，不补默认 keep |
| E04 wrong_binding_each_field | 从一个合法 L 出发，逐一改变 principal/role/task/team/claim/gen/C/S/source/target/每个 revision/epoch/envelope/attempt | 每个单因素拒绝，预算／effect／完成 IDs 零变化；不能仅测 request_id |
| E05 old_owner_and_old_claim | 保留原 Actor/token，重开同请求 Store，签发新 owner，接收旧结果 | 旧 session/token 实际拒绝；新 owner 只能核原 receipt／收尾，原绑定从未更新；callback=1 |
| E06 exact_control_replay | queued 和 inflight 中先 AMEND/cancel，再用原 command/Actor 重放；另有新绑定调用 | 原重放没有新 stop；真实新转换仅废止原旧绑定；后继 callback/业务不被历史命令误停 |
| E07 wait_budget_script | A WAITING，B 可运行；缺脚本、attempt/上下文/关联容量分别触顶 | A 不准入；B 推进；失败项无新预约或假退款；问题／纠正 ID 仍可见，控制仍可达 |
| E08 double_id_owner | 两个不同 L 争同一 inbox ID；同一 L exact replay；改 S 或目标 hash 的 replay | 第二未决关联唯一约束拒绝；精确 replay 不重复预算／receipt；改意图 Conflict |
| E09 each_crash_window | 每次独立 Store，仅注入 W00–W19 中一个窗口；保存父 barrier 与子 exit | 对应行 oracle、所有原 hash、dispatch/effect/feedback/ID 计数；不通过循环重跑获取绿色 |
| E10 capacity_tail | 原 business/control 各离上限 0/1/所需尾部；关联 bytes/links/claims 达界；64 result 普通满载 | 在 effect 前原子拒绝或有足够原尾部；两种 Review reserve 和独立 controls 不被借用；checkpoint 不重置终身关联数 |
| E11 retained_resource_fifo | T4 注入 feedback/proof storage failure，另一池同资源后继与无关兄弟并列 | Retained 持槽/资源，整个 coordinator 新业务围栏；控制可达，flush 不应用业务，join 后才释放 Store |
| E12 terminal_and_legacy | DELIVERED/CANCELLED/旧 410／旧快照与 enabled 新请求混合 | 新模式不恢复业务；原合法 source-bound experience 独立；旧 API/default 各按原契约，legacy pump 拒绝 enabled 请求 |
| E13 migration_failure_matrix | 402–410 每个已支持前缀、410 含历史／PENDING/UNKNOWN/旧 claim；013 缺资产／错 hash／半步故障 | 原 SQL/字节不变；只读不升级；带数据须 all-request；迁移零半写，历史不合成 link |
| E14 old_binary_and_rollback | 对 411 用冻结 aa2 程序打开；恢复升级前一致性快照；另有已发生新业务样本 | 旧程序 Unsupported 且业务字节不变；无新业务时快照恢复可核；有新业务禁止静默回旧快照／改 user_version |

E01/E02/E03 要读取每一个具体 inbox ID，不能只比较 pending 总数。E04 的完整字段向量应由
[事务契约](storage-contract.md) 的身份表生成，另加入同文不同 source/hash 和同 hash 不同对象版本。
现有正确守卫只记 baseline guard，不伪造旧代码生产缺陷红例。

## 在获得实施与执行放行之后

1. 先形成获采纳的 ADR／存储批准记录和精确实现 base；再实现 013/内部事务步骤及新模式 guard。
2. 普通用户下先执行 E01–E14 的适用原测试入口；每个真实缺陷在精确旧基线建立确定红例，已有正确
   守卫只记录覆盖增量。记录命令、exit、原完整日志、源码和 ELF 身份；故障与测试失败不覆盖。
3. 在同一最终输入完成适用 Release 两 umask、Clang strict、UBSan、共享 GCC ASan/LSan、原 Make。
   注册数、内部方法数、设计辅助检查分别计数。本地原失败和未执行项保留，不由 CI 代填。
4. 非作者核对 Store/旧身份/容量/迁移/故障原件，再由协调者决定是否进入后继 N5-2。

本文件没有可声称成功的当前产品命令，因为这些接口和迁移尚未实现；它给出未来真实 harness 的
构造、hook、查询和精确反面断言。源码读／文档一致性自查不能填写 E01–E14 或 W00–W19 的 PASS。
