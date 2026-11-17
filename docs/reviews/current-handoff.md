# 最新交接

更新：2026-11-17。[#149](https://github.com/wmqfl861/wls861/pull/149) 已合入，[v0.13.1](https://github.com/wmqfl861/wls861/tree/v0.13.1) 已推送，Store 410；长期状态见 [STATUS](../STATUS.md)，后续按 [唯一前向计划](claude-2026-11-16/04-plan.md)。

现在能做什么：U1、U2 真实交付均通过，原请求导出 UNKNOWN 已按 R0 恢复。E-14、E-11、E-1 已由 [#146](https://github.com/wmqfl861/wls861/pull/146)、[#147](https://github.com/wmqfl861/wls861/pull/147)、[#148](https://github.com/wmqfl861/wls861/pull/148) 合入；R11 完整对照也通过 12/12 成果检查和唯一确认，输入从 375,089 降至 201,365 token。

卡在哪：G3-B0 五场景检查已实现，本机新旧回归通过，等待本 PR 门禁；G4 实机 U3/U4 等合规宿主，Windows 11 未执行。整体项目尚未完成。R11 首次停在 UTF-8 工具语义的待答问题，原状态完整保留；其 6 次调用计入总量，没有把不完整结果算作节省。

下一步：先完成 G3-B0 的五任务成果检查，再做五任务×两拓扑的真实对照、快路径和新版多工具应答；D-50 SSE 修正已推送为 [#150](https://github.com/wmqfl861/wls861/pull/150)，正在跑核心/源码及受影响 ASan。普通单 Agent 的 AMEND/返工/交付确认限制和未知费用必须独立列明。G4 保持宿主等待，不重复要求 owner 修改已决定保留的本机配置。

实际结果、输入身份、调用/重试、未知费用和比较限制集中在 [G2 出口记录](../reports/g2-first-real-run.md#r12出口与r11真实比较)。G2 累计 155/250 次、2,610,716/5,000,000 输入保守占用；R11 两次合计 24 次，无自动重试。组合效果不能分别归因，旧失败不改写。

验证：#148 本机协议 63/63、配置 24/24；PR 核心 276/276、受影响 ASan 19/19、源码检查和源码/离线复核通过。[#148 main](https://github.com/wmqfl861/wls861/actions/runs/44060667435) 完整矩阵 460/460 CTest、Make 两组通过，92 套 ASan 无超半项。R12 本机版本一致性、增量构建和 CLI 14/14、[PR 核心](https://github.com/wmqfl861/wls861/actions/runs/44068439533) 276/276 及源码检查通过；合入后的 [main 完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44070987714) 460/460 CTest、Make 两组通过，92 套 ASan 无超半项。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：`output-headroom-real-xjvkt5yx/` 为成功 U1/U2；`r11-real-prep-owsr_1_5/` 为首次未完成对照及完整备份；`r11-real-second-l1sjsnp1/` 为完成对照；`r11-e*-evidence/` 和 `run-intake/` 为离线/CI 原件。原始数据库只复制核对，分析查询用副本。凭据及私有原件不入 GitHub。
