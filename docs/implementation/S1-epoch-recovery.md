# 当前说明的代次恢复（ADR-064）

> 恢复范围以[请求限定恢复契约](S1-request-recovery.md)为准：CLI请求写者只恢复目标，submit仅准入；旧默认Store与显式init仍全库维护。
> R2.4 按 [ADR-091](../adr/091-demand-driven-request-recovery.md) 在事务内判断选中请求的恢复需要，
> 无需恢复时保持 epoch；Actor 另核签发 Store 会话。下列规则仍适用于实际执行的恢复。

本增量承接 0.7.1-s1.6；不改已发布SQL、原说明、原模型调用及旧操作绑定。

- recover 照旧提升请求/团队epoch、释放旧领取、取消PREPARED、把RUNNING/待定调用围栏为UNKNOWN。
- 当前任务/需求/计划/说明修订都匹配时，recover事件记载原epoch、新epoch及原正文SHA-256。
- instructions 表继续不可变。`instruction()`读取的是原行加经最新recover事件验证的有效epoch投影；原始记录仍在原行和instruction事件中。退休任务无活动团队时仍可读取原说明。
- 可执行任务的 applied_instruction_revision 清零；新Worker身份按当前CAS重新ack。原工具消耗、返工次数、问题、候选及历史调用不重置。
- EventRuntime与旧run-fake按语义版本判定是否需要TeamLead；仅epoch变化不预约新的TeamLead调用。
- 同一问题在重开后回答不再因无关新说明而变SUPERSEDED；真实AMEND/计划/说明变化仍会作废。
- ack/ack_change两入口均核恢复投影。同一 Store 会话的旧命令精确重放只返回旧结果，不能完成新代次ack；新键旧actor仍被拒绝。跨 Store 的旧 Actor 即使精确重放也拒绝；当前 Actor 可按原历史绑定重放允许的原结果。
- WAITING、AMEND、阻塞问题、UNKNOWN保持独立门禁。数据库恢复事件失败时所有恢复变更一起回滚。

本项不修全库恢复范围、历史容量、强进程正向部署或真实模型质量；后两P1和R1/R2继续按路线图。
验证记录见本批报告 `docs/reports/audit24-recovery-validation.md`（发布时生成），测试为
`audit24_reopen_tests`、`audit24_reopen_cli_smoke`及既有Store/运行器/CLI全量回归。
