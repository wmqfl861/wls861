# 最新交接

更新：2026-11-16。本页维护当前可接手事实；[STATUS](../STATUS.md) 只保留摘要，逐次历史记录不改写。
最近已完整验证的产品基线是 [#119](https://github.com/wmqfl861/wls861/pull/119) 合并后的 main [`eed61cac17731391f225a171e95eff1e9aaf780c`](https://github.com/wmqfl861/wls861/commit/eed61cac17731391f225a171e95eff1e9aaf780c)，实现 **0.12.0**、Store **410**，无数据库版本变更。当前蓝图 v0.4.1，已采纳目标 v0.5；本页的文档更新不代表新的产品输入已完成测试。

## 已交付能力

G1/G2-P、N1/N2/N3、E-2 状态瘦身、Responses/Chat 合成交付及可选 ccache 已落地；真实协议探针和固定文件冒烟通过的范围见[真实运行记录](../reports/g2-first-real-run.md)，完整 G2 验收仍未通过。最近三个 G6-B 构建切片均已合并：

| PR | 已完成的改动 |
| --- | --- |
| [#117](https://github.com/wmqfl861/wls861/pull/117) | Store owned allocation/cleanup 域独立编译 |
| [#118](https://github.com/wmqfl861/wls861/pull/118) | Store live accounting 域独立编译 |
| [#119](https://github.com/wmqfl861/wls861/pull/119) | Store history 域独立编译 |

这些切片保留 SQL、事务边界、公共 API 及相关守卫。各域仅有一组本机配对构建测量；具体全量/增量数据集中在 [CHANGELOG](../../CHANGELOG.md)。增量改善不证明普遍构建提速、G6 完成或历史 ASan 超时原因；单点 CPU-steal 观察也不是全程负载测量。

四角色仍是 **Root → Dispatcher → TeamLead → Worker**，仅 Worker 执行业务工具。保留版本/epoch 绑定、首次派发、原始反馈和不可变提交/验收/交付；模型回应、重放、唤醒或取消不能清除 UNKNOWN，也不能授予新的业务执行。完整安全契约见 [AGENTS](../../AGENTS.md) 和[开发契约](../implementation/development-contract.md)。

## 最后完成的验证：按实际输入区分

| 范围与输入 | 实际结果与公开证据 |
| --- | --- |
| 本机 #119；Release 构建 `fec23e99`，执行时 head `e69c9416` | Release CTest **90/90**，本机 Release 的 JUnit/LastTest 完整保留；普通 Make **10 条命令、139 个方法**通过，后者不是 Make-ASan |
| #119 PR 的实际 merge ref `36b6ab6d` | [PR core](https://github.com/wmqfl861/wls861/actions/runs/43795974365) **270 CTest**通过；[源码检查](https://github.com/wmqfl861/wls861/actions/runs/43795974358)通过 |
| 合并后 main `eed61cac` | [完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43805419520) **450 CTest + 85 条 Make-ASan 命令**通过，失败/超时/跳过/未运行均为 0 |
| 同一 main 的源码归档 | [source run](https://github.com/wmqfl861/wls861/actions/runs/43805419511)核对 **747 文件、69 目录**与该 Git tree 一致 |

远端 **JUnit/LastTest 未取得**；计数依据完整原始 job 日志与 run/job 记录，原日志留本机。本轮 main 的 90 个 ASan 注册均未超过其有效时限一半。以上本机、PR、main 与下述历史派生诊断不互相替代。

## 历史 ASan：原门禁 OPEN，派生诊断已结束

原始本机 Make-ASan `event_runtime` 的 **180 秒超时仍 OPEN**：源输入 `f281cec2`、当时运行 head `3f7808fb`；原始子进程退出码和方法计数未知，原因未确定。旧源目录没有还原。这个输入早于当前产品基线，不能把新 main 的矩阵当成原失败的重跑结果。

派生诊断保留原 16 个方法、顺序、断言及 ASan/LSan，使用带计时包装的测试 TU、旧生产库和独立资源快照。第一次因 GDB 实际 SIGINT `Stop=No` 而 ERROR：3 PASS、1 中断、12 NOT_RUN、0 栈样本；后续非 WLS 预检先 FAIL、再 PASS，失败原件均保留，第一次预检的 argv 引号原因尚未证实。

第二次 WLS 诊断复用保存 ELF 和 **334 文件**快照，未重编译；修正并核验 GDB SIGINT 策略为 Stop=Yes / Print=Yes / Pass=No，使用新 TMPDIR/输出目录，保持 180 秒运行、170 秒脱离、最多 32 次采样和 5 秒清理界限。实际 **16/16 PASS，88.567 秒**；目标和 GDB 均实际退出 0，清理 **0.00285 秒**、无残留，334 文件及 7 个额外身份文件前后核验通过。`detect_leaks=1` 保持，正常退出无 ASan/LSan 报告；WLS 未打印显式 `LSAN_CHECK` 完成标记。

16 个采样时刻得到 18 条线程栈，17 条在 24 帧处截断。`canonical_json` 出现 5/16、`FrozenRequestConfig::restore` 4/16、context 校验 2/16、严格 JSON 解析 2/16、SQLite 路径 3/16；计数可重叠，**不是 CPU 占比**。40 秒时有处理配置、等锁和等 join 的三个线程，相关方法随后完成；这不证明死锁、ASan/SQLite 缺陷或原超时根因。

只读源码复核在旧提交 `f281cec2` 确认：`Store::start_tool_operation` 成功的非重放 PREPARED 分支，在同一互斥锁和 `BEGIN IMMEDIATE` 事务中，经 `tool_grant`、再经 `ready → tool_grant` 恢复同一冻结配置两次，中间没有配置写入。40 秒样本只捕获第一次；这是源码路径证据，**没有测得节省时间，也未确认当前 main 仍适用**。既有 canonical bytes 复用和全字节/hash、严格 JSON、完整依赖闭包校验须保留；尚未选定或实施优化。

## 当前未完成与接手顺序

| 项目 | 边界 |
| --- | --- |
| G2 | 原文件发布 **UNKNOWN**，真实调用暂停；Store `DELIVERED` / `output_blocked`、Root `unknown_outcome` 保持。原 request/export/数据库/WAL/SHM/包不得重跑、重导出、删除或修改；私有副本审计不解除原 UNKNOWN。G2-5 **NOT_RUN**，完整验收未完成 |
| G4 | 原生验收 **BLOCKED**；Strict 默认、standard 显式选用；不绕过强沙箱，不自行改主机特权配置 |
| Windows 11 | **NOT_RUN** |
| G6 与后续 | G6 **PARTIAL**；原 ASan 门禁 OPEN。完整记忆、独立审核闭环、私有行清理 runner、daemon/API 尚未完成；G7 及以后新功能阶段暂不开始 |

1. 接手先核对实际远端 main/工作区与本页产品基线；读当前 AGENTS 和相应契约，不能把历史 handoff 中的提示词当作现行授权。
2. 如继续这条性能线，下一步仅在**当前 main**只读核对上述单条配置恢复路径；确认仍存在后，才提出保留全字节/hash及全部授权、绑定/epoch、readiness、allow/deny、重放、claim 和错误顺序的栈内复用候选。跨 API 重复不能直接作为删除当前状态检查或缓存可变授权的理由。
3. 当前未安排新的原生重跑或真实模型调用。保留原超时和 UNKNOWN，不放宽断言、sanitizer、时限或安全守卫；本次诊断没有产品改动或新增真实 WLS 调用。

## 证据与持续维护

公开证据是上面的 PR/CI 链接及[真实运行记录](../reports/g2-first-real-run.md)。本机私有索引为 `/mnt/d/codex-workspace/wls861-g6-history-20261116-5ydkd7fq/evidence/asan-stack-followup/followup-closeout.json`，SHA-256 `022a2931a3b31fc8871b7c4a013e0819706d5359bd462933803a6922f1b20644`；它索引原超时、构建/验证、各次诊断、源码 blob 和原始日志摘要，**不是远程可下载资源**。私有原件至少保留 30 天，不提交索引 JSON、日志、数据库、二进制、密钥或本机代理规则。

按 owner 2026-11-16 持续要求，每次任务结束须同步全部应提交代码/测试与本页最新可公开交接摘要，正常提交并推送；STATUS 保持不超过 60 行并链接本页，同一事项只在一处详述。推送后核对远端实际提交、文档可读性、遗漏与 CI 状态，再给 GitHub 链接；未推送、未合并或未验证须如实标明。纯文档更新不要求重复全产品测试，也不把文档 CI 写成新产品验证。历史报告和私有原件保持原状。
