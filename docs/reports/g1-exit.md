# G1 合成结果与开放门禁（2026-11-12）

本页是 [05 开发计划](../reviews/claude-2026-11-11/05-development-plan.md)指定的 G1 简报；完整接手入口为[主线交接](../reviews/g1-main-handoff-20261112.md)。**G1 未宣称正式通过**，实现仍为 0.11.1-r3.3c-audit / Store 410。

产品合并基线 main@`b09848e787e307a002ab97282c1b3d95901a397f` 已含A–M（[#66](https://github.com/wmqfl861/wls861/pull/66)，23:09:34 UTC普通合并）；本页是随后文档提交。CI快照截至23:11:47 UTC，新main完整矩阵仍待完成。owner此次特定合并要求不替代完整矩阵验收或正式评审，不扩大未来授权。

| 证据范围 | 已观察结果 |
| --- | --- |
| M 新 head `c2aa6440dec010b9161527c1c80d6b75ccf2081a` 本机注册 `g1_exit_e2e` | 1/1 PASS，19.80 秒；1 条 `wls run`、1 次确认、3 文件任务、32 次本机合成 TLS 调用（D4/TL9/W19），自动纠正一次失败读取 |
| 缓存与估算 | Worker 各任务第 3 次调用起最低模拟缓存读取 72.0093408%；整体 v3 约 62.1915%；估算 234,326 micro-USD（$0.234326）；原 cache-write/provider-cost 保持 NULL |
| A head `a862ad102b226792fcbdecfff18667fc74d760fa` | [PR core 43275846213](https://github.com/wmqfl861/wls861/actions/runs/43275846213) 三路各 82/82 PASS；[source 43275846242](https://github.com/wmqfl861/wls861/actions/runs/43275846242) SUCCESS |
| A 合并 main `b267ae5b7f2d0fff7c20d78225c5263ebd2fcd33` | [full 43323428497](https://github.com/wmqfl861/wls861/actions/runs/43323428497) IN_PROGRESS；[source 43323428558](https://github.com/wmqfl861/wls861/actions/runs/43323428558) SUCCESS |
| 新 M head `c2aa6440dec010b9161527c1c80d6b75ccf2081a` | 本机新Release构建后，注册G1一次1/1 PASS（19.80秒）；[core 43324561502](https://github.com/wmqfl861/wls861/actions/runs/43324561502) 三路各83/83 PASS、[source 43324561578](https://github.com/wmqfl861/wls861/actions/runs/43324561578) SUCCESS；未执行本机全部83项 |
| 产品合并main `b09848e787e307a002ab97282c1b3d95901a397f` | [full 43326432461](https://github.com/wmqfl861/wls861/actions/runs/43326432461) PENDING；[source 43326432580](https://github.com/wmqfl861/wls861/actions/runs/43326432580) SUCCESS；与前一main的在途运行分开登记 |

前一产品候选 `fba19b59853f56af8059121bd5e7dc9a28711c6a` 的 full [43262681195](https://github.com/wmqfl861/wls861/actions/runs/43262681195) 为 CANCELLED：CMake ASan 66 PASS、2 TIMEOUT、1 中断、13 未开始；本机正式 capture 90.17 秒超时仍 OPEN。新分片用于分摊聚合job负载，尚未据此确认完整通过，不能推断单项故障已修复。

复现从准确最终 checkout 以普通用户运行 `ctest --test-dir /absolute/data-disk/build -R '^g1_exit_e2e$' --output-on-failure`（原 300 秒期限）；构建依赖、数据盘临时目录与完整证据矩阵见交接页 §5–8。CTest 注册项、内部子例、Make 命令与模型调用数分别计数。

全部调用为本机合成 TLS；真实供应商与实际付费为 0，D-2/D-3 未给出。Windows 11 NOT_RUN、强沙箱部署正向未验收、私有 CI 原件托管 BLOCKED。原日志/数据库/凭据不上传，历史证据不改写；下一步按交接页有界处理当前组合验证与开放超时。
