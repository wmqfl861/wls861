# G1-3 纠错整改交接说明（BLOCKED 开发快照）

日期：2026-11-09。性质：**开发快照交接页**。组件状态：**BLOCKED / NOT_FULL_GREEN /
NOT_COMPLETE**；不构成 Controls 依赖关闭、G1 验收、CI 通过、合并或发布资格。
本页是当前改动之外唯一新增文件，不改动任何既有源码、测试或报告字节，也不预声明
任何提交/树/远端结果；本页事实已经本地 root 机械核对，其提交与推送由新的 ZCode
会话在既有用户授权下正常执行。

## 快照内容（相对固定基线 4987f2c）

- 生产 7 路径合计 added+deleted=329（上限 800）：C1 同一次显式调用内跨 claim、
  跨成功累计的纠错额度（按受信 ExecutionBinding 与冻结配置哈希分链，成功或 claim
  完成不清零）；C2 真实资格耗尽时输出有界 blocked(code=correction_limit) 报告、
  live 立即停止整批（更强既有停止原因优先）；单点 Clang 修复
  （`FrozenRequestConfig::schema_retry_max()` 返回值显式 int64 转换）。
- 测试 10 路径，其中 7 个冻结 T1 文件字节原样保留；文档 3 路径为
  docs/implementation/R3-live-workflow.md 追加节与两份历史报告。
- 上述 20 个路径当前字节与通过完整源硬审的候选逐字一致（本页写入前逐文件核验）。

## 实测结果（保留原始 FAIL/PASS，未重试）

- focused 九注册选择：9/9 通过。
- 全量 78 注册：76/78；两个失败注册保留——doctor_diagnostics_cli 1/12 方法、
  review_cli_smoke 3/14 方法，均为 executable-relative 资源布局类 unsupported 失败，
  与 C1/C2 修复无语义交集。
- unset 六注册兼容运行：实际 6/6 通过，但在全量失败后执行，属路线偏离，
  NOT_AN_EVIDENCE_RUN。
- 布局结论：可执行文件相对资源布局不匹配有强源码/可执行文件依据；布局控制实验
  NOT_RUN（原 2400 秒截止已过期），不存在经验证的布局控制 PASS。
- 六 lane C0/G 配对矩阵（strict0022/strict0002/Clang18/UBSan/ASan/Make-ASan）NOT_RUN。

## 独立审核（两个范围，分别保留原结论）

- 先前有限范围硬审 PASS：仅覆盖最小 C1/C2 修复语义、单点转换、T1 冻结与既有
  有限验证真实性。
- 新完整源硬审 PASS：逐字覆盖全部 7 生产 + 10 测试 + 3 文档相对固定基线的全部
  差异，未发现源码正确性/安全/契约缺陷或测试围栏弱化。
- 两者均为源范围审查，均不关闭 Controls 依赖、G1 验收、CI 或合并门。

## 保留缺口（未由上述 PASS 消除）

- 资源合规未证明：首采样前约 557.4 秒缺口、最大相邻采样间隔约 149.8 秒；
  仅观测到无空间不足，不能倒推全阶段合规。
- 历史物理环境实体闭包不完整；当前 venv 文件身份 111 vs 105 计法对账未决；
  GCC 实施收据缺 plan_review_session_id 链接（经版本记录补足，原件未改）。
- 全量 78 两失败、unset 顺序偏离、六 lane 矩阵与布局控制 NOT_RUN 状态不变。

## 历史文档范围说明

docs/reports/g1-correction-validation.md（2026-11-08 v6 阶段记录）与
docs/reports/g1-correction-remediation-v8.md（同期勘误与源码准备）均为时点范围
记录；docs/implementation/R3-live-workflow.md 追加节描述 0.11.1-r3.3c 有界纠错
契约。三者均不是当前组件合格性声明。

## 固定状态

Windows 11 NOT_RUN；私有证据托管 BLOCKED；Store schema 410 与既有 SQL 不变；
真实/付费 WLS 模型调用 0；开放 PR 与发布流程不在本页范围。
