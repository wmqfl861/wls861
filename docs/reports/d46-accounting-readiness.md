# D-46 观察模式账目恢复

合成验证已通过：观察模式的一次不可用 ERROR 应答停止首批运行后，相同请求第二次运行完成交付；原始 null 用量保持不变。真实端点未调用，托管 D-40 待分支推送后运行。

符合条件的已结算 ERROR 使用原正数 token 预留作为有效投影；`live-status` 显示 `basis: reservation` 和 `ready: true`。账本仍按已知支出加未知预留保守占用，原结算和迟到核对规则不改。PENDING、UNKNOWN、取消、争议、超预留和原额度限制继续阻塞。

本地以 uid 1000、GCC 13 Release 构建：`live_model_store_tests` 30/30，完整 `live_workflow_cli` 23/23，以及两项修改的 run CLI 集成用例 2/2 通过。交付用例启用 structured-v3/append-fold-v1，核对相同请求、原失败行、输出字节和唯一 CONFIRMED 回执。Store 用例覆盖部分用量、重启、迟到核对、争议及 token/调用/时间上限。

首轮 CLI 运行与最终链接重叠导致启动失败，该轮不计验证通过；随后修正一处被 D-46 改变的凭据检查旧断言，定向和完整 CLI 重跑均通过。原始日志及校验记录保存在数据盘私有目录 `wls861-g2-ready-20261117-9_tfsmtv/d46-evidence/development-vs3ytoyg/validation.json`。

差异增删合计：产品 53 行、测试 106 行。D-40 的 `asan-affected` 固定选择补入 `live_model_store`，共 15 个完整测试组；托管结果由本次 PR 的实际运行记录给出。
