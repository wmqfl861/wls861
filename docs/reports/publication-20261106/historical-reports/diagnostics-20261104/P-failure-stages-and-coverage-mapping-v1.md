> **HISTORICAL PROJECTION (REDACTED) — NOT CURRENT ACCEPTANCE.**
> Source: `P/P-failure-stages-and-coverage-mapping-v1.md` as it existed uncommitted in its
> worktree at head `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` (branch `fix/n5-diagnostics-20261104-_7wxnkyn`); original 13756 bytes,
> SHA-256 `1cab85eac225df769a0bfa87accc6ccc3b4644d5e58610b70b3006dea58fb89d`. Redacted public projection created 2026-11-06T10:21:09Z under
> `docs/reports/publication-20261106/`. This is a new version, not a byte copy
> of the original; the original is retained locally and is not in Git.
> Redaction scope: absolute local workspace/evidence-root paths, local task plan / version-record / plan-review references, internal session identifiers.
> No engineering result, status, failure, timestamp or six-state value was
> changed; PUSH/CI states remain the historical values of the report's date.
> **Adjacent qualification:** the root-cause attributions in this P report and
in the S-stage receipts are historical hypotheses; the current root causes of
the open sanitizer failures remain OPEN and are not closed by this backfill.

# P 流首批：当前失败阶段表与 T0–T4/生命周期覆盖映射 v1

- 日期：2026-11-04。作者：该轮实施代理会话（内部计划引用已去除；绑定 diagnostics worktree @ `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`）。
- 性质：只读源码核查 + 对原 ASan 终态（`8f755f1925b095068ae1015b8f828958c86ed2830abe5da73438004cb8e7badd`，95=64P/2F/29T，exit8）的映射。未修改任何产品源码、测试或构建；不构成独立审核或正式验收。
- 旧报告口径提醒：`docs/reports/n5-1-persistent-boundaries-20261103.md` 的 Store25→30/runtime13→14 是 **c2 输入**的记录；当前 3a 输入下 runtime 为 **15 方法**（新增两个 stop-gate 方法），store 保持 30。不得挪用方法数作注册数（CTest 注册各为 1）。

## 1. 当前共享 ASan 终态中的相关套件（原始事实）

| 套件 | 当前 ASan 终态 | 时间 | 说明 |
|---|---|---|---|
| persistent_event_runtime | **FAIL**（12 方法 PASS / 3 FAIL） | 82.30s | 三处 `until()` 5s 等待超时（测试源 301/344/668 行；5s deadline 定义在 198 行） |
| persistent_event_store | **TIMEOUT**（注册期限 120s 到期，无逐方法终态） | 120.39s | 属于 29 个 TIMEOUT 之一；30 方法在 Release 0022/0002、Clang strict（95/95）下为 PASS |
| live_model_store_tls | **FAIL**（1 ERROR + 1 FAIL） | 91.22s | `effective_live_capacity` driver 18s 子进程期限超时；`large_escaped` accepted=0（期望 1） |
| store / live_model_store | PASS | 33.30s / 67.48s | 对照 |

两个新增 stop-gate 方法（`stop_between_false_predicate_and_wait...` 554 行、`destructor_joins_callback...` 576 行）在当前 ASan 原跑中 **PASS**；不预设三处失败仍是同一 lost-wakeup 缺陷。

## 2. 三处 5s 失败的最小阶段表

每次模型调用的完整链（owner 线程 = 构造 PersistentEventRuntime 的主机线程）：

```
runtime.poll()【owner 线程】
 └ coordinator.poll()
    ├ 控制命令（amend/cancel → Store 事务，跨请求 stop 传播）
    ├ 就绪 job → finish：settle_event_observation(T3) + commit_event_action_and_finish(T4)【Store 事务内】
    ├ AwaitingClaim job → claim_event_dispatch(T2 唯一 dispatch)【Store 事务】→ 许可回调线程
    └ 新 job 入队（Scheduler 公平池）
PersistentEventRuntime::advance()【owner 线程】
 └ invoke() → OfflineRuntime::capture_call（捕获原始调用）→ submit_event
    ├ T0 claim_event_batch（claim C，含 ID 快照）
    ├ T1 reserve_event_call（reservation+link+selected/open items，同一事务）
    └ 资源前驱排队 → Scheduler 线程 execute()：等许可 → T2 已在 owner poll 中完成 → ModelAdapter::complete()【回调线程，Store 事务之外】
```

| 失败点 | 测试/等待谓词 | 到达该状态必须已完成的阶段 | 5s 内未完成时的候选卡点 |
|---|---|---|---|
| :301 `two_requests_two_nodes...` | `gate->entered==1`（请求一 TeamLead 回调进入 GatedModel 门）**且** 3 个 note op SUCCEEDED（T4 CLOSE 落库） | 请求一：capture→T0→T1→T2→（回调线程抵达 gate.wait）；三份 note 需完整 T3+T4 | 双请求四调用并发推进下，任一环节（T0/T1/T2 许可顺序、资源 FIFO、T3/T4 事务）在 ASan Debug 下 5s 内未走完 |
| :344 `feedback_created_after_capture...` | `gate->entered==1`（Worker role_call_index==2 回调进入门） | 该调用 capture→T0→T1→T2（此时 links[2] CLOSED、links[3] DISPATCHED 为检查前提） | 同上单链卡点；注意 :346 检查依赖 links 已推进 |
| :668 `empty_selection_releases_only...` | 1 个 note op SUCCEEDED 且 `outstanding==0`（无门控，纯推进） | 首批 add_request 的 alpha 链完整 T0→T4 后静默 | 无门控下的纯 T0→T4 链在 5s 内未完成一轮（含 poll/yield 步进开销） |

共同结构：`until()` 以 `poll()+yield()` 自旋、墙钟 5s；等待的是**完整跨线程链路收敛**，不是单个 Store 调用。原 stop-gate 方法已证明 stop→wait 唤醒路径本身正确；本轮三处失败与 S2 观测到的「ASan 下夹具侧固定 5s 期限被 CPU 时间吃掉」方向一致，但 runtime 侧根因仍 OPEN（见 S3 探针收据）。

## 3. 21 个定义场景 × T0–T4 映射（以当前源 `persistent_event_store_crash.inc` 为准）

场景清单（当前源 453–484、486–571 行；**不是连续 W00–W19，无 W17/W18**）：
W00, W01, W02, W03, W04, W05, W06, W07, W08, W09, W10, W11_prepare, W11_start, W12, W13, W14_first_id, W14_close, W15, W16_amend, W16_cancel, W19 —— 共 **21** 个定义场景。

durable_stage 语义（crash.inc 的阶段参数）：0=T0 claim 前；1=T0 后/T1 中；2=T1 后/T2 中；3=T2 后（回调/观察/T3 中）；4=T3 后（T4 各插入点/观察后控制）；5=T4 CLOSE 后。

| 场景 | 注入点（触发器/barrier） | 对应事务阶段 | 覆盖状态 |
|---|---|---|---|
| W00 | claim 前进程 barrier（stage 0） | T0 前 | 已有执行覆盖（Release/Clang 95/95 中执行并通过；共享 ASan 下该注册 TIMEOUT，无逐方法终态） |
| W01 | BEFORE INSERT ON n5_event_claims（stage 0） | T0 中 | 同上 |
| W02 | claim 落库后 barrier（stage 1） | T0 后/T1 前 | 同上 |
| W03 | BEFORE INSERT ON n5_event_links（stage 1） | T1 中 | 同上 |
| W04 | link 落库后 barrier（stage 2） | T1 后/T2 前 | 同上 |
| W05 | BEFORE INSERT ON n5_event_receipts kind='DISPATCH'（stage 2） | T2 中 | 同上 |
| W06 | dispatch 提交后 barrier（stage 3） | T2 后/回调前 | 同上 |
| W07 | ModelAdapter::complete 入口计数 barrier（stage 3，callbacks=1） | 回调内 | 同上 |
| W08 | 响应产生后 barrier（stage 3） | 回调后/T3 前 | 同上 |
| W09 | BEFORE INSERT kind='SETTLEMENT'（stage 3） | T3 中 | 同上 |
| W10 | settlement 提交后 barrier（stage 4） | T3 后/T4 前 | 同上 |
| W11_prepare | AFTER INSERT ON events type='prepare_op'（stage 4） | T4 中 | 同上 |
| W11_start | AFTER INSERT type='start_op'（stage 4） | T4 中 | 同上 |
| W12 | BEFORE INSERT ON model_tool_feedback（stage 4） | T4 中 | 同上 |
| W13 | AFTER INSERT ON model_tool_feedback（stage 4） | T4 中 | 同上 |
| W14_first_id | AFTER UPDATE event_inbox 首个 DONE（stage 4） | T4 逐 ID | 同上 |
| W14_close | BEFORE INSERT kind='CLOSE'（stage 4） | T4 尾 | 同上 |
| W15 | CLOSE 提交后 barrier（stage 5） | T4 后 | 同上 |
| W16_amend | 观察后 amend 提交 barrier（stage 4+控制） | T3 后控制/T4 前 | 已有执行覆盖（同上口径）；**共享 ASan 下属 3 FAIL 之一（见 S2：夹具 5s 管道期限）** |
| W16_cancel | 观察后 cancel 提交 barrier（同上） | 同上 | 同上 |
| W19 | 410 旧路径 feedback INSERT 前真实 _exit（独立函数） | 旧同步路径 settlement 后/feedback 前 + 410→411 升级 | 已有执行覆盖（同上口径）；**共享 ASan 下属 3 FAIL 之一（同 S2 机制）** |

注：W16_amend/W16_cancel 由循环构造（480–483 行）；定义数不等于本次执行数或 21 份新 SIGKILL 收据。crash 每例都是「已知 PID SIGKILL + waitpid 回收 + WTERMSIG 校验」，回收证明在测试内。

非 crash 的 T0–T4/生命周期覆盖（persistent_event_store_tests.cpp，30 方法）同样为已有执行覆盖（Release/Clarch 95/95 通过；ASan 注册 TIMEOUT）：T1/T2/T3 注入回滚（`t1_t2_t3_failure_rolls_back_both_sides`）、T4 三类业务 helper × feedback/CLOSE 故障全表快照回滚（`t4_all_business_helpers...`，每表每列单读快照哈希）、commit outcome unknown T1–T4 四档（`committed_wal_error...`）、重开恢复不重发不重放（`recovery_never_reissues...`）、部分 ID/迟到响应（`closed_partial_id_replay...`）、容量/尾部/元数据边界（`lifetime_limits...`、`exact_metadata...`）、损坏事实 fail-closed（`corrupt_open_projection...`、`corrupt_link_fact...`）、410 升级维护门（`structural_410_upgrade...`）。

## 4. 生命周期专项核查（按计划第 4.3 项逐条）

1. **commit outcome unknown 的真实执行时点**：CommitOutcomeFault 以 `AFTER INSERT ON n5_event_receipts WHEN NEW.kind=…` 触发器在**目标事务内的实际写连接**上安装 WAL hook（faults.hpp:44–50 注释：sqlite3_open 的自动 checkpoint 挂钩可能在连接初始化时替换扩展 hook，故必须在事务内安装）；hook 在事务 **COMMIT 已持久化后**回调并返回 SQLITE_IOERR_WRITE，测试要求 `installed()==1 && hits()==1 && committed_pages()>0`（store_tests:761）并独立只读核对持久结果。
2. **WAL hook 替代自动 checkpoint 的限制**：在连接上安装 WAL hook 会替换该连接默认自动 checkpoint hook——夹具不建立「checkpoint 时序不变」；journal/synchronous 模式与生产 Store 代码不变（boundaries 报告已声明，当前源一致）。这是夹具边界，不是产品断言。
3. **rollback 全表快照**：`crash_database_hash`（crash.inc:45–59）以只读连接在单事务内对每表每行（排序行哈希）做全库哈希；T4 helper 回滚用它做前后全等（store_tests:709–713），crash 各阶段用它做 checkpoint 事实与恢复后不变式。
4. **跨重开 stale Actor/session/claim**：crash 每例重开后旧 actor 的 `claim_event_batch`/`release_event_batch`/`claim_event_dispatch`/`commit_event_action_and_finish` 全部 `Stale`（crash.inc:392–399）；`association_mutations_and_exact_replay_do_not_spend`（store_tests:284）注入 `execution_epoch+1` 与 `writer_session="foreign-session"` 变体；runtime 侧 `reserved_and_dispatched_restarts_keep_original_actor_hold`（runtime_tests:461–498）验证 RESERVED/DISPATCHED 重开后 HOLD_UNKNOWN、原 envelope 逐字节保留、claim 数不变。
5. **原 nullable usage/预算**：usage 置空 → HOLD_UNKNOWN + unknown_input/output/cost=1 + 预算 reserved→unknown 转换（store_tests:320–331；runtime_tests:413–434 input_tokens.reset()）；绝不以 0 替代 unknown。
6. **partial-ID**：`closed_partial_id_replay...`（store_tests:818–863）— CLOSED 后被省略 ID 回到 PENDING、新 claim 可取、精确重放返回原 dispositions、迟到不同响应 `Conflict`、全表与兄弟请求不变；crash W14_first_id 为 T4 中首个 DONE 的崩溃位。
7. **late/AMEND/cancel**：runtime cancel 保留迟到原观察（runtime_tests:436–459）；store `control_after_settlement_invalidates_business_but_keeps_original_response`；W16_amend/cancel 观察后控制崩溃位；部分 AMEND 逐 ID DEFERRED_NO_EFFECT（runtime_tests:367–410、store `partial_amendment_defers_each_omitted_subject`、`nonpending_amendment_selection_is_rejected_before_reservation`、`amendment_task_cas_is_checked_before_reservation_and_dispatch`）。
8. **stop→join→Store lease 释放**：`close_joins_callback_and_stores_only_original_observation`（runtime_tests:531–552）与 `destructor_joins_callback_before_releasing_the_store_writer_lease`（576–633）：回调存活期间对 `control.sqlite.writer-lock` 的 `flock(LOCK_EX|LOCK_NB)` 必须 EWOULDBLOCK——**在实际排它写租约上**证明 join 先于 Store 释放；析构后重开 SETTLED/未知观察保持、旈权限 Stale。
9. **进程内 started/halted 不是跨重启权限**：`event_application_halted` 在 T0/T2/T4 入口围栏新业务（association.inc:516/598/647），`event_application_started.insert(id).second` 防同进程重放应用（648），成功 CLOSE 后 erase（785）；重启路径只依赖持久 receipts/open projection/claims（W 系列、recovery、HOLD_UNRESOLVED_ORIGINAL_CALL 均验证），无任何进程内标志跨重启读取。

## 5. 覆盖差距与可证最小实验（交付给计划作者；未实施）

| # | 差距/提议 | 状态标注 | 说明 |
|---|---|---|---|
| G1 | 当前共享 ASan 下 persistent_event_store 注册 120s 整体 TIMEOUT：30 方法在共享配置中无逐方法终态（Release/Clang 有） | 观测事实，非新测试 | 最小实验：同 S3 fixture 方式对该注册做一次隔离单套件探针（新 fixture+私有 TMPDIR+前后清单），判据=是否在 180s 注册期内完成并给出逐方法终态。属 S 类探针，需作者定稿后执行 |
| G2 | runtime 三处 5s 等待在共享 ASan 失败：已有 S3 隔离探针证据（见 S3 收据）；若隔离下仍失败且采样显示忙等，下一项最小插桩提案=在 until() 循环记录 poll() 推进计数/阶段时间戳（仅测试插桩，改 persistent_event_runtime_tests.cpp 前须作者定稿） | COVERAGE_ONLY/诊断 | 不改期限、不加 sleep、不重复到绿 |
| G3 | 夹具 crash 管道 5s 期限 vs ASan 下 checkpoint 全表哈希耗时（S2 已证 W11_prepare checkpoint 5.68s） | 诊断事实（S2 收据） | 生产代码无缺陷证据；如需修复属测试夹具调参（如 barrier 前 checkpoint 的预算或比较方式），必须返回作者定稿；不得顺手改 5s CHECK |
| G4 | commit-outcome 夹具不建立 checkpoint 时序不变（WAL hook 替代自动 checkpoint） | 已声明的夹具边界 | 无需新守卫；若要覆盖「checkpoint 时序」需独立设计，超出本批 |
| G5 | TLS 两个失败（effective_live_capacity 18s driver 期限、large_escaped accepted=0）：S3 隔离探针已取一次原输入结果；若隔离下仍失败，最小实验=driver 单例 + 阶段采样区分「容量拒绝语义正确但慢」vs「期限内计算未完成」 | 诊断 | 见 S3 收据；不调用外部供应商，loopback/合成凭据 |

未发现需要立即以旧输入红例修复的生产缺陷；本批无生产源码修改授权，也未做任何修改。
