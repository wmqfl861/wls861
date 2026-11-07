# 原始 43 项 PR 收敛表（历史队列）与当前状态

**状态：2026-11-06 方案已获 owner 确认并执行——原始队列 #1–#42 已全部关闭（该原始队列 42 个 PR 合并数为 0，仅指此历史队列而非仓库当前合并总数，关闭不等于合并）；#43/#44/#45 已于 2026-11-07 获 owner 对三个精确 head 的按序合并批准并全部正常合并（三项 MERGED，原分支保留），main 现为 [f2a92275ed02b3786777b3b46882dff1e8180ad5](https://github.com/wmqfl861/wls861/commit/f2a92275ed02b3786777b3b46882dff1e8180ad5)；该 main 的完整六配置+source 门禁已于 2026-11-07T15:59Z 全部成功（Linux run [42385790587](https://github.com/wmqfl861/wls861/actions/runs/42385790587)、source run [42385790585](https://github.com/wmqfl861/wls861/actions/runs/42385790585)）；G0 仍 NOT_COMPLETE——G0-4 文档收口已开出为 PR #47（Draft、直接 main），其最终合并与合并后对应 main 门禁未完成；#47 内的 phase1（纯文档首批）已执行、phase2（契约迁移）进行中，二者均为文档步骤，不是 G1 开发阶段。**

- 合并前状态快照（2026-11-07T11:24–11:26Z UTC，只读刷新 PR 列表、检查汇总与远端 ref 清单；历史保留）：原始队列 #1–#42 全部 CLOSED；[#43](https://github.com/wmqfl861/wls861/pull/43) OPEN Draft，head [`4d8e28f08c1ce7242fcf3863cd49675f05f1663e`](https://github.com/wmqfl861/wls861/commit/4d8e28f08c1ce7242fcf3863cd49675f05f1663e)，base 为 main [`a6b8559018a599d7a806b05942857a56d69a00ce`](https://github.com/wmqfl861/wls861/commit/a6b8559018a599d7a806b05942857a56d69a00ce)，待 owner 合并。
- 原 43 个 head ref 仍全部保存：#1–#42 的远端原分支 ref 42/42 存在且仍指向表内精确 head（无缺失、无漂移）；#43 分支 `fix/g0-s1-backport-20261106-wwmxqbtd` 存在并指向当前 head，其前一 head [`8c3087e26cab84b20579f42892d127b9242b2e84`](https://github.com/wmqfl861/wls861/commit/8c3087e26cab84b20579f42892d127b9242b2e84) 作为其父提交仍可达。
- 后继新 PR（不属于本 43 项历史队列，登记于文末后继列表）：[#44](https://github.com/wmqfl861/wls861/pull/44)（CI 减负）、[#45](https://github.com/wmqfl861/wls861/pull/45)（版本常量集中），均 OPEN Draft、直接基于 main；仓库当前 OPEN 共 3 个，PR 总数已达 45——“43 项”只指本表历史队列，不再用于描述仓库总量。
- main 仍为 [`a6b8559`](https://github.com/wmqfl861/wls861/commit/a6b8559018a599d7a806b05942857a56d69a00ce)，未包含 #43 修复；后继 PR 的存在不改变 G0 未完成的状态。
- 合并后状态（2026-11-07，按 owner 批准与合并/验证记录更新）：owner 批准 #43→#44→#45 依次正常合并（普通 merge commit，无 squash/rebase/admin/auto，原分支与提交保留）：#43 → [d28c4a54520338d00e9e3b09ea362aefcbc2df6c](https://github.com/wmqfl861/wls861/commit/d28c4a54520338d00e9e3b09ea362aefcbc2df6c)（2026-11-07T14:25:13Z，父 a6b8559 + 4d8e28f，实际 tree 1df4b257c1afae2a75ed3d0221b2cf9c466e04a9，与此前已通过完整检查的合并候选 af091295d623c0c351eeec57f16bdead75bd4d03 的 tree 一致）；#44 → [11d83eba931bb62f3a45cdaf19633d7384feff5d](https://github.com/wmqfl861/wls861/commit/11d83eba931bb62f3a45cdaf19633d7384feff5d)（15:17:26Z，父 d28c4a5 + 1628364，tree 31f33108d5c45e73e4e3738cd7871b6be507b1fd）；#45 → [f2a92275ed02b3786777b3b46882dff1e8180ad5](https://github.com/wmqfl861/wls861/commit/f2a92275ed02b3786777b3b46882dff1e8180ad5)（15:19:07Z，父 11d83eb + 2159746，tree fcd279d029f1dea93bb4a7f90d6c2bf7e1ce4119 即当前 main tree）。
- #44/#45 合并前的新组合验证：验证分支上的既有 ci.yml 手动完整六配置运行（#44 组合 run 42382158615、#45 组合 run 42382170789，各 5 个 CTest 配置 × 78 项注册测试通过）与 source 归档（run 42382161297 / 42382173458）全部成功；#45 实际合并输入与验证候选 f2381e5a… 等价（tree 相同）；两个新组合由同一次 fresh 组合独立硬审一并覆盖并 PASS（单次审核覆盖两个组合，区别于此前各模块各自的独立硬审）。三项批准所依据的事实为其当前提交均已推送、独立硬审核与 CI 通过。
- 最终 main 的完整六配置+source 门禁已于 2026-11-07T15:59Z 全部成功：Linux run [42385790587](https://github.com/wmqfl861/wls861/actions/runs/42385790587)（六产品配置、5 个 CTest 配置 × 78 项注册测试、0 失败/超时/跳过）与 source run [42385790585](https://github.com/wmqfl861/wls861/actions/runs/42385790585)（归档与 commit/tree/字节核对一致）。#44 合并后的中间 main Linux run 42385652115 因后续 main 推送自然 CANCELLED（取消不算通过；其 source run 42385652162 成功），精确组合验证已全部通过的事实不变。仓库当前 OPEN 为 2（后继 #46/#47，均 Draft、直接基于 main），最终合并各需 owner 单独确认。

## 历史快照（2026-11-06 方案制定时点，仅作历史语境保留）

- 快照：2026-11-06T15:04:26Z 采集、15:53:43Z 刷新确认 42 个 PR 全部 OPEN、0 合并、40 个 Draft（仅 #1、#2 非 Draft），head 与 base 分支未漂移；main = [`a6b8559018a599d7a806b05942857a56d69a00ce`](https://github.com/wmqfl861/wls861/commit/a6b8559018a599d7a806b05942857a56d69a00ce)。数据按冻结时点描述。
- 归档语义：保留现有远端原分支 ref 与精确 head，获 owner 确认后关闭 PR；不为改名重复推送、不删除原分支、不强推、不为归档制造新的批量 push。执行时先核每个待关 PR 的远端 head 未漂移且原 ref 仍持久可达。
- 汇总：4 个集成包装建议**关闭**（#10、#22、#25、#31）；37 个**转归档分支**（关闭 PR、保留原 ref/head）；#40 在 G0-1 摘取替代可审/通过后**关闭**；0 个建议整链合入。
- 祖先事实（按精确 head 祖先检查）：叶头 #21、#26、#37、#38、#39、#40、#42 互不包含；#40 比 main 多 129 提交、含 31 个其他 PR 头，不包含 #21/#26/#33/#34/#35/#37/#38/#39/#41/#42（#19 当前头已包含）；联合分支只含 5 个修复模块中的 2 个，故不能把 #40 当作可整体合入的"最新代码"。
- #20 特例：其 base 分支为 #19 的 head 分支，但 API 上报 base（`bef9cec0…`）是创建时快照；该分支当前 tip 已前移至 `0c33889a…`，且当前 tip 不是 #20 head 的祖先（merge-base 仍为 `bef9cec0…`）。**#20 不包含 #19 的当前 head**，不能把 API base 快照当作当前 #19 head。

## 43 行处置表（原始队列 #1–#43）

表内 #1–#42 行为 2026-11-06 历史登记，其处置已按 owner 批准执行完毕（关闭/转归档，原 ref 与 head 保留，2026-11-07 复核无漂移）；“当前 base tip”等列为当时快照值。#43 行为 2026-11-07 补入的当前状态行。

| PR | Draft | head（完整 SHA） | base 分支（对应 PR） | 当前 base tip（完整 SHA） | 处置 | 理由 |
| --- | --- | --- | --- | --- | --- | --- |
| [#1](https://github.com/wmqfl861/wls861/pull/1) | 否 | [`55dc989e02f7e3e2cd1806e53d47298dd8742376`](https://github.com/wmqfl861/wls861/commit/55dc989e02f7e3e2cd1806e53d47298dd8742376) | `main` | `a6b8559018a599d7a806b05942857a56d69a00ce` | 转归档分支 | R2.4 是 S1 恢复/actor 隔离修复，不属用户指定冻结类别。历史 pristine main 三套 RED（6/17、2/6、1/3 通过后退出 1）与候选通过为历史证据（未重跑）。暂归档仅为收敛活跃 PR，不否定既有反例；保留原远端 ref/head 与失败证据。G1 跨进程重开/批次恢复若重现连续运行阻塞或 actor/CAS 隔离失效，优先评估独立最小回补，不能当作无关功能丢弃。 |
| [#2](https://github.com/wmqfl861/wls861/pull/2) | 否 | [`079b52766ee0929472165a71a8563ce31671bb46`](https://github.com/wmqfl861/wls861/commit/079b52766ee0929472165a71a8563ce31671bb46) | `feat/r2-4-demand-driven-recovery`（#1） | `55dc989e02f7e3e2cd1806e53d47298dd8742376` | 转归档分支 | v0.5.0 蓝图与治理扩展；阶段路线以本次 G0→G3 为准，旧蓝图保留历史。 |
| [#3](https://github.com/wmqfl861/wls861/pull/3) | 是 | [`1a5ca4f3549e1531066a159b0e410e69b1a57573`](https://github.com/wmqfl861/wls861/commit/1a5ca4f3549e1531066a159b0e410e69b1a57573) | `feat/blueprint-v0.5.0-and-r2-4-governance`（#2） | `079b52766ee0929472165a71a8563ce31671bb46` | 转归档分支 | #1 的恢复安全补充：正文记录 a6 六个场景在必要恢复后因空闲重复打开改写持久快照失败、R2.4 通过；既有 ASan 失败仍开放。随 #1 暂归档并保留 ref/head；S1 恢复触发条件出现时同批复核并最小摘取必要测试。 |
| [#4](https://github.com/wmqfl861/wls861/pull/4) | 是 | [`772feaee62086ddb0e7f605a7472721d3bc61406`](https://github.com/wmqfl861/wls861/commit/772feaee62086ddb0e7f605a7472721d3bc61406) | `test/r2-4-recovery-boundary-coverage`（#3） | `1a5ca4f3549e1531066a159b0e410e69b1a57573` | 转归档分支 | GCC ASan 专用运行器扩展；旧失败保留，G0-2 使用现有标准测试入口，不整链引入。 |
| [#5](https://github.com/wmqfl861/wls861/pull/5) | 是 | [`21f5308271505706594f1a03941ec6ab4a804d50`](https://github.com/wmqfl861/wls861/commit/21f5308271505706594f1a03941ec6ab4a804d50) | `fix/r2-4-gcc-asan-alignment`（#4） | `772feaee62086ddb0e7f605a7472721d3bc61406` | 转归档分支 | Windows 基础层；用户明确 G2 前冻结。 |
| [#6](https://github.com/wmqfl861/wls861/pull/6) | 是 | [`1c279f6770f5f4440adcf6b8ee2f51e2401d9a74`](https://github.com/wmqfl861/wls861/commit/1c279f6770f5f4440adcf6b8ee2f51e2401d9a74) | `fix/r2-4-gcc-asan-alignment`（#4） | `772feaee62086ddb0e7f605a7472721d3bc61406` | 转归档分支 | sanitizer 诊断与完整清单工具；保存失败定位成果，按需取用，不作为 S1 前置合入。 |
| [#7](https://github.com/wmqfl861/wls861/pull/7) | 是 | [`e8a861f32e29e191645f788d20eee8e5ad0e8533`](https://github.com/wmqfl861/wls861/commit/e8a861f32e29e191645f788d20eee8e5ad0e8533) | `fix/sanitizer-followup-20261031`（#6） | `1c279f6770f5f4440adcf6b8ee2f51e2401d9a74` | 转归档分支 | 十槽离线评测包；不代替 G1/G2 的三任务真实工作流，保留后续对照参考。 |
| [#8](https://github.com/wmqfl861/wls861/pull/8) | 是 | [`e78791ca1420204b6205301ec9f22a2e1595fa96`](https://github.com/wmqfl861/wls861/commit/e78791ca1420204b6205301ec9f22a2e1595fa96) | `fix/sanitizer-followup-20261031`（#6） | `1c279f6770f5f4440adcf6b8ee2f51e2401d9a74` | 转归档分支 | 异步协调器；G2 前冻结，main 的同步 S1 为当前基线。 |
| [#9](https://github.com/wmqfl861/wls861/pull/9) | 是 | [`86b82f8f63a3c60c7659c9cd8f9b8e7d7252870b`](https://github.com/wmqfl861/wls861/commit/86b82f8f63a3c60c7659c9cd8f9b8e7d7252870b) | `feat/coordinator-async-20261031`（#8） | `e78791ca1420204b6205301ec9f22a2e1595fa96` | 转归档分支 | 公平调度与 A4 反例；G2 前冻结。 |
| [#10](https://github.com/wmqfl861/wls861/pull/10) | 是 | [`99c9063f3e29cd6d69c56fb15815715d499a374a`](https://github.com/wmqfl861/wls861/commit/99c9063f3e29cd6d69c56fb15815715d499a374a) | `feat/fair-scheduling-20261031`（#9） | `86b82f8f63a3c60c7659c9cd8f9b8e7d7252870b` | 关闭 | 多模块集成包装；来源 PR/远端提交已保存，无需把冻结模块整体合入 main。 |
| [#11](https://github.com/wmqfl861/wls861/pull/11) | 是 | [`7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a`](https://github.com/wmqfl861/wls861/commit/7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a) | `work/next-cycle-integration-20261031`（#10） | `99c9063f3e29cd6d69c56fb15815715d499a374a` | 转归档分支 | 异步控制重放/跨池死锁修复；随异步线归档，保留已知修复供复启时采用。 |
| [#12](https://github.com/wmqfl861/wls861/pull/12) | 是 | [`08dad7e815b9c3c84d3bd2393818b2030a6e0cde`](https://github.com/wmqfl861/wls861/commit/08dad7e815b9c3c84d3bd2393818b2030a6e0cde) | `fix/async-review-99c9063-20261031`（#11） | `7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a` | 转归档分支 | Store 事件序列化与诊断复用；如 G1 的 main 反例证明需要，再单独摘取，当前不带入异步依赖。 |
| [#13](https://github.com/wmqfl861/wls861/pull/13) | 是 | [`ceaa9b3e18618c592345001dfc597bdaf579cf81`](https://github.com/wmqfl861/wls861/commit/ceaa9b3e18618c592345001dfc597bdaf579cf81) | `fix/zcode-local-validation-20261101`（#12） | `08dad7e815b9c3c84d3bd2393818b2030a6e0cde` | 转归档分支 | 计费子测试失败诊断；保留历史，不把诊断增量当作 P1-1 产品计费修复。 |
| [#14](https://github.com/wmqfl861/wls861/pull/14) | 是 | [`4289589384d75f29d42233cbaad256ceb711e9bf`](https://github.com/wmqfl861/wls861/commit/4289589384d75f29d42233cbaad256ceb711e9bf) | `fix/zcode-local-validation-20261101`（#12） | `08dad7e815b9c3c84d3bd2393818b2030a6e0cde` | 转归档分支 | 旧验证报告与附件；保留其失败/缺口范围，历史可由原分支访问。 |
| [#15](https://github.com/wmqfl861/wls861/pull/15) | 是 | [`aa2c592b8aed6509189710cc6f6d69e0959098a8`](https://github.com/wmqfl861/wls861/commit/aa2c592b8aed6509189710cc6f6d69e0959098a8) | `docs/zcode-final-validation-20261101`（#14） | `4289589384d75f29d42233cbaad256ceb711e9bf` | 转归档分支 | N0–N4 综合修订含 SSE 与工具；保留来源，G1-2 如需 SSE 增量必须单独复现后摘取。 |
| [#16](https://github.com/wmqfl861/wls861/pull/16) | 是 | [`451b232a0dde08220a5e096d91209087b027b42e`](https://github.com/wmqfl861/wls861/commit/451b232a0dde08220a5e096d91209087b027b42e) | `work/coordinator-n0-n4-20261102-0k9`（#15） | `aa2c592b8aed6509189710cc6f6d69e0959098a8` | 转归档分支 | 持久事件设计；与 Store 411 一同冻结。 |
| [#17](https://github.com/wmqfl861/wls861/pull/17) | 是 | [`af3bd2b14ed497a57453c0261f26d0c6b52190be`](https://github.com/wmqfl861/wls861/commit/af3bd2b14ed497a57453c0261f26d0c6b52190be) | `work/coordinator-n0-n4-20261102-0k9`（#15） | `aa2c592b8aed6509189710cc6f6d69e0959098a8` | 转归档分支 | Windows 只读身份基础；G2 前冻结。 |
| [#18](https://github.com/wmqfl861/wls861/pull/18) | 是 | [`77cf51265211d9f766926dfb5e6aad59c1604d94`](https://github.com/wmqfl861/wls861/commit/77cf51265211d9f766926dfb5e6aad59c1604d94) | `work/coordinator-n0-n4-20261102-0k9`（#15） | `aa2c592b8aed6509189710cc6f6d69e0959098a8` | 转归档分支 | 有界 JSON 序列化与私有证据工具；保留源码，S1 若复现必要缺陷才拆出产品部分。 |
| [#19](https://github.com/wmqfl861/wls861/pull/19) | 是 | [`0c33889ae75e5672f08984cefd90401fa5595e86`](https://github.com/wmqfl861/wls861/commit/0c33889ae75e5672f08984cefd90401fa5595e86) | `design/n5-1-persistent-events-20261102-csj777e9`（#16） | `451b232a0dde08220a5e096d91209087b027b42e` | 转归档分支 | Store 411 持久收件箱；默认 main 保留 410，不引入 013。 |
| [#20](https://github.com/wmqfl861/wls861/pull/20) | 是 | [`97e3c3f75e35b674d2a254aa0a80bf89ccdf9a59`](https://github.com/wmqfl861/wls861/commit/97e3c3f75e35b674d2a254aa0a80bf89ccdf9a59) | `feat/n5-1-persistent-inbox-20261102-csj777e9`（#19） | API 上报 `bef9cec095e972f7d5c7ff7e14ff917a9c43c4fe`；当前分支 tip `0c33889ae75e5672f08984cefd90401fa5595e86`（非本 PR head 祖先） | 转归档分支 | 持久事件接入异步协调器；G2 前冻结。base 特例：当前基线分支（#19 head 分支）tip 已前移，本 PR 不包含 #19 的当前 head（merge-base 仍为 `bef9cec0`），不能把 API base 快照当作当前 #19 head。 |
| [#21](https://github.com/wmqfl861/wls861/pull/21) | 是 | [`4326ea5004889c4e14b31e26c6b86e6b0a8509b3`](https://github.com/wmqfl861/wls861/commit/4326ea5004889c4e14b31e26c6b86e6b0a8509b3) | `design/n5-1-persistent-events-20261102-csj777e9`（#16） | `451b232a0dde08220a5e096d91209087b027b42e` | 转归档分支 | DAG/双边审核存储设计；G2 前冻结。 |
| [#22](https://github.com/wmqfl861/wls861/pull/22) | 是 | [`79654ca9cca80d5ae8f5204df580797b04963460`](https://github.com/wmqfl861/wls861/commit/79654ca9cca80d5ae8f5204df580797b04963460) | `feat/n5-1-async-events-20261102-csj777e9`（#20） | `97e3c3f75e35b674d2a254aa0a80bf89ccdf9a59` | 关闭 | N5 综合集成包装；源分支保留，不整体合并。 |
| [#23](https://github.com/wmqfl861/wls861/pull/23) | 是 | [`a99ad867a25ad0404bf4b4b7e6a5b76d446022d0`](https://github.com/wmqfl861/wls861/commit/a99ad867a25ad0404bf4b4b7e6a5b76d446022d0) | `work/n5-integrated-candidate-20261102-csj777e9`（#22） | `79654ca9cca80d5ae8f5204df580797b04963460` | 转归档分支 | Windows Store 411 资源夹具；随 Windows/411 冻结。 |
| [#24](https://github.com/wmqfl861/wls861/pull/24) | 是 | [`0c7a7c2478c532fc5114e178df8e63bbc93a8070`](https://github.com/wmqfl861/wls861/commit/0c7a7c2478c532fc5114e178df8e63bbc93a8070) | `fix/n5-native-capture-fixture-20261102-csj777e9`（#23） | `a99ad867a25ad0404bf4b4b7e6a5b76d446022d0` | 转归档分支 | Windows 失败身份留存；随 Windows 冻结。 |
| [#25](https://github.com/wmqfl861/wls861/pull/25) | 是 | [`c2a5cea64aeea261b214cdfd9a968b53f491110a`](https://github.com/wmqfl861/wls861/commit/c2a5cea64aeea261b214cdfd9a968b53f491110a) | `work/n5-integrated-candidate-20261102-csj777e9`（#22） | `79654ca9cca80d5ae8f5204df580797b04963460` | 关闭 | N5 最终集成包装；源分支与精确候选头保留，默认不引入 411。 |
| [#26](https://github.com/wmqfl861/wls861/pull/26) | 是 | [`f5d1fc77fb886a78bd80ccb901f7dc851ee00b7a`](https://github.com/wmqfl861/wls861/commit/f5d1fc77fb886a78bd80ccb901f7dc851ee00b7a) | `work/n5-final-candidate-20261102-csj777e9`（#25） | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | 转归档分支 | N5 最终验证与未解门禁；独立叶头必须留存，失败仍开放。 |
| [#27](https://github.com/wmqfl861/wls861/pull/27) | 是 | [`a2ca1055db58dab10aa254029a01c15e1e0c9f18`](https://github.com/wmqfl861/wls861/commit/a2ca1055db58dab10aa254029a01c15e1e0c9f18) | `work/n5-final-candidate-20261102-csj777e9`（#25） | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | 转归档分支 | ASan 方法投影证据；保留结果，不扩建证据框架。 |
| [#28](https://github.com/wmqfl861/wls861/pull/28) | 是 | [`8ec950f7ffbf7ece0d9192abc31a60f1c1aa2c29`](https://github.com/wmqfl861/wls861/commit/8ec950f7ffbf7ece0d9192abc31a60f1c1aa2c29) | `work/n5-final-candidate-20261102-csj777e9`（#25） | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | 转归档分支 | Windows SID/镜像身份收紧；随 Windows 冻结，不能当作原生产品已验收。 |
| [#29](https://github.com/wmqfl861/wls861/pull/29) | 是 | [`3888d95a53572e06825d774aae732d01f7996903`](https://github.com/wmqfl861/wls861/commit/3888d95a53572e06825d774aae732d01f7996903) | `work/n5-final-candidate-20261102-csj777e9`（#25） | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | 转归档分支 | 未来 Store 版本拒绝与 sidecar 保护；与 411 链解耦后仅在 main 出现可复现 S1 缺口时摘取；严格 WAL/SHM 兼容仍开放。 |
| [#30](https://github.com/wmqfl861/wls861/pull/30) | 是 | [`3a2dd55d51e32e7e49ed546ccb4c441d3bd96782`](https://github.com/wmqfl861/wls861/commit/3a2dd55d51e32e7e49ed546ccb4c441d3bd96782) | `work/n5-final-candidate-20261102-csj777e9`（#25） | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | 转归档分支 | 持久事务/关闭覆盖；随持久事件冻结。 |
| [#31](https://github.com/wmqfl861/wls861/pull/31) | 是 | [`f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc`](https://github.com/wmqfl861/wls861/commit/f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc) | `work/n5-final-candidate-20261102-csj777e9`（#25） | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | 关闭 | N5 审查集成包装；各来源保留，避免再次引入混合依赖链。 |
| [#32](https://github.com/wmqfl861/wls861/pull/32) | 是 | [`3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`](https://github.com/wmqfl861/wls861/commit/3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123) | `work/n5-review-integration-20261103-74bnxwj5`（#31） | `f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc` | 转归档分支 | 持久运行器测试 Gate 停止唤醒修复；保留其历史 RED/GREEN，随持久事件冻结。 |
| [#33](https://github.com/wmqfl861/wls861/pull/33) | 是 | [`537b3aef5d5671325239d626448608041de8b79a`](https://github.com/wmqfl861/wls861/commit/537b3aef5d5671325239d626448608041de8b79a) | `fix/n5-stop-gate-20261103-74bnxwj5`（#32） | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | 转归档分支 | proc/DSO 观察工具；G2 前冻结新的证据工具。 |
| [#34](https://github.com/wmqfl861/wls861/pull/34) | 是 | [`9cd526130609a8eb0187f1ee65cf0b28aa4e182c`](https://github.com/wmqfl861/wls861/commit/9cd526130609a8eb0187f1ee65cf0b28aa4e182c) | `fix/n5-stop-gate-20261103-74bnxwj5`（#32） | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | 转归档分支 | Windows 证据与缺口报告；保留历史，不继续报告版本链。 |
| [#35](https://github.com/wmqfl861/wls861/pull/35) | 是 | [`efcd4547bd7627fa821a4b7bce097e633b514982`](https://github.com/wmqfl861/wls861/commit/efcd4547bd7627fa821a4b7bce097e633b514982) | `fix/n5-stop-gate-20261103-74bnxwj5`（#32） | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | 转归档分支 | Store 严格兼容观察工具与报告（v3）；保留未通过状态，不将工具当作 410 兼容通过。 |
| [#36](https://github.com/wmqfl861/wls861/pull/36) | 是 | [`a5675e1245bebba41c499e56bd2c8a16be2d5dae`](https://github.com/wmqfl861/wls861/commit/a5675e1245bebba41c499e56bd2c8a16be2d5dae) | `fix/n5-stop-gate-20261103-74bnxwj5`（#32） | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | 转归档分支 | 持久提交结果与 callback 关闭测试；随持久事件冻结。 |
| [#37](https://github.com/wmqfl861/wls861/pull/37) | 是 | [`7cf8216fd29d72973951b97e312db47bb59624fb`](https://github.com/wmqfl861/wls861/commit/7cf8216fd29d72973951b97e312db47bb59624fb) | `test/n5-compatibility-20261104-_7wxnkyn`（#35） | `efcd4547bd7627fa821a4b7bce097e633b514982` | 转归档分支 | 兼容观察工具完整性与子进程回收；独立叶头归档，严格 WAL/SHM 产品缺口仍开放。 |
| [#38](https://github.com/wmqfl861/wls861/pull/38) | 是 | [`60aafac565b152ee2a8373f74bb860b2ccf24fdb`](https://github.com/wmqfl861/wls861/commit/60aafac565b152ee2a8373f74bb860b2ccf24fdb) | `fix/n5-proc-evidence-20261104-_7wxnkyn`（#33） | `537b3aef5d5671325239d626448608041de8b79a` | 转归档分支 | proc 读取与对象稳定性证据修复；独立叶头归档，暂停该工具线。 |
| [#39](https://github.com/wmqfl861/wls861/pull/39) | 是 | [`2719f7103e07a4efb331c1ef2a826329f0ac26fc`](https://github.com/wmqfl861/wls861/commit/2719f7103e07a4efb331c1ef2a826329f0ac26fc) | `fix/n5-windows-evidence-20261104-_7wxnkyn`（#34） | `9cd526130609a8eb0187f1ee65cf0b28aa4e182c` | 转归档分支 | Windows 进程树退出证据；精确头 `2719f710` 的 Windows 运行 109/110 仍失败（最近 10 次查询均失败），G2 前冻结。 |
| [#40](https://github.com/wmqfl861/wls861/pull/40) | 是 | [`2326e521b483bf0a910bc8cac3c96accd38f59f2`](https://github.com/wmqfl861/wls861/commit/2326e521b483bf0a910bc8cac3c96accd38f59f2) | `test/n5-persistent-workflow-20261104-_7wxnkyn`（#36） | `a5675e1245bebba41c499e56bd2c8a16be2d5dae` | 关闭（已执行；替代摘取 = #43，2026-11-07 已合并） | 由 G0-1 摘取 [`0542d38`](https://github.com/wmqfl861/wls861/commit/0542d38) 的 4 源+4 测试增量替代后关闭，原 PR 不整链合入：0542 单提交 10 文件=8 代码/测试+2 报告；本 PR 相对 merge-base 全 delta 12 文件=同 8 代码/测试+4 报告；报告一律不摘取。本 PR 比 main 多 129 提交、含 31 个其他 PR 头，不能作为"最新代码"整体合入。 |
| [#41](https://github.com/wmqfl861/wls861/pull/41) | 是 | [`a90d96fc3e47ecb6397e07fd926bcb7e874f90a5`](https://github.com/wmqfl861/wls861/commit/a90d96fc3e47ecb6397e07fd926bcb7e874f90a5) | `test/n5-persistent-workflow-20261104-_7wxnkyn`（#36） | `a5675e1245bebba41c499e56bd2c8a16be2d5dae` | 转归档分支 | 验证工具、评测脱敏、夹具修复；保留该线及未解 sanitizer 状态，按需拆出真正阻塞 S1 的部分。 |
| [#42](https://github.com/wmqfl861/wls861/pull/42) | 是 | [`2d48b3bb4318251c38a00f2a83b516067089a9e0`](https://github.com/wmqfl861/wls861/commit/2d48b3bb4318251c38a00f2a83b516067089a9e0) | `fix/audit-validation-20261105-adp35vg4`（#41） | `a90d96fc3e47ecb6397e07fd926bcb7e874f90a5` | 转归档分支 | 历史发表索引已在远端；保留这一个历史入口，不向 main 重导报告、诊断源码和 vN 链。 |
| [#43](https://github.com/wmqfl861/wls861/pull/43) | 是 | [`4d8e28f08c1ce7242fcf3863cd49675f05f1663e`](https://github.com/wmqfl861/wls861/commit/4d8e28f08c1ce7242fcf3863cd49675f05f1663e) | `main` | `a6b8559018a599d7a806b05942857a56d69a00ce` | MERGED（2026-11-07 owner 批准后正常合并） | G0-1 落地 PR：从 main 开分支，只摘取 [`0542d38`](https://github.com/wmqfl861/wls861/commit/0542d38) 对 8 个文件（4 源+4 测试）的增量（不带 013 迁移与持久事件/异步依赖），并补 `tests/s1_cli_smoke.py` 缺失 DB disposition 注释更正与 update/keep/cancel 三个缺失 DB 用例（本地套件 14 个测试方法通过，其中新增 1 个方法含 3 个 subTest；该本地结果已通过一次独立硬审核，不等同远端检查）。远端验证分开记录：旧 head [`8c3087e`](https://github.com/wmqfl861/wls861/commit/8c3087e26cab84b20579f42892d127b9242b2e84) 的 12 项 Linux 检查与 1 项 source 检查全部成功，仅绑定旧 SHA；新 head `4d8e28f` 的远端检查与独立硬审核其后完成通过；2026-11-07 获 owner 对该精确 head 的合并批准，14:25:13Z 正常合并为 [`d28c4a54520338d00e9e3b09ea362aefcbc2df6c`](https://github.com/wmqfl861/wls861/commit/d28c4a54520338d00e9e3b09ea362aefcbc2df6c)（父 a6b8559 + 4d8e28f），实际 tree 1df4b257c1afae2a75ed3d0221b2cf9c466e04a9 与此前已通过完整检查的合并候选一致，原分支保留。 |

## R2.4（#1/#3）处置政策

R2.4 不属用户明确冻结的 Windows/411/异步类别。#1/#3 暂归档仅为收敛活跃 PR，不否定既有 S1 恢复反例；保留原远端 ref/head 和历史失败证据。G1 涉及跨进程重开/批次恢复时复核已保留场景；若恢复重复改写、actor/CAS 失效或 UNKNOWN/预算/兄弟请求护栏影响 S1 连续运行，先重新评估独立最小回补，不将其塞进 G0-3 版本重构。

## 阶段 0 候选 PR 与顺序（G0）

42 个旧 PR 的收敛已按 owner 确认执行完毕。G0 工程 PR 直接基于 main 开出：#43、#44、#45 相互独立、不堆叠，最终合并顺序由 owner 决定；每次 main 合并仍需具体 owner 确认，对收敛方案的确认不构成对未来代码 SHA 的合并许可。2026-11-07 owner 批准三个精确 head 按 #43→#44→#45 顺序合并并已执行完毕（详见文首合并后状态）；该批准不构成对未来新 PR（G0-4 docs、disposition helper）合并的许可。

### G0-1 `fix: backport validated disposition and retained-path fixes`（= PR #43，已合并）

- 范围：从 main 开新分支，只摘取 [`0542d38`](https://github.com/wmqfl861/wls861/commit/0542d38) 对 8 个文件（4 源+4 测试）的增量；不复制 R 整文件、不整 cherry-pick 含 2 份报告的提交；不带 013 迁移与任何持久事件/异步依赖。
- main 基线：`a6b8559`（Store 410）。
- 验证：先在 main 上以测试获得目标 RED（复现目标限定为长路径 abandon 与效果前处置校验两项缺陷），再以 4 源修复转 GREEN；其他原有护栏通过不冒称新增 RED。严格 Release、umask 0002、Clang 与相应完整 sanitizer/Make-ASan；不用旧叶头结果替代新 backport 结果。
- 当前状态（2026-11-07）：已开出为 [#43](https://github.com/wmqfl861/wls861/pull/43)（实际标题 fix: backport S1 disposition and retained-path guards to Store 410），head `4d8e28f0`；除 8 文件摘取外含本批 `tests/s1_cli_smoke.py` 缺失 DB disposition 注释更正与 update/keep/cancel 三个缺失 DB 用例（本地套件 14 个测试方法通过，其中新增 1 个方法含 3 个 subTest；该本地结果已通过一次独立硬审核，不等同远端检查）。远端检查：旧 head `8c3087e` 的 12 项 Linux 检查与 1 项 source 检查全部成功，仅绑定旧 SHA；最终 head `4d8e28f` 的远端检查与独立硬审核其后完成通过。2026-11-07 获 owner 对该精确 head 的合并批准，14:25:13Z 正常合并为 [`d28c4a54520338d00e9e3b09ea362aefcbc2df6c`](https://github.com/wmqfl861/wls861/commit/d28c4a54520338d00e9e3b09ea362aefcbc2df6c)（父 a6b8559 + 4d8e28f），实际 tree 1df4b257c1afae2a75ed3d0221b2cf9c466e04a9 与此前已通过完整检查的合并候选一致，原分支保留。
- owner 关口：2026-11-07 已获对该精确 head 的合并批准并执行；后续新 PR 仍各需单独确认。#40 已在替代 PR 开出且旧头检查全绿后按既定条件关闭。

### G0-2 `ci: run one Linux PR gate and schedule full validation`（= PR #44，已合并）

- 范围：所有现存 workflow 的 push 仅监听 main；PR 仅走 pull_request；`concurrency` 按 workflow 与 ref 分组、`cancel-in-progress: true`；同一 Linux 入口内做轻量文档检测与稳定 required 汇总加 strict/umask 0002/Clang；源码/测试/Schema/config/CI 变更仍跑完整 PR 核心；ASan/UBSan/Make-ASan 移至 main push、nightly 与显式 release gate；已存在的 windows-native 改手动触发（main 默认不含该链，不凭空引入）；升级 actions/checkout（实施时核官方固定 SHA 与 runner 兼容）；归档分支既有触发器不因 main 变更自动消失，不再批量 push。
- main 基线：`a6b8559`（直接基于 main 开出，不堆叠于 #43；最终合并顺序由 owner 决定）。
- 验证：文档-only 提交走轻检查且 required check 不长期 Pending；一次 PR 推送只触发一套 Linux 运行；未解历史 sanitizer 保持 OPEN，并发取消不能变 PASS；发布前所发精确 SHA 须完整矩阵通过。CI 政策调整不改名旧失败。
- 当前状态（2026-11-07）：已开出为 [#44](https://github.com/wmqfl861/wls861/pull/44)，head `162836443d69e2b532867bfbdd61899cd4521a10`，分支 `ci/g0-events-20261107-iBL7iz`；其远端检查与独立硬审核其后完成通过。2026-11-07 获 owner 对该精确 head 的合并批准；合并前按新组合（d28c4a5 + 1628364）在验证分支上以既有 ci.yml 手动触发完整六配置运行（run 42382158615，5 个 CTest 配置 × 78 项注册测试通过）与 source 归档（run 42382161297）全部成功，该新组合与 #45 新组合由同一次 fresh 组合独立硬审一并覆盖并 PASS；15:17:26Z 正常合并为 [`11d83eba931bb62f3a45cdaf19633d7384feff5d`](https://github.com/wmqfl861/wls861/commit/11d83eba931bb62f3a45cdaf19633d7384feff5d)（父 d28c4a5 + 1628364，tree 31f33108d5c45e73e4e3738cd7871b6be507b1fd），原分支保留。
- owner 关口：2026-11-07 已获对该精确 head 的合并批准并执行；后续新 PR 仍各需单独确认。

### G0-3 `refactor: derive current Store version and retain 410 compatibility`（= PR #45，已合并）

- 范围：`MigrationRegistry::latest_version` 仍为 410；不加入 013、不加实验开关、不改历史 SQL；集中"当前最大/默认/诊断"版本常量，保留历史 401…410 迁移边界与 from/to 语义；一致性测试核 manifest/registry/默认 Store 三处一致。
- main 基线：`a6b8559`（直接基于 main 开出，不堆叠；最终合并顺序由 owner 决定）。
- 验证：已有请求的 410 库可不 init 升级直接打开、user_version 仍 410；schema、冻结配置与原业务证据不因版本集中被改写；资源身份保持；兼容拒绝未来版本。按既有打开模式允许合法恢复事件与 epoch/lease/claim 运行时状态变化，**不要求 DB 整文件或全部行字节恒等**。维护性去重是静态事实，不伪造业务 RED；如另发现行为问题须先独立 RED。
- 当前状态（2026-11-07）：已开出为 [#45](https://github.com/wmqfl861/wls861/pull/45)，head `2159746c849fa1d8a86c0998c1235c94d031aa1f`，分支 `refactor/store-current-version-20261107-iBL7iz`；其远端检查与独立硬审核其后完成通过。2026-11-07 获 owner 对该精确 head 的合并批准；合并前按新组合（11d83eb + 2159746）验证：验证候选 `f2381e5a` 上的完整六配置运行（run 42382170789，5 个 CTest 配置 × 78 项注册测试通过）与 source 归档（run 42382173458）全部成功，实际合并输入与验证候选等价（tree 均为 fcd279d029f1dea93bb4a7f90d6c2bf7e1ce4119），该新组合与 #44 新组合由同一次 fresh 组合独立硬审一并覆盖并 PASS；15:19:07Z 正常合并为 [`f2a92275ed02b3786777b3b46882dff1e8180ad5`](https://github.com/wmqfl861/wls861/commit/f2a92275ed02b3786777b3b46882dff1e8180ad5)（父 11d83eb + 2159746），原分支保留。main 现为 f2a9227。
- owner 关口：2026-11-07 已获对该精确 head 的合并批准并执行；后续新 PR 仍各需单独确认。R2.4 空闲重开改写快照的恢复语义不并入本 PR，另按 #1/#3 触发条件评估。

### G0-4 `docs: state S1 capabilities and compact operating guidance`（进行中）

- 范围：STATUS 首屏只写当前能力与缺口，批次历史入 CHANGELOG；AGENTS.md 精简至不超过 100 行，细节移回可追溯契约；本两份处置/收敛文档随后一并纳入；每 PR 一页说明；不复制历史附件与私有收据。
- main 基线：`a6b8559` + G0-1…G0-3 之后串行。
- 验证：文档 review；`docs/**` 仅轻量检查。
- owner 关口：合并 main 前具体确认。
- 当前状态（2026-11-07）：第一批已执行并发布——本两份处置/收敛文档以已发布原件（分支 `docs/claude-s1-convergence-20261106-svddnu4h`，[`a442b4b95525064e3af949e4c31b15bbb4fb8f72`](https://github.com/wmqfl861/wls861/commit/a442b4b95525064e3af949e4c31b15bbb4fb8f72)）为底本，按 2026-11-07 合并与验证证据更新，提交 f119933 进入 G0-4 docs 分支并已创建为 PR [#47](https://github.com/wmqfl861/wls861/pull/47)（Draft、直接 main）。其纯文档头真实检查行为已如实记录：Linux run [42388742503](https://github.com/wmqfl861/wls861/actions/runs/42388742503) 三个同名轻量作业成功、产品构建/CTest 按既定分类 NOT_RUN（轻量不等于产品 78/78 通过），source run [42388742522](https://github.com/wmqfl861/wls861/actions/runs/42388742522) 成功（检出 b34d47c2 / tree bf43de04），无 push 运行。AGENTS/STATUS/CHANGELOG/development-contract 契约迁移为第二批，正在同一 PR 追加；追加后的最终 head 按分类预期走三核心编译配置，其证据与纯文档头轻量证据分开记录，最终 head 的远端检查与独立硬审完成后才向 owner 请求该新 PR 的合并确认。

### G0 完成门禁（五项；2026-11-07 #43/#44/#45 合并后更新）

1. 主干含 #40 两项修复（经 G0-1 摘取）——已达成：#43 已于 2026-11-07 合并（merge commit d28c4a54520338d00e9e3b09ea362aefcbc2df6c），main 含两项修复。
2. 打开的 PR 不超过 3 个，且没有 PR 以未合并 PR 为基础——2026-11-07T16:04Z 只读复核：当前 2 OPEN（后继 #46/#47 均 Draft、直接基于 main、不堆叠），满足。
3. 一次 PR 推送只触发一套 Linux 运行——已由实际新产品 PR 推送验证：#46 head 5073527 推送仅触发一套 Linux pull_request 运行（run [42388192709](https://github.com/wmqfl861/wls861/actions/runs/42388192709)，3 个 CTest 配置 × 78 项注册测试）加一项 source 归档（run [42388192705](https://github.com/wmqfl861/wls861/actions/runs/42388192705)），无 push 运行。
4. 纯文档提交不跑约 40 分钟门禁——已由 #47 纯文档头 f119933 记录：run [42388742503](https://github.com/wmqfl861/wls861/actions/runs/42388742503) 仅三个同名轻量作业（产品构建/CTest 按分类 NOT_RUN）+ source run 42388742522；追加契约文件后的最终 head 走三核心编译配置属既定分类预期，非门禁回退。
5. 默认构建能不升级地打开已有请求的 410 库——#45 已合并；其一致性回归已在 #44+#45 组合验证（tree fcd279d029f1dea93bb4a7f90d6c2bf7e1ce4119，即当前 main tree；5 个 CTest 配置 × 78 项注册测试通过）中通过；最终 main checkout 的完整六配置+source 门禁已于 2026-11-07T15:59Z 全部成功（Linux [42385790587](https://github.com/wmqfl861/wls861/actions/runs/42385790587) + source [42385790585](https://github.com/wmqfl861/wls861/actions/runs/42385790585)）。

本文两份文档的完成不等于 G0 完成。

## 未来阶段登记（只登记目标与关口）

以下条目本轮不设计、不编码、不调用真实模型；格式纠错类调用始终消耗原请求预算，不借 G1 授权真实付费重试。

- **G1（接入真实模型前六项改造，每项一个 PR）**
  - G1-1 计费：本地 TLS 同请求连续 30 次调用无需签名收据；估算费用与按单价手算一致；超预算仍在预约前停下。
  - G1-2 协议：官方推理模型响应与 SSE 流式样例回放通过；未声明字段仍被拒绝；instructions 为纯文本。
  - G1-3 纠错回路：Fake 脚本"只回文字 / 参数不合 Schema / 读取不存在的文件"三种情况在下一次调用内被纠正且批次不中断；UNKNOWN、权限、预算、取消、deadline、存储、不变量错误仍安全停机。
  - G1-4 上下文：规定 token 预算内 60KB UTF-8 旧结果不超过 3 次调用读完；宿主核对哈希后分页；新请求默认 structured-v2 加 window-v1；不对任意压倒预算的文本作承诺。
  - G1-5 联网：Ubuntu 24.04 系统 libcurl 上按域名完成一次本地 TLS 测试。
  - G1-6 付费保护：请求总墙钟、80% 预算预警、重复错误停滞检测各有回归且 `config check` 非 validated_only；一个 3 任务请求在一次授权内跑完。
  - G1 总门禁：本地 TLS 合成端点上同一请求 30 次以上调用跑完 3 个只用文件工具的小任务，无需签名收据、无人介入。
- **G2（首个真实闭环）**：owner 书面决定 P-01（供应商、模型、端点；建议先用非推理模型）与 P-05（每请求预算上限，付费重试 0 次）之前，不做任何真实或付费调用；随后跑 3 个只用文件工具的小任务，记录成功率、格式错误率、token、估算费用与耗时，写一页报告。
- **G3（三条件对照）**：先补普通单 Agent 工具循环（G3-1）；多角色分层、role-broker 基线、普通单 Agent 三个条件各跑 10 组；结论决定异步协调器、持久事件、S2 并行、第二适配器与 Windows 原生是否继续投入。

## 后继列表（2026-11-07 起，不属于上述 43 项历史队列）

后续新增 PR 只登记于此，不回填 43 行历史队列；仓库 PR 总数不再以 “43 项” 表述。

| PR | 角色 | head（完整 SHA） | base | 状态（2026-11-07 合并后更新） | 下一步 |
| --- | --- | --- | --- | --- | --- |
| [#44](https://github.com/wmqfl861/wls861/pull/44) | CI 减负（G0-2） | [`162836443d69e2b532867bfbdd61899cd4521a10`](https://github.com/wmqfl861/wls861/commit/162836443d69e2b532867bfbdd61899cd4521a10) | main | MERGED（2026-11-07T15:17:26Z，merge commit [`11d83eba`](https://github.com/wmqfl861/wls861/commit/11d83eba931bb62f3a45cdaf19633d7384feff5d)，父 d28c4a5 + 1628364；组合验证 run 42382158615 完整六配置 + source run 42382161297 成功，组合独立硬审 PASS；原分支保留） | 已合并；后续新 PR 各需 owner 单独确认 |
| [#45](https://github.com/wmqfl861/wls861/pull/45) | Store 当前版本常量集中（G0-3） | [`2159746c849fa1d8a86c0998c1235c94d031aa1f`](https://github.com/wmqfl861/wls861/commit/2159746c849fa1d8a86c0998c1235c94d031aa1f) | main | MERGED（2026-11-07T15:19:07Z，merge commit [`f2a9227`](https://github.com/wmqfl861/wls861/commit/f2a92275ed02b3786777b3b46882dff1e8180ad5)，父 11d83eb + 2159746；组合验证 run 42382170789 + source run 42382173458 成功，实际输入与验证候选等价（tree fcd279d），组合独立硬审 PASS；原分支保留） | 已合并；main 现为 f2a9227，最终 main 完整门禁已于 2026-11-07T15:59Z 全部成功 |
| [#46](https://github.com/wmqfl861/wls861/pull/46) | disposition 词表/整数取值共用 helper（#43 后续维护，非 G0 门禁项） | [`5073527ffc9052f648cf00785ae5230e1b7c78ad`](https://github.com/wmqfl861/wls861/commit/5073527ffc9052f648cf00785ae5230e1b7c78ad) | main | OPEN Draft（未合并；本地注册 CTest 4/4、offline 子例 16/16；远端 PR 核心三配置 run [42388192709](https://github.com/wmqfl861/wls861/actions/runs/42388192709)（3 × 78 项注册测试，0 失败）与 source run [42388192705](https://github.com/wmqfl861/wls861/actions/runs/42388192705) 成功；独立硬审 PASS（2026-11-07T16:00Z 收据）） | 待 owner 最终合并确认 |
| [#47](https://github.com/wmqfl861/wls861/pull/47) | G0-4 文档收口（处置/收敛两表 + AGENTS/STATUS/CHANGELOG/development-contract 契约迁移） | 纯文档头 [`f119933e5629dce5abe022ba67a847dfaa22b95d`](https://github.com/wmqfl861/wls861/commit/f119933e5629dce5abe022ba67a847dfaa22b95d)（第二批追加后 head 将前移） | main | OPEN Draft（未合并；纯文档头 docs-only run [42388742503](https://github.com/wmqfl861/wls861/actions/runs/42388742503) 与 source run [42388742522](https://github.com/wmqfl861/wls861/actions/runs/42388742522) 成功） | 待最终 head 的远端检查与独立硬审后，向 owner 请求该新 PR 合并确认 |

- 后继顺序与 owner 门禁（2026-11-07T16:04Z 更新）：owner 已批准 #43→#44→#45 三个精确 head 按序合并并执行完毕；同项答复批准继续 G0 文档收口与 disposition helper 去重，两个后续新 PR 的开出（普通 commit/push 与 Draft PR 创建）属已批准的后续开发范围、无需再请示——#46 与 #47 现已开出（均 Draft、直接基于 main）。二者最终合并仍各需在具体 head/CI/硬审齐备后单独请示 owner，三项精确 PR 批准不自动延伸。G0 在 G0-4（#47）进入 main 且合并后对应 main 完整门禁成功前保持 NOT_COMPLETE；#47 内的 phase1/phase2 均为该 PR 的文档步骤，不是 G1 开发阶段。

## 执行边界

- 已执行（owner 确认收敛方案后）：按表关闭 #1–#42，全部保留原远端 ref 与精确 head（2026-11-07 复核 42/42 存在且无漂移）；开出 #43（G0-1）、#44（G0-2）、#45（G0-3）；2026-11-07 owner 批准后按 #43→#44→#45 顺序以普通 merge commit 正常合并（d28c4a5 / 11d83eb / f2a9227，原分支保留），main 推进至 f2a92275ed02b3786777b3b46882dff1e8180ad5。未强推、未删除分支、未改 Git 配置、未改数据库版本或 SQL。
- 可自主推进（已批准的后续开发范围，无需 owner 再确认）：G0-4 docs PR #47 与 helper PR #46 均已发布为 Draft（直接 main）；剩余可自主推进项为 #47 第二批契约迁移追加提交的普通 commit/push。
- 待 CI（非 owner 审批项）：最终 main 完整门禁已成功（2026-11-07T15:59Z）；尚待：#47 追加后最终 head 的三核心检查与独立硬审、#46/#47 合并后对应 main 门禁。
- 待 owner：后续两个新 PR（G0-4 docs、disposition helper）的最终 main 合并各需 owner 具体确认。
- #43 新头的远端完整检查、#44/#45 的检查与独立硬审核均已在合并前完成通过；各时点的运行中/待验证表述为当时快照。不以旧头结果、本地套件或单次成功记录替代整体判定的原则继续适用于后续新 PR 与最终 main 门禁。
- 本轮更新仅改写本文件与同目录处置表两份文档；无产品代码、测试、Git 写入或数据库变更。
