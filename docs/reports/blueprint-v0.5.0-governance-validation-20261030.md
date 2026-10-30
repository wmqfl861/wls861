# R2.4 正式交接与 v0.5.0 规划治理验证（2026-10-30）

本批实际交付是 R2.4 独立工程复核、正式 PR、后续蓝图和开发治理。指定十一项源码检查
没有确认可复现的阻断性生产缺陷，因此没有改动生产代码、测试、构建、迁移或运行资源。
v0.5.0 是 **PLANNED / 待主协调 Agent 审核** 的规划版本；运行版仍为
`0.11.1-r3.3c-audit`、Store `410`，原 v0.4.1 历史文件保持。

本报告记录提交前已发生的检查，不预填自身未来 commit SHA 或未结束的 CI。发布后的
完整 SHA、远端读回、最终 Actions 和源码归档核验另存数据盘
`evidence/blueprint-final-delivery.json` 与 `.md`，随最终交付返回；该回执生成前不能
据本报告推断规划分支已推送或 CI 完成。

## 1. 环境、仓库与授权范围

| 项目 | 实际观察 |
| --- | --- |
| 数据盘 | `/mnt/d`，`/dev/vdb2`、ext4、79 GiB；接手及本轮复核约 55 GiB 可用 |
| 本批 worktree | `/mnt/d/codex-workspace/wls861-blueprint-20261030/feature` |
| 本批日志/临时目录 | 同任务根目录 `evidence/` 与 `tmp/`；测试显式设置数据盘 `TMPDIR` |
| 原交付 | `/mnt/d/codex-workspace/wls861-r2-4-20261029/feature`；原 baseline、构建和失败证据保留 |
| 用户/系统 | ubuntu UID/GID 1000；Linux 6.8.0-147-generic、x86_64；未用 root 执行权限测试 |
| 工具 | Git 2.43.0、CMake 3.28.3、GCC 13.3.0、Clang 18.1.3 |
| main | `a6b8559018a599d7a806b05942857a56d69a00ce` |
| R2.4 开发基线 | `55dc989e02f7e3e2cd1806e53d47298dd8742376`，tree `31f45957a6aeb57bdab0704b89f3d7fdadbae6b3` |
| 本批分支 | `feat/blueprint-v0.5.0-and-r2-4-governance`，独立 worktree 从 R2.4 派生 |

实际执行 fetch/status/rev-parse/remote/未合并分支检查；接手时原 R2.4 工作树干净，
远端 main 与 R2.4 均与指定 SHA 一致。旧 `work/*` 未合并分支已枚举、未整合或覆盖。
没有运行中的项目编译/测试需要接管；现存 Agent/索引进程保留。
初始及后续环境的原命令、退出码和输出分别见外部 `initial-inspection.json`、
`current-environment.json/.log`。GitHub 已有获准的仓库写权限，不复制或打印凭据。

本批没有使用真实模型、付费 API、生产数据或生产服务，没有 main 合并、自动合并、
强推或分支保护修改。未开始并发写者或 Windows 大规模生产开发。

## 2. 阅读与独立审查

团队完整语义阅读实际存在的八份 v0.4.1 规格、26 份 ADR、37 份实施文档、原 reviews
目录十份文本及 40 份历史报告 Markdown；具体分工、路径、SHA 和补读记录分别在
`blueprint-reading.json`、`r24-code-review.json`、`r24-evidence-audit.json`。
AGENTS、STATUS、ROADMAP 和新蓝图另行阅读核对。机器记录使用结构化解析、原日志
终态/失败扫描及归档成员/hash 检查，不能把这些方法称作逐行语义审核全部日志。

结构探索采用已安装的 codebase-memory 技能，先核图覆盖、再补读未解析的实际源码，
没有把图谱缺失解释为安全路径不存在。源码复核由独立于本批文档作者的
`independent_recovery_review` Agent 完成；证据复核由 `evidence_audit` Agent 完成。
这些是另一 Agent 的工程审查，属于同一模型体系，不代表主协调 Agent 已正式批准。

[源码审查](../reviews/r2-4-independent-audit-20261030.md)逐项核对了原独占锁与恢复事务、
空闲/必要恢复、UNKNOWN、StoreActor 所有授权入口及精确重放、WorkerTools 早期验证、
旧 request ID 重构、三元组结算围栏、4096/全局耗尽保护、迟到 prepare 与原安全守卫。
没有确认生产缺陷，故没有另开代码修复分支、制造新红例或增加无关代码提交。

已明确保留的覆盖限制是：4096 容量触顶/容器分配异常没有直接完整触发回归；
CANCEL_REQUESTED、孤立 CLAIMED inbox/FAILED job 没有各自独立的新 R2.4 红绿子例；
并发回归不穷尽所有时序。Actor session 是受信宿主会话围栏，不是对恶意同 UID 任意代码
的密码认证。原 actor-free 迟到事实/结算入口仍按原绑定收证，不能描述为全体 Store 写入
均要求 Actor。以上未被伪称为已复现漏洞，也没有通过放松边界消除疑点。

## 3. R2.4 原执行证据的本轮核验

完整核对方法、原文件和运行身份见[交付证据复核](r2-4-handoff-audit-20261030.md)。
下表是**此前真实执行、本轮重新核验**的结果，不是本轮重新运行全部 C++ 测试。

| 原门禁 | 最终有效结果 | 原记录耗时（秒） |
| --- | --- | --- |
| Release，umask 0022 | 81/81，exit 0 | 1180.883560 |
| Release，umask 0002 | 81/81，exit 0 | 957.807708 |
| Clang 18 严格构建/全套 | 81/81，exit 0 | 1174.086152 |
| UBSan 相关选择 | 31/31，exit 0 | 1156.904475 |
| ASan/LSan 相关选择 | 31/31，exit 0 | 1243.067519 |

四种默认构建完成；原源码输入共 312 项，冻结清单 SHA-256 为
`59c8310d64a961d2ebac1159b93e8aa925499be61795b06831a96d79e2fa2050`。
原恢复、Store、工作流、进程结算、清理、计费、single-agent、计费 E2E 和历史兼容
测试属于实际原计划及上述完整/选择套件，不能额外计为数十个新独立套件。

三个新增 R2.4 逻辑套件共 26 子例（17+6+3）：pristine `a6b8559…` 的核心 6/17、
清理 2/6、CLI 1/3，均 exit 1；修复后各套件全部通过。WorkerTools 原套件另外从
18 增至 20 子例：late-prepare 的红例是 pristine 核心加私有调度观察点，18/19→19/19；
settlement-session 红例是 feature v2 的 19/20→v3 的 20/20，不能称为 pristine 反例。
初始夹具错误、历史 ASan 19/31（12 timeout、exit 8）和内部期限失败都保留，最终同源
成功没有证明早期失败的唯一根因，历史 `local_validation_complete=false` JSON 未被覆盖。

原提交的[六路 CI](https://github.com/wmqfl861/wls861/actions/runs/40930613935)
和[源码快照](https://github.com/wmqfl861/wls861/actions/runs/40930613463)已通过 API、
原 job 日志和下载产物重新核验：五个 CMake job 各 81/81，Make-ASan 原目标与
CPPFLAGS 检查成功。原 source artifact `12920854435` 的 ZIP 摘要为
`sha256:1b4fe5ae088db8bfe39e324dcb4282690b0a83f7c63bd30124906cb877011c98`，
COMMIT/TREE、解压 tar 和本地 `git archive` 逐字节相同。

## 4. 本轮实际执行的适用验证

本轮没有生产差异，以运行输入不变、历史证据复核、现有规格测试、文档语义/链接及
源完整性检查验证交付；没有为了文档改动反复重跑耗时 C++ 门禁或虚构新运行记录。
普通用户、数据盘 TMPDIR，使用原数据盘测试 venv；命令记录器还保存起止时间、
真实退出码、原日志 hash 和执行前后 312 项输入。

| 命令/检查 | 本轮实际结果 | 外部原记录 |
| --- | --- | --- |
| `test-venv/bin/python tests/contracts_tests.py` | 38 项测试 OK，exit 0；输入不变 | `specification-contracts.json/.log` |
| `make verify-handoff PYTHON=…/test-venv/bin/python` | exit 0；33 原文件、32 hash 项/31 payload、51 历史验收仍 NOT_RUN；49 原审核源文件一致 | `published-source-integrity.json/.log` |
| 文档边界、链接/锚点、完整原文件和模式、运行输入、敏感模式、diff 检查 | 首轮 exit 0：688 既有文件中 685 份字节/模式不变，仅允许三份治理文档修改；16 份交付、227 个本地链接有效；定稿后再次冻结 | `documentation-preflight.json`、`documentation-preflight-check.json/.log`；最终为 `documentation-final.json` |
| 新蓝图/治理独立语义复核 | 完成，未留阻断项；八张卡各 11 个必需字段齐全，另全文核对语义、安全和依赖 | `blueprint-independent-review.json` |

可复现的本轮命令（任务根目录）如下；记录名称必须使用新名称，不能覆盖原日志：

```sh
export TMPDIR=/mnt/d/codex-workspace/wls861-blueprint-20261030/tmp
export PYTHONDONTWRITEBYTECODE=1
python3 record.py specification-contracts-rerun feature \
  /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python tests/contracts_tests.py
python3 record.py published-source-integrity-rerun feature make verify-handoff \
  PYTHON=/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python
python3 record.py documentation-rerun feature python3 \
  /mnt/d/codex-workspace/wls861-blueprint-20261030/verify_governance.py documentation-rerun.json
git -C feature diff --check
git -C feature status --short
git -C feature diff --stat
git -C feature diff
```

校验脚本属于数据盘外部取证工具，没有加入项目生产/测试路径。它按固定 R2.4 tree
比较所有既有文件内容和 Git 模式，仅允许 AGENTS/STATUS/ROADMAP 三份既有文档改变；
新增文件必须恰为本次 13 份文档。历史 SQL、third_party、handoff、v0.4.1、ADR、
实施契约、reports 和 reviews 均逐字节保留。源码清单、变更文档清单和实际提交树
在发布时再次核验，关键字扫描不能替代人工判断是否含真实凭据。

新规划的独立审阅发现 S3a 使用了与当前 v0.4.1 不符的追踪编号，已改为原记忆规格
第 2/3/4/10 节的实际链接；同时收紧每张卡的授权措辞，本批只授权卡片编写和设计
资料整理，后继离线验收包或原生构建等仍按具体任务派发。审查记录保存发现及闭环，
这两项文档整改没有被换标为生产代码缺陷或新增运行时红绿测试。

## 5. 实际修改和 PR

新增十份 `docs/spec/v0.5.0/` 文档，覆盖总体架构、S1–S5、模块依赖、Windows 原生、
安全验收、并发调度、真实产品、P0–P7 与发布治理。更新 AGENTS、STATUS、ROADMAP，
另新增独立源码审核、R2.4 证据审核和本报告，共 16 份文档，运行输入不在修改范围。

Windows 无 WSL 的要求成为跨阶段工作流；CreateProcessW、Handle/Reparse Point、
ACL/SID、受限令牌/AppContainer、继承句柄、网络、Job 子树、实际退出和清理权分别
设计与验收，明确 Job Objects 不是完整沙箱。P1 先做写者生命周期/租约/fencing/UNKNOWN
风险设计，P2 先做真实任务材料和费用条件准备；没有把规划写成已实现能力或付费授权。

R2.4 正式 [PR #1](https://github.com/wmqfl861/wls861/pull/1)已实际建立，标题为
`feat: R2.4 demand-driven recovery and Store actor session isolation`；
base `main`、head `feat/r2-4-demand-driven-recovery`、head SHA `55dc989e…`，OPEN、
非 draft、未合并。`r24-existing-pr-check`、`r24-pr-create` 保存去重查询与真实创建结果。
描述列范围、架构、安全边界、红绿来源、五本地门禁、六远端门禁、未实现能力及费用限制。

规划分支按正常 Git 工作流提交/推送；若建立规划 PR，其 base 为 R2.4 分支以便仅审核
文档差异，并明确 P0 依赖。主协调 Agent 决定 R2.4 合并和规划后续集成，本批不自行
rebase/retarget/合并。发布前若远端基线变化，保留成果并报告差异，不覆盖或强推。

## 6. 状态、未执行范围和交回决策

R2.4 `55dc989e…` 的 `IMPLEMENTATION_COMPLETE`、`LOCAL_VALIDATION_COMPLETE`、
`PUSH_COMPLETE`、`REMOTE_CI_COMPLETE` 已据实际原证据完成；
`PR_REVIEW_COMPLETE=PENDING`、`MERGED=false`。新规划文档的发布状态以发布回执为准，
不能继承 R2.4 的提交 SHA 或把本批绿色检查换标成 Windows 验收。

本批未重新执行全部本地 C++/sanitizer 套件（运行输入无改动，已复核原真实记录）；
未执行 Windows 原生、强沙箱部署正向、残留进程接管、真实供应商、付费对照、
4096/分配故障触顶或穷尽并发验证。没有把这些 NOT_RUN 范围算作门禁通过。
已运行文档验证的失败/修订记录同样保留，发布回执说明实际最终结果。

交主协调 Agent 决定：R2.4 正式 PR 是否接受/合并；v0.5.0 是否采纳及集成；
P1 写者方案；P2 端点/模型/可信计费来源/预算与真实调用授权；Windows 最低版本、
架构、文件系统、工具链、隔离支持集合；覆盖缺口是否另列确定性回归卡；P1–P7 的
具体后继执行授权。本地执行 Agent 完成本批交付后不自动启动下一卡。
