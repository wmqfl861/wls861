# 最新交接

更新：2026-11-17。当前 main 为 [f31647d](https://github.com/wmqfl861/wls861/commit/f31647d9b2424fcb22c7cb5e28e77235e5489df2)，[#156](https://github.com/wmqfl861/wls861/pull/156) 已合入；实现 0.13.1、Store 410。长期边界只在 [STATUS](../STATUS.md) 维护。

现在能做什么：G2 U1/U2 已真实交付；G3-C1/C2 的冻结初始计划/工单已合入，同一合成文件任务总调用 7→6→5。B1 的真实结果、费用未知及比较限制见 [G3 报告](../reports/g3-comparison.md)。CSV 的原 CLI→Worker run→不可变提交准备已合入，止于 SUBMITTED，详见 [G4 记录](../reports/g4-run-tool.md#g4-d-csv-准备候选2026-11-17)。

当前候选：`feat/g3-file-batch-history-20261117`，源实现 [b870074](https://github.com/wmqfl861/wls861/commit/b870074b66c9ca285b43982b1d9cbbbe30c3da82)，已正常整合上述 main。E9 仅增加显式可信主机上下文的双文件调用历史表示，分别保留两个工具结果与原 native ID；生产调用继续使用关闭的默认上下文，新配置值和批执行入口仍关闭。产品 191 行、测试 301 行；两个 PR 的 600 行总限余 409 行。

卡在哪：该候选本机定向验证通过，一次独立有界源码复核与最终提交核心、源码、26 组 affected-ASan 待完成。128/512 KiB 只是表示上限；实际序列化容量、可信 Store 配置路由与顺序派发必须在第二切片证明，当前不能宣称批执行完成。

并行：G4-D 原“生成小函数和测试并实际运行”场景从 main f31647d 开发；中断→AMEND→同命令恢复仍是后续准备项。下一步先收口 E9 表示层，再从合入后的准确 main 接执行层，最后 B2。B2 的 fixed-single-task 不适用于 partial_amend，旧 v4 不认证新宿主初始化，按实际适用性和原始 oracle/工作流分列。唯一前向计划为 [11-16 §15](claude-2026-11-16/04-plan.md#15-修订记录)。

## 验证与证据

main `2213c27` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44160566010)与[源码](https://github.com/wmqfl861/wls861/actions/runs/44160566058)已核验：475/475 CTest、Make 61+25 通过；全部 95 个 ASan 套件低于半时限，最高 acceptance 71.48/180 秒。PR155 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44166272729) 288/288、[源码](https://github.com/wmqfl861/wls861/actions/runs/44166272804)和[D-40](https://github.com/wmqfl861/wls861/actions/runs/44166266566) 26/26 已核验通过；main `67f52f0` 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44172025562)仍单独收件，不能替换为 PR 结果。

PR156 最终 head `586eaea` 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44173217114) 294/294、[源码](https://github.com/wmqfl861/wls861/actions/runs/44173216920)及[native](https://github.com/wmqfl861/wls861/actions/runs/44173270997)已独立核验 PASS：三配置各 98 项，失败/超时/跳过/未运行均 0，源码与 780 个 blobs/modes 同树；native 为精确 head、UID 1001，旧四沙箱+两环境场景和 CSV 原结果核验通过。已保险备份、正常合并并删除清洁分支/工作树。新 main f31647d 的[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/44176163480)及[源码](https://github.com/wmqfl861/wls861/actions/runs/44176163468)另行登记。

E9 本机 UID 1000、GCC 13.3 Release 的八套定向检查通过；最终只增强测试 wire 数量/末项断言后，protocol_codec 重编重跑 67/67、0.80 秒。38 项契约及历史源码完整性通过。初次编译和夹具失败原件保留；实际 wire 为 anchor+五项原 native 内容+current-state，七项及两个完整不同结果逐项核对。D-20 的新负载估算 0.80×10=8 秒，低于现有 ASan 180 秒期限半数；实际 ASan 仍待执行。仅 protocol_codec TU 新增测试，26 组选择器已覆盖，不机械增加 CTest 注册数。

私有证据根 `/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/`：PR156 最终收据 `run-intake/hosted-pr156-586eaea/verification.json`（SHA256 `1adf15ba938d8fe598d08db99bd6b714d8c1a1e78f6212d3d48c639f97fc0764`）；E9 本机 `g3-e9-history-evidence/result.md`；main155 的原件与恢复点在 `run-intake/hosted-main155-67f52f0/`。原始失败、旧输入及 B1 ELF 完整保留；数据库、凭据、构建、长日志和本机规则不入 Git。本批新增真实模型调用 0。
