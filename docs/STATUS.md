现在能做什么：U1 单团队、U2 三任务三团队已真实交付，唯一确认回执和 12/12 成果检查均通过。D-46、D-48、显式 JSON 媒体兼容与测试分片已由 #140–#144 合入。
还不能做什么：R11 优化比较、0.13.1 出口和 G3 及后续阶段未完成；实现仍为 0.13.0。G4 真实 U3/U4 等合规宿主。
需要 owner 做什么：当前继续已有授权任务；本机 BLOCKED 的选择保持，不重复要求配置。到宿主依赖或额度边界时按已定规则处理。

# 当前状态

更新：2026-11-17。实现 **0.13.0**，蓝图 **[v0.5](spec/v0.5/01_实施规格_v0.5.md)**，Store **410**；SQL 和已有请求未迁移。

**稳定接手入口：[最新交接](reviews/current-handoff.md)**。唯一前向计划是 [11-16 计划](reviews/claude-2026-11-16/04-plan.md)；G2 当前结论、用量、输入身份和冻结回归只在[运行记录](reports/g2-first-real-run.md)详记。

R6 当时按计划中“明确记录未达原因”的路径完成出口，本次 U1/U2 成功另行记录，不能回写成旧版本已通过；六格式为 `structured-v3`、`lean-v1`、`fixed-role-v1`、`append-fold-v1`、`guided-v1`、`bounded-v1`。E1 当前仍未恢复；后续按 D-51/D-56 在 R11 顺序处理。[#135](https://github.com/wmqfl861/wls861/pull/135) 已合入 `b49bbb1`，[`v0.13.0`](https://github.com/wmqfl861/wls861/tree/v0.13.0) 注释标签已推送，无 Release。

## 已核对输入与验证

D-46 [#140](https://github.com/wmqfl861/wls861/pull/140) 的核心 270/270、源码检查和 D-40 15/15 通过；合入 main `8c3cb975` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43998851346) 450/450 CTest、Make 两 shard 通过。D-48 [#141](https://github.com/wmqfl861/wls861/pull/141) 的核心 270/270、源码检查和 D-40 16/16 通过；合入 main `93b8968` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44002919509)450/450 CTest 与 Make 两 shard 通过。合成、CI 和真实结果详见[运行记录](reports/g2-first-real-run.md#r7r10账目恢复重试与真实复验)，不相互替代。

E1 [#133](https://github.com/wmqfl861/wls861/pull/133) 曾合入 `3fa7019`；本机协议 Release/ASan 各 60/60、同组 29 捕获默认 wire 全等，只有离线字节收益。一次真实 R5 未形成完整比较，回退 [#134](https://github.com/wmqfl861/wls861/pull/134) 已合入 `6f13998`，精确恢复 E1 前树，独立审核、PR 核心和 D-40 通过。R6 的 PR 核心 270/270 和源码检查已核对通过；`b49bbb1` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43946016276)已通过：450/450 CTest、Make 60+25。

此前 `c01df93` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43929349216)通过：450/450 CTest、Make 60+25；它与回退源树相同，仍按原提交登记。PR、main 完整矩阵、本机局部测试和真实请求分列，不能相互替代。

媒体修复 [#143](https://github.com/wmqfl861/wls861/pull/143) 的核心 270/270、源码与受影响 ASan 16/16 通过，合入 main 的完整矩阵也已核验通过。D-20 测试分片 [#144](https://github.com/wmqfl861/wls861/pull/144) 保留全部 24 方法，核心 276/276、受影响 ASan 18/18 及独立源码审核通过并合入；其 main 完整矩阵仍在核对。最新 CI 与真实记录见 [运行记录](reports/g2-first-real-run.md)。

## 长期边界

G4 保持 Strict 默认、Standard 显式选用。绑定及挂载传播修复后，[托管原生](https://github.com/wmqfl861/wls861/actions/runs/43895693147)四场景通过，包含输出、命名空间/挂载和完整回收；Standard 的 `strong_isolation=false`。每夜调度已合入；scheduled native 仍 NOT_RUN。

G4-N3 的[输入切片](reports/g4-run-tool.md)已由 [#136](https://github.com/wmqfl861/wls861/pull/136) 合入 `adb89e46`；合入 main 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43959157815) 10/10 作业、450/450 CTest、Make 60+25 通过。N4 [#137](https://github.com/wmqfl861/wls861/pull/137) 已合入 `87c31473`（树同最终提交 d365）；核心 270/270、源码检查、D-40 14/14、[组合 native 独立复核](https://github.com/wmqfl861/wls861/actions/runs/43965206860) 和[main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43971296505) 均通过；后者 10/10 jobs、450/450 CTest、Make 两 shard，无测试失败/超时/跳过/未运行。W128 容量候选被 TL check 的 1 MiB capture guard 拒绝，已按 D-23 归档并删除清洁分支/工作树，未合 main；正式失败、一次私人计数诊断与归档事实见[阶段记录](reports/g4-run-tool.md)。F-15 写入/提交扩容保持未实现，G4 只部分完成，版本仍为 0.13.0。

本机仍 BLOCKED：缺受信任 bwrap 和专用 cgroup 配置，普通用户 `uid_map: Operation not permitted`；owner 已选择保持该状态，agent 不改主机设置，也不把未配置推断为无 delegation。U3/U4 真实验收 NOT_RUN；下一步 G4-D 真实 run 必须先落实合规原生宿主，CI 不调用真实模型。Windows 11 NOT_RUN。完整记忆、独立审核闭环、私有行清理 runner、daemon/API 尚未完成。

G6 的五个 Store 子域编译切片与可选 ccache 保留，实际并行未验收；单组构建测量不能证明普遍提速。D-39 的历史本机 ASan `event_runtime` 180 秒超时已关闭为“本机现象，托管 CI 未复现”，原失败与派生诊断保留。

每次任务结束同步全部应提交代码和最新公开交接，推送后核对远端提交、文档和 CI；私有原件、凭据、数据库、构建包和本机规则不入库。
