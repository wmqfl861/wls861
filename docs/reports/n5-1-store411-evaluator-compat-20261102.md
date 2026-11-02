# Store 411 离线评测兼容修复：定向验证记录

时间范围：2026-11-02，本分支提交前。执行者 `/root/concurrency_fix`，任务由 `/root`
从 N5-1 首轮实际 CI 失败中分离；本报告不替代协调者保存的原 CI 日志或最终集成门禁。

## 基线与最小修改

源码基线 `28cf4b58d790984e691b59fb41959a1631173712`；独立分支
`fix/n5-1-store411-evaluator-compat-20261102-csj777e9`，数据盘工作树
`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/compat-411`。

Store 已通过新增 013 注册为 411，但 scripts/evaluate_r3.py 的只读 database() 仍只接受
408/409/410，导致真实新库在读取原业务证据之前被拒绝。产品改动只有该 allowlist 增加
明确已知的 411；未知 412 继续拒绝。原 regular-path、512 MiB、mode=ro、query_only、
单读事务、SQLite 长度／执行期限、quick_check，以及所有原请求／配置／计费／来源／
v1/v2/v3 业务核验均保留。没有迁移、修改数据库或放宽 UNKNOWN／model 来源判断。

四处当前 CLI 输出断言由精确 410 更新为**精确 411**：

- tests/s12_cli_smoke.py：新库 init diagnostics。
- tests/s13_cli_smoke.py：原多任务文件／反馈链的 PRAGMA user_version。
- tests/s15_cli_smoke.py：原业务验收／交付链的 PRAGMA user_version。
- tests/test_s1_allocation_recovery_cli.py：原 owned allocation／只读查询的版本。

没有把 assertEqual 改为允许集合，也没有改变旧版输入／输出含义、时间上限、业务断言或测试注册。
tests/r3_evaluation_tests.py 的原 Evaluation suite 增加三个方法；这仍是**一个既有 suite**。

## 真实输入与二进制身份

本机普通用户 UID 1000；Python 3.12.3，SQLite 3.45.1，Linux 6.8.0-147-generic x86_64。
临时目录固定到数据盘 `.../tmp/compat-411`，继承原 fixture 的 umask 0077；本次没有重跑
两种完整 umask。全部新方法仅调用真实 CLI 的本地准入，模型调用数为 0，无 paid/provider。

使用协调者已经编译的真实 411 CLI：

```text
/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/integrated/build-targeted-release-01/wls
SHA-256 2a673b74345e7ab82f69275050d8656d05d610c53a02b5bd37f4b743faacfc37
协调者记录的编译源码 f0208f9d8bdb13decd02f162f99d356936319440
```

它是已集成运行时的二进制，**不是本分支 28cf 的精确重编译产物**。本子 Agent 没有编译。
执行前后核同一个 ELF SHA；其 f020 源码与 28cf 的 db、schemas、prompts、runtime manifest、
config/examples 字节差异为空。执行的 `--source-root` 为本兼容工作树；Python 前后输入另冻结。
协调者随后只有测试源码修正的事实不被当作这个 ELF 的新身份。

## 新回归及旧版反例

| 方法 | 真实条件和核验 |
| --- | --- |
| test_current_store_411_keeps_original_evidence_and_readonly_guards | 真实CLI创建411；先核metadata13及原013 SHA、N5表存在且未启用。评测产生原v1/v2计划并保持10个未执行样本；query_only阻止写入，关闭连接本地query_only后URI只读仍阻止写入；所有原表行、主库字节和零attempt不变 |
| test_unknown_store_412_refuses_before_reader_body_without_writes | 真实411备份到自己的临时副本，只将副本user_version改为明确未知412；不是声称实现412。database()在yield前拒绝，原411与副本原字节均不变 |
| test_historical_408_to_410_schema_reader_contract_stays_readonly | 三个subTest分别执行原发布schema与截至010/011/012的DDL，得到真正对应的空schema，N5表不存在；原reader继续可读且不能写。没有伪造业务成功记录，也没有给411库贴旧版本标签 |

历史方法覆盖读取器的空 schema 兼容，未替代完整已填充历史业务 E2E。
新方法不声称验证 N5-1 持久事件启用后的全部关联语义。

先只添加测试，保留旧 evaluator 字节
`89bc317e52f90471d07e2c6ec6f84bf3c55e11a9f87ec177812c7dd6efd76a47`，运行三个方法：

```text
Ran 3 tests in 2.395s
FAILED (errors=1)
exit_code=1
```

真实 411 的 metadata/hash 检查已通过，失败恰在旧 database() 第176行：
`InvalidEvidence: unsupported Store version`。未知412和历史408–410两个方法通过。
因此旧版红例来自版本白名单遗漏，非缺 CLI／坏迁移／fixture 标签错误或编译失败。

加入 411 后相同三个方法：

```text
Ran 3 tests in 2.539s
OK
exit_code=0
```

修复后的 evaluator SHA-256 为
`770d2200a9086e53552c39458d0f57e880843b89ed45925a7994d9679d73348e`。
新增测试源码在前后运行完全一致，SHA-256
`a525b9533fe7b0f650f93aff7b3d5632448df97ab0c6c3d48354b691a53a12ba`。

此外把**第一次失败运行所保留的同一个真实411库**依次交给冻结旧 evaluator 和修复 evaluator：
前者明确拒绝，后者成功生成并验证 schema_version=1 原计划。两次之间没有准入、改库或模型调用，
主库 SHA-256 始终为 `f69eaad8584eba464b936678ff894eb7d6a527d23b7edbc60516791ef7b5fd7d`；
全部行 iterdump SHA-256 始终为 `ca8d499559f39a223245c8ef87554afcfbb702d8d07043174a4786d731c34bcc`。
这是补充同一夹具的诊断探针，不另算测试套件。

## 原命令、日志和源字节保存

证据根目录：
`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/evidence/n5-1-store411-compat-01`。
before/after-command.json 记录完整 argv、cwd、TMPDIR、UID、输出和实际退出码；before.log
和 after.log 保留原始 unittest 输出。before/after 子目录保留测试自己的合成 SQLite fixtures，
不进入 Git。baseline-inputs 保存基线六文件原字节，inputs-after-fix 保存六文件运行后原字节。

实际定向执行命令形状（两个输出目录分别为 before 与 after）：

```text
python3 <evidence>/run_selected.py <上列411_ELF> <compat-411_source> <evidence>/before
python3 <evidence>/run_selected.py <上列411_ELF> <compat-411_source> <evidence>/after
```

run_selected.py 只从既有 Evaluation 类选择上表三方法并在 fixture 清理前保存合成目录；
它不改 evaluator、不 mock Store、也不修改原 CLI timeout。两次测试进程都取得实际结束状态。

| 原始证据 | SHA-256 |
| --- | --- |
| before.log | 80c6cb3bc7b37d3c7915b86f5f6b1789702af1c5e5321124d7939f062101faed |
| after.log | 9411ec36cbdaf670dddf47e2e25fdae1f8a3c557f5c387c402e360d87cb9c13d |
| same-fixture-probe.json | 1d73c59d8a3a8a1f650be26599dd5e06d5827da5aa74b74f13858cbb0b519e4e |
| inputs-after-fix.json（六文件SHA、ELF、前后命令日志索引） | 66878f55f333768753b7676e243ddc4eb7410d6cb7abf1c582e86ed376183133 |

提交前另检查六个 Python 文件语法、git diff --check、全部实际 diff、限定修改文件和受保护
原路径；不把这些静态检查算产品 suite。最终工作树与提交树核验放入外置 post-commit receipt，
不通过追加自指提交填写本报告 SHA。

## 尚未执行与实际状态

四个 CLI suite 的原完整方法、本 Evaluation 的其他方法、r3_offline_acceptance_cli、
billing_baseline_e2e、历史已填充兼容及全项目 Release／两种umask／sanitizers／严格Clang
由协调者在最终集成输入上执行。本分支未自行启动大型完整 suite，未取得最终远端 CI，
未独立证明 Store 411 事件协议整体完成。

| 状态 | 本报告时域 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | YES：仅本兼容补丁范围 |
| TARGETED_REGRESSION_COMPLETE | YES：原反例exit1，修复3方法exit0，同一原库补充探针通过 |
| LOCAL_VALIDATION_COMPLETE | NO：适用完整集成门禁尚待协调者执行 |
| PUSH_COMPLETE | NO：本子任务未执行发布 |
| REMOTE_CI_COMPLETE | NO：未取得本提交CI结果 |
| PR_REVIEW_COMPLETE | NO：交协调者／非作者审查 |
| MERGED | NO |
