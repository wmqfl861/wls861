# D-46 观察模式账目恢复

已由 [#140](https://github.com/wmqfl861/wls861/pull/140) 合入 main `8c3cb975`。合成验证通过：观察模式的一次不可用 ERROR 应答停止首批运行后，相同请求第二次运行完成交付，原始 null 用量保持不变。本项开发验证没有调用真实端点；合入后的 R8/R9/R10 均观察到 `accounting.ready=true`，但分别被原冻结期限或响应媒体类型不匹配阻止交付。后者在接收响应体前拒绝，无法补出实际用量；真实 U1/U2 仍未通过，窄范围修正计划进行中，见[运行记录](g2-first-real-run.md#r7r10账目恢复重试与真实复验)。

符合条件的已结算 ERROR 使用原正数 token 预留作为有效投影；`live-status` 显示 `basis: reservation` 和 `ready: true`。账本仍按已知支出加未知预留保守占用，原结算和迟到核对规则不改。PENDING、UNKNOWN、取消、争议、超预留和原额度限制继续阻塞。

本地以 uid 1000、GCC 13 Release 构建：`live_model_store_tests` 30/30，完整 `live_workflow_cli` 23/23，以及两项修改的 run CLI 集成用例 2/2 通过。交付用例启用 structured-v3/append-fold-v1，核对相同请求、原失败行、输出字节和唯一 CONFIRMED 回执。Store 用例覆盖部分用量、重启、迟到核对、争议及 token/调用/时间上限。

首轮 CLI 运行与最终链接重叠导致启动失败，该轮不计验证通过；随后修正一处被 D-46 改变的凭据检查旧断言，定向和完整 CLI 重跑均通过。原始日志及校验记录保存在数据盘私有目录 `wls861-g2-ready-20261117-9_tfsmtv/d46-evidence/development-vs3ytoyg/validation.json`。

差异增删合计：产品 53 行、测试 106 行。最终提交 `3eafe36` 独立审核 PASS；[PR 核心](https://github.com/wmqfl861/wls861/actions/runs/43995465626)270/270、[源码检查](https://github.com/wmqfl861/wls861/actions/runs/43995465610)通过。[D-40](https://github.com/wmqfl861/wls861/actions/runs/43995485977)实际执行 15 个完整测试组，含补入的 `live_model_store`，15/15 PASS。

合入 main `8c3cb975` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43998851346)450/450 CTest、Make 两 shard 通过；ASan/UBSan 各 45+45 覆盖全部 90 个注册组，ASan 最大耗时占比 61.89/180 秒（34.38%），没有组超过期限一半。托管逐作业核对与原日志在本轮 `run-intake/hosted-d46-main140-43998851346/verification.json`，与本机局部验证分别登记。
