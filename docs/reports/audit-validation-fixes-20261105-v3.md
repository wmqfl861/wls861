# 审计修复验证模块报告 v3 —— 有界更正附录（bounded correction appendix）

- 报告版本：`audit-validation-fixes-20261105-v3`（一次性排他创建：`O_CREAT|O_EXCL|O_NOFOLLOW` 单次写入；本文件不再回写，后续新事实仍由新版本化报告/收据承载）。
- 性质：短更正附录，不重写完整报告。**仅** §2 列明的三项文字事实与 §3 列明的既知过程限定口径由本 v3 优先；v2 的其余全部内容（含其记载的测试结果与有效 UNKNOWN）按既有范围承接。两份不可变 v2 原件与本附录一同保留（发布由根按 report-only 提交路由，本阶段不做任何 Git 写入）。
- 生成阶段：`report-correction-01`（REPORT_CORRECTION_ONLY），由全新 ZCode implement 会话按 FINAL `plan-final-report-correction-v1.md`（SHA-256 `b0e3fb57bef479ca83e8d2427fb8c9c13bda47c6d51f479dd17c594cf1fdd097`）及其绑定版本记录执行；该 FINAL 的计划审核 session 为 `sess_cba34d79-f6d6-4704-a8a5-7845bb7f4d45`（PASS，3 次模型请求）。
- 事实时间范围：本附录更正值测量与机械核验完成于 2026-11-06T05:04:26.783426+00:00（UTC）；不自引用未来提交 SHA 的 CI，不预签任何最终结果硬审结论（按既有路由由后续外部收据绑定）。

## 1. 被更正的不可变原件

| 文件 | 字节 | SHA-256 | 状态 |
| --- | --- | --- | --- |
| `docs/reports/audit-validation-fixes-20261105-v2.md` | 39151 | `5de000f5929a65fc6bdb1dccd2657411012cd599cead4367b45291ff73ec695b` | 原字节保留，不修改 |
| `docs/reports/audit-validation-fixes-20261105-v2.json` | 24419 | `7f3974dbffa526628b5d883f4fae628254bdde5374924992c102fdf5ebbeb21b` | 原字节保留，不修改 |

v1 报告（`c78cfd7da4e32c460241100ba381679b22dcb7ce84d101fb14ac4c1db324411d` / `3cd040285373980d776ab6398c09fb3c66c0ca5592049653ec5ef07a1d549d67`）与全部私有阶段证据同样不变。

## 2. 三项文字事实更正（old → corrected，共六处定位）

### 更正 1 · `scripts/r3_offline_acceptance.py` 的 SHA-256

| 项 | 值 |
| --- | --- |
| v2 现存错误值（65 位，`…c00cc27eb0eb1…` 处多一个字符） | `11e447bc1126aaf71522c00cc27eb0eb1a08d2534219b26881a1e111acd3d6544` |
| 正确值（64 位；本更正阶段新测得的实际文件 SHA） | `11e447bc1126aaf71522c00cc27f0eb1a08d2534219b26881a1e111acd3d6544` |

位置（3 处）：`audit-validation-fixes-20261105-v2.md:33`（13 个改动文件表该文件行）；`audit-validation-fixes-20261105-v2.json:51`（`changed_files_13[4].sha256`）；`audit-validation-fixes-20261105-v2.json:115`（`defect_details.F-01_public_error_projection.fix["scripts/r3_offline_acceptance.py"]`）。

### 更正 2 · `tests/gcc_asan_expected_tests.json`（95 项目录）的 SHA-256

| 项 | 值 |
| --- | --- |
| v2 现存错误值（64 位，`…ffd64977e9e7…` 一位之差） | `620fe97b1d0b502b26f6ffd64977e9e7292423b78283467fd49ffe2816427efd` |
| 正确值（64 位；本更正阶段新测得的实际文件 SHA） | `620fe97b1d0b502b26f6ffd6497769e7292423b78283467fd49ffe2816427efd` |

位置（2 处）：`audit-validation-fixes-20261105-v2.md:23`（95 项目录行）；`audit-validation-fixes-20261105-v2.json:65`（`protected_files_verified_baseline_identical["tests/gcc_asan_expected_tests.json"]`）。

### 更正 3 · v2 PUSH_COMPLETE 说明中的验证叶短 SHA

| 项 | 值 |
| --- | --- |
| v2 现存错误值 | `b9cd31dd` |
| 正确值（短 SHA） | `b9cd31cd` |
| 完整代码头 | `b9cd31cd084b2411589128e7268d39eacac83741`（与冻结清单、根机械记录及本阶段实测 git HEAD 一致） |

位置（1 处）：`audit-validation-fixes-20261105-v2.md:149`（PUSH_COMPLETE 行说明）。该值是**报告前**验证叶代码头，不能冒充未来更正报告提交的 SHA；v2.json 各处（identities/gates）原本即为正确值，无需更正。

除上述三项与 §3 过程限定外，本阶段未对 v2 其余哈希/事实做全面复审；“未发现其他错值”不是本附录的结论；其余内容按 v2 既有范围承接。

## 3. v2 生成阶段的既知过程限定（须与 v2 一同阅读）

1. 排他创建证明等级：v2 阶段的“目标路径先确认不存在后 Write”与原子 O_EXCL 不是同一证明；v2 报告与作者回执中的排他创建/exclusive_create=true 只能保留为实施者自述，原子防覆盖保证为 NOT_ESTABLISHED，不以“单用户所以等效”消除该缺口。
2. 创建后单点 Edit：实施者自述在 v2 JSON 创建后以 Edit 修改一处 tests/r24_recovery_predicates_tests.cpp 哈希，初稿未保留；这是“新建后不覆盖”规则的过程偏差，不以“定稿前”豁免。初稿字节与实际改动完整范围 NOT_RETAINED/UNKNOWN；当前最终值 b66c4d7b13bfb5fa6f03943b0c2909ec2dfd8b42d8c909d8a84aa93f58010ad6 与实际源一致只证明最终值，不证明过去仅发生该单点更改。
3. 核验记录口径：report-input-verification-v1.json 只含归纳与摘录，其 commands_used 是描述性条目；“all outputs preserved in this record”不能称为完整原命令 stdout/stderr。原阶段未单独保全完整工具原输出，入口最终回答 JSON 不能补替；根 2026-11-06T04:23:10Z 的 root-report-mechanical-v1.json 是带新时点的独立机械观测，不能回填为实施时原件。
4. 附件读取范围：v2 最终响应/核验记录明确给出 3 项必须附件、3 项二进制附加材料与 5 项阶段输入共 11 项哈希；其余附加文件的实际完整读取/逐项核验未获独立证明，附件清单中的哈希不证明模型已读内容。candidate.diff 仅做 hash+13 路径结构核验、未全文重读，如实列明；此前首次硬审有其自身范围，不冒称 v2 报告阶段又做了全文代码审查。
5. 入口回执语义：真实入口回执 receipt-implement-9da6efb5.json（session sess_b96b2bae-918a-4861-b901-ce3cb23f57df，12 次模型请求，结论 DONE）是 v2 报告阶段的终态记录，不是最终结果硬审 PASS；作者自写 implement-receipt-v1.json 不代替入口会话身份。
6. 门禁与开放状态不变：LOCAL_VALIDATION_COMPLETE=false、leaf_standalone_tested=false、historical_clang_asan_failures_closed=false 维持；原失败/超时/UBSan 中断与历史 Clang/live_workflow/Make-IPC/sanitizer OPEN、Windows 11 实机 NTFS NOT_RUN、私有原件托管 BLOCKED 不变；GCC 内层 CTest exit 8 与外层 11 份回执 UNKNOWN 的既有裁定继续适用；不得补造执行或批准。

## 4. 测量、来源与范围

- 更正值来源：本阶段（2026-11-06T05:04:26.783426+00:00）对两份源文件**新测得**的实际 SHA-256（`scripts/r3_offline_acceptance.py` → `11e447bc1126aaf71522c00cc27f0eb1a08d2534219b26881a1e111acd3d6544`；`tests/gcc_asan_expected_tests.json` → `620fe97b1d0b502b26f6ffd6497769e7292423b78283467fd49ffe2816427efd`），并与两个既有来源逐字一致：`local-freeze-01/frozen-input-v1.json`（SHA-256 `a254897777332c34b1ba3a46b122fc8e5c2fa4d0060366f815227580e2705cba`）与 `report-implementation-01/root-report-mechanical-v1.json`（SHA-256 `02a108785ba259fb71dd952aa558c7cccf0e0e56e37e10f8b76ba885e7456cc0`，2026-11-06T04:23:10Z）。根机械记录是**新时点**独立观测，本附录不宣称恢复或补回了 v2 生成时的原始命令输出。
- 仓库状态实测：验证叶 HEAD 仍为 `b9cd31cd084b2411589128e7268d39eacac83741`（分支 `fix/audit-validation-20261105-adp35vg4`）；本附录创建前 porcelain 仅为两份未跟踪 v2 报告，创建后仅为 v2×2 + v3×2 四份未跟踪报告；13 个改动文件 + 9 个受保护文件 + `tests/test_support.hpp` 按冻结清单逐项复测不变。
- 本阶段机械核验（源码 SHA / JSON 解析 / 内容定位 / 状态差异核验）属于报告更正的机械核验，**不是**产品复验；未调用产品程序，未构建，未运行 sanitizer/CTest/自检。
- 私有阶段证据位于模块私有证据根 `report-correction-01/`（不入 Git）：核验/创建脚本源文本、argv/cwd/时间、完整 stdout/stderr 与实际退出状态。本 v3 的真实入口实施回执由固定入口在本会话结束后生成并由根绑定（如缺 `plan_review_session_id`，由根按 FINAL 以绑定原回执 SHA 的 sidecar 补充）；本附录不冒充该回执。
