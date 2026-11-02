# N5-2 存储选择包 v1：文本检查记录

时间范围：2026-11-02，初版独立文档工作树检查；后续提案复核修订另在下文标明。
执行者：本地子 Agent `/root/concurrency_fix`；本地执行／集成人 `/root`。
主协调 Agent 另行负责正式架构采纳、任务决策与最终批准；本地分派和发布不代表该批准。
这不是生产功能验证、独立设计批准或真实供应商运行报告。

## 基线、范围与实际产物

基线 PR16：<https://github.com/wmqfl861/wls861/pull/16>，已核对为 OPEN，
head `design/n5-1-persistent-events-20261102-csj777e9`，完整 SHA
`451b232a0dde08220a5e096d91209087b027b42e`，base
`work/coordinator-n0-n4-20261102-0k9`。
本地分支 `design/n5-2-dag-review-storage-20261102-csj777e9`。
工作树位于数据盘
`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/dag-design`。

本地集成人分派在等待 N5-1 集成／编译窗口期间具体化 N5-2 存储选择，明确不做生产实现。
新增四份 [设计文档](../implementation/n5-2-storage-design-20261102/README.md)、
一份来源清单和本报告。现有 N4、N5-1 提案、历史报告／SQL／Schema、third_party、
docs/handoff、C++ 与构建注册均未修改。

推荐选择是：追加不可变 graph/node/edge/acceptance receipts；保存验收前后 CAS，
在 T2/T4 复核两条依赖；新数据库候选内容对象显式映射原 submission；后继采用明确的
ReviewPair host scope 与单层父预算逐次借记。恢复后的依赖重捕获、scope 和预算扩展
都需协调者采纳，不能由本文自行启用。原 `independent_nodes` 与 required-review
Unsupported 守卫保留。

013/411 仅是已批准的 N5-1 事件关联；014/412 仅为图方案提案，票据／预算迁移编号未分配。
本报告不把 N5-1 另一工作树已保存的源码、正常合并或 Draft PR 当成本设计的实现验证。

## 输入与调查限制

[source-inputs.sha256](../implementation/n5-2-storage-design-20261102/source-inputs.sha256)
固定 21 个基线来源文件；生成时逐文件比较 `git show 451b232:<path>` 与工作树原字节。
清单 SHA-256：`4d59004c0514dd1ba27286ac4e087bf76813548e32d7839c5865ca833c921fa5`。
这是设计依据的完整文件身份，不宣称逐行审核整份 store.cpp。

非作者 `/root/delivery_report` 已完成对初版 edf08f0 的有界源码／设计复核，原记录在
`evidence/n5-2-nonauthor-design-review-01/review.md` 和 `review.json`；这不是正式 PR 批准。
修订按 F1 区分现 Fake 三维 unknown 与 live projection 五维完整性；新 review 的五维 HOLD
明确是拟议新增守卫。F2 补齐原 verifier 的矛盾 pass、重复 findings 与精确 host check 集合；
F3 修正本地集成人和主协调 Agent 的角色表述。ReviewPair 恢复前双边验证／原子恢复、新
action 的 T1/T4 尾部预留及 D2 缺见证 HOLD 也作为实施前置写明。

[复核补充输入清单](../implementation/n5-2-storage-design-20261102/review-source-inputs-v2.sha256)
新增固定同一451基线的 `src/store_live.inc` 和 `scripts/validate_contracts.py` 原字节身份，
没有修改原21项清单或设计基线。实际结算守卫／账本重建在 store_live.inc；store.cpp 保存
相关调用和三维原 settlement 更新，两者不能混称为同一处实现。

本会话按 installed codebase-memory skill 做有界结构调查；本子 Agent 无 graph MCP tools，
使用准确源码 read/rg fallback。未以父会话旧图的缺项推断实现不存在。核对了独立计划守卫、
原 submit/acceptance 实现、两种 CAS、十域 binding、原 request_budgets/call_attempts，
Schema 和 N5-1 设计依赖。范围未覆盖未来尚不存在的 014 或 ticket 生产实现。

路径调查曾以猜测名查询 `include/wls/types.hpp`、`004_store_runtime.sql`、
`008_acceptance_delivery.sql` 和 `src/store_reconciliation.inc`；这些名称在基线不存在。
实际依据使用 `include/wls/runtime_types.hpp`、`db/migrations/004_s1_store.sql`、
`db/migrations/008_acceptance_runtime.sql` 与 `src/store.cpp` 的现有账本调用，不据缺失名造 API。
缺失路径的 rg 返回 2，仅是调查修正，不是产品失败或反例。部分过大 read 输出曾截断，
需要引用的具体实现重新按范围读取；不宣称所有批量输出都完整读完。

## 实际命令与结果

本段保留初版六文件的轻量文本／Git 检查结果，不计入产品测试套件。精确 argv、退出码和完整输出在数据盘
`evidence/n5-2-design-static-01/`；检查脚本和日志是该次文本检查的证据，不进入生产构建。

| 检查 | 命令或方法 | 实际状态 |
| --- | --- | --- |
| 工作树基线 | git status --short --branch；git rev-parse HEAD | clean 基线451b232，exit 0 |
| 来源身份 | Python：逐文件 git show 与本地 bytes 相等，再 SHA-256 | 21/21，exit 0 |
| 设计文档链接／格式／协议状态 | Python 静态检查器，包含相对文件链接、代码围栏、提案标签、范围 | 6个新增文件／13个相对链接／21个来源，PASS，exit 0 |
| 来源清单 | sha256sum -c docs/implementation/n5-2-storage-design-20261102/source-inputs.sha256 | 21/21 OK，exit 0 |
| 原路径保护 | git diff --exit-code 基线 -- db schemas third_party docs/handoff src include tests CMakeLists.txt Makefile .github；原提案路径 | 无差异，exit 0 |
| 空白与实际 diff | git diff --check；git status --short；git diff --stat；git diff | diff --check单独执行exit 0；仅6个新增文本文件，原始diff已保存 |

纯文档设计没有复现生产缺陷；旧版红／新版绿为 **NOT_APPLICABLE**，不制造反例。
设计中的 G/R/B/S/C 各行均 **PLANNED / NOT_RUN**，不是实际测试子例数或测试套件数。
本批没有执行 C++ 编译、Release、umask、ASan/LSan、UBSan、Clang、集成、计费 E2E、
旧411 ELF 拒绝或迁移故障测试；这些仍是后继实现的退出要求。

## 决策和交付状态

协调者需对 [README 的 D1–D6](../implementation/n5-2-storage-design-20261102/README.md)
逐项采纳／退回；恢复连续性证明与双边 scope 的实现可行性、父级总账的原子性需独立审查。
特别是原 acceptance 的数据来源与当前图执行权限不能合并，child 的本地账本不能被当作
额外免费的父预算。图片、审核片和异步文件执行相位分别验收。

| 状态 | 本报告时域 |
| --- | --- |
| DESIGN_PACKAGE_PREPARED | YES：具体提案已写出，未获正式采纳 |
| IMPLEMENTATION_COMPLETE | NO：本批无生产实现 |
| LOCAL_VALIDATION_COMPLETE | NO：产品验证未执行；文本检查另列 |
| PUSH_COMPLETE | NO：本报告不承担发布动作 |
| REMOTE_CI_COMPLETE | NO：本包未取得最终SHA CI证据 |
| PR_REVIEW_COMPLETE | NO：已有非作者有界设计复核，正式采纳／PR批准仍待主协调 Agent |
| MERGED | NO |

最终 commit/tree、文档快照 SHA-256 及后续发布状态由数据盘外置 receipt 记录，避免为记录
自身提交 SHA 再生成自指提交。本文保留提交前时域；后续批准／验证用新证据或新版本报告。
