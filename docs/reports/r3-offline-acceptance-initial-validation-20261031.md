# A2 离线验收包：初始候选验证记录

时间范围：2026-10-31 UTC，本模块首次本地候选提交之前。原始模块基线为
`772feaee62086ddb0e7f605a7472721d3bc61406`，工作分支为
`feat/offline-acceptance-20261031`。本报告不声称最终提交、推送或远端 CI 已完成。
后续十格执行、修订和最终 SHA 的结果应追加在新的报告/发布收据中，保留本次未执行事实。

## 实际修改及范围

新增 `scripts/r3_offline_acceptance.py`，提供 prepare、dry-run、run-fake、report 和
check-real-template；新增 `scripts/offline_acceptance/v1/` 的七份版本化配置、提示词和
Fake 参数材料。它复用原 `evaluate_r3.py` 的五任务 × multi/single 两条件、原 v3 计划及
全部 provenance/accounting/logical-Agent 检查，未改该评测器或 v1/v2/v3 判据。

新增 `tests/r3_offline_acceptance_tests.py` 和 `tests/r3_offline_acceptance_cli_tests.py`。
主代理在本 worktree 增加 CMake/Make 注册：两个新测试进程的独立期限分别为 30 和 600
秒，既有测试期限未改。十个样本位属于一个 CLI 集成套件，不能登记为十个独立套件。
实现行为见 [R3-offline-acceptance-package.md](../implementation/R3-offline-acceptance-package.md)。

真实执行准备卡默认不授权、没有费用默认值，缺字段拒绝；字段齐全也只证明准备记录完整。
所有真实执行、供应商质量、实际供应商费用和独立语义验收仍是 `NOT_RUN`。没有真实模型、
付费 API、生产服务、真实凭据或生产数据被使用。

本模块新增入口此前不存在；不能将“旧版无此入口”冒称已复现的运行时故障。本轮准备了
在真实临时 Store 样本的独立副本上破坏来源/账目/身份的负例，**尚未执行**。原 R2.4 和
旧评测器的已有反例保持其原报告时点，不作为本模块十格已通过的证据。

## 环境与已执行证据

工作区位于 `/mnt/d/codex-workspace/wls861-next-20261031-065247/offline`。执行用户
`ubuntu` 为 UID/GID 1000，数据盘 ext4 在接手时剩余约 50 GiB。源码、证据、准备包和
临时目录均在数据盘；本模块使用的测试 `TMPDIR` 为本任务 `evidence/a2/tmp`，Python
为本机 `python3`。执行期另一 worktree 的正式 GCC ASan 全轮正在运行，主代理要求不
竞争重测试资源，因此本候选只执行源码检查和下列轻量命令，没有编译 C++ 或运行十格。

原 argv、stdout/stderr、退出码和耗时保存在任务目录的 `evidence/a2/`，未作为构建产物
或临时数据库提交。实际执行记录：

| 命令/证据 | 实际结果 |
| --- | --- |
| `python3 tests/r3_offline_acceptance_tests.py <source>`，contract-01 | 8 cases，exit 0；初版轻量输入 |
| `python3 -B tests/r3_offline_acceptance_tests.py <source>`，contract-02 | 8 cases，exit 0；加入准备卡类型/URL/凭据引用与材料篡改检查 |
| `python3 -B scripts/r3_offline_acceptance.py prepare --package <evidence/a2/prepared-01>`，entrypoint-01 | exit 0；实际写出固定材料、配置、清单和人审表 |
| 同一入口 `dry-run --package <prepared-01>`，entrypoint-02 | exit 0；保留十格 not_executed，没有 run 目录 |
| 同一入口 `check-real-template --card <prepared-01/real-execution-template.json>`，entrypoint-03 | exit 2，符合预期：必填前提缺失，仍不授权 |
| 三个新增 Python 文件 AST parse | 通过；不生成源码目录 pycache |
| 非作者独立运行纯 Python 合约套件 | 8 cases，exit 0；输入前后摘要一致，见下述审核收据 |

轻量测试实际拦截了 SQLite connect、socket、subprocess、环境取值和写模式 open，并
比较干跑前后全部包字节；零 Store/凭据/网络/发送权限不是只检查报告字符串。测试也验证
改写材料和重哈希非 Fake 配置仍拒绝、符号链接/FIFO/重复 JSON 拒绝、现有目录不覆盖、
两条件拓扑与私有文件模式，以及完整准备卡仍不获得真实授权。

这些执行没有声称完成两种 umask 的完整产品测试；完整 Release、ASan/LSan、UBSan、
Clang、原计费 E2E 和历史兼容矩阵均仍待本批统一验证。其他 worktree 正在执行或已通过
的测试不能转换成本候选的结果。

## 非作者审核与源码身份

`/root/independent_recovery_review` 完成有界源码审核，并独立运行上述轻量套件。
外部收据为 `evidence/a2-independent-review.json`，原日志为
`evidence/a2-independent-review-contract.log`。其范围包含入口、全部版本化材料、两个
测试、实现契约和原评测器；读取当前源码而非仅按说明审查，未发现已确认的阻断问题。
这是同模型代理的非作者工程复核，不是第二模型质量评测或主协调者批准。

源码检查期间发现 Fake AMEND 脚本需要服从真实调度优先级：遗漏 beta 处置仍待处理时，
不能提前让 Dispatcher 脚本返回 acceptance。当前候选把 initial/AMEND/update/keep/
acceptances 分为明确批次。该调整已包含于审核输入和独立轻量运行；实际 Store 十格
尚未执行，因此这里不称其动态问题已经修复通过。

审核记录中的关键 SHA-256：

| 实际输入 | SHA-256 |
| --- | --- |
| `scripts/r3_offline_acceptance.py` | `d76e33bb48d894249b3218d469ea93a5affc6231ed12359f5164178d0e6357b9` |
| `tests/r3_offline_acceptance_tests.py` | `175bf67b5529392e4b4e3e29c7b29b209690be09fe76e3e0fb060df169f0873c` |
| `tests/r3_offline_acceptance_cli_tests.py` | `e63a9526e209c5b97d76e623dc5f70c60735b1214cb2ba038981a6e152345a92` |
| 未改动的 `scripts/evaluate_r3.py` | `89bc317e52f90471d07e2c6ec6f84bf3c55e11a9f87ec177812c7dd6efd76a47` |

本候选完整源码输入摘要另存 `evidence/a2/source-inputs-candidate-01.json`；它记录所有
待提交受控文件的原字节，而不是用本报告推导二进制身份。本时点没有本模块执行过的
C++ 二进制，故无可声称已运行的二进制 SHA。原 SQL、third_party、docs/handoff、
已发布蓝图及历史 reports 没有修改。

## 未完成工作与真实状态

十格 CLI 与其来源、账目、身份负例需要在主代理释放资源后，用已经核验身份的普通用户
Release 二进制真实运行；失败原包必须保留。然后执行适用完整矩阵和最终快照/二进制核验。
公共 GCC 验证工具合入后，新增的两项必须进入冻结测试清单，注册总数从 89 增至 91；不能
仅改数量而漏掉名称和原期限。推送、远端 CI、正式评审及是否合并由主代理继续处理。

| 状态 | 本报告时点 |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | false；可审阅候选已编写，真实 CLI 验证与必要修订仍待执行 |
| `LOCAL_VALIDATION_COMPLETE` | false；仅轻量八例及准备入口已执行 |
| `PUSH_COMPLETE` | false；本工作代理仅准备本地候选提交，不推送 |
| `REMOTE_CI_COMPLETE` | false；本候选无实际远端结果 |
| `PR_REVIEW_COMPLETE` | pending 主协调者对确切 SHA 的决定 |
| `MERGED` | false |
