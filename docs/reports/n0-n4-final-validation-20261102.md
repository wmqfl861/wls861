**N0–N4 实现与验证记录（2026-11-02，v4）**

本轮实现与所需证据收集已收尾，完整本地门禁未通过，整体保持 **VALIDATION_BLOCKED**。
本记录固定区分三个输入：7bf3 的完整正式矩阵、b58f7b0 的后继 Windows 工具夹具整合，
以及承载本报告/索引的后续文档发布提交。后续发布提交自身的 SHA、远端读回和该 SHA
的 Actions 结果写入外部交接收据；本报告不预填未来 CI，也不通过再次提交来更新自身 CI。
配套的[机器索引](n0-n4-final-validation-20261102-index.json)提供相同结论及证据定位。
正式矩阵于 08:21:34 UTC 结束；collector 于 08:22:07 UTC 形成 COMPLETE/FAILED。
单诊断和 e23 整合后的 16 方法检查均已结束。7bf3 远端审计截点仍为 02:36:08 UTC，
最新实现发布前范围核验截点为 08:35:43 UTC；这些不同时间/源码范围不会互相代替。

| 身份 | 已确认值 |
| --- | --- |
| 原始 main | `a6b8559018a599d7a806b05942857a56d69a00ce` |
| 协调交接资料提交 | `c33978ca62fe3fc63b4cf3440afdc4f5b647f13b`；这是资料输入 |
| 本轮集成起点 | `f3fc9bfaf345030ee994097d2a2443e3c290e9fc` |
| 集成双父，按序 | PR14 `4289589384d75f29d42233cbaad256ceb711e9bf`；PR13 `ceaa9b3e18618c592345001dfc597bdaf579cf81` |
| 完整正式矩阵输入 | `7bf3a16d9683ed6a8de65d88023bae9687abd20d` |
| 完整矩阵 tree | `02e78d39f96473e81801ad4eb884e7e01bdfe534` |
| 分支 / PR | `work/coordinator-n0-n4-20261102-0k9`；[Draft PR15](https://github.com/wmqfl861/wls861/pull/15) |
| PR base | `docs/zcode-final-validation-20261101`，精确为上述 PR14 SHA |
| 最终实现 HEAD / tree | `b58f7b0e63ad68f67f124eddd303825ade817791` / `346035869fb99edf548d9c9b4be6522de43f5095` |
| 本报告/索引的文档发布 SHA | 由正常提交产生，实际值及其远端结果保存在外部发布收据 |

项目、构建、临时目录和私有证据均在 `/mnt/d`，UID/GID 1000。正常本地合并 PR14/PR13，未合并 GitHub PR 或移动 main；版本仍为 `0.11.1-r3.3c-audit`、Store 410。父关系及原截点见
[N0–N4 输入说明](n0-n4-directed-work-20261102.md)，原记录保持原字节。

**实际实现与范围。** 从集成起点到 7bf3 共 30 个修改路径，唯一 `src/`/`include/`
生产变更为 `src/sse_replay.cpp`；未修改原 SQL、third_party、docs/handoff 或已有历史报告。
后继 b58f7b0 相对 7bf3 严格只改两条路径：Windows 工具测试夹具和新增
`docs/reports/n3-capture-path-validation-20261102.md`；122 个生产/build/catalog Git blob
比较不变，没有为此重建二进制。集成起点至 b58f7b0 合计 **31** 个变更路径；7bf3 时的
30 路径记录保持原范围。本报告/索引两条新文档路径的最终清单另由发布收据确认。
R-A3-01/R-A4-01 保持 `VERIFIED_FIXED_SCOPED`，没有重写 Store、Actor、CAS、预算或 claim。

| 卡 | 实际产出与边界 |
| --- | --- |
| N1 | 私有 `within` 改收 `std::string_view`，仅原失败分支建立 owning 错误消息；14 个调用点的条件、限额、取消、UNKNOWN 与错误语义保持。SSE 新方法覆盖长行/CRLF/chunk、原字节/hash、恰限额及超一字节。 |
| N2 产品回归 | async 两方法覆盖真实 Coordinator/M1/Store、adapter exception、跨池同资源后继、持久失败后 retained/fence、控制与 close；Store 两方法覆盖原 event bytes/hash/重放守卫及完整 command 限额/事务回滚。 |
| N2 工具 | 共享 GCC runner 保存完整原件，修正发布收尾、末尾身份独立采集、严格 JSON/XML guard、隔离失败回执和合法 CTest fail 投影；共享 GCC ASan 的 CI artifact 只上传经检查的公开投影。 |
| N3 | Windows workflow/runner 增加前后来源记录；新增 Win11 只读主机检查入口，限定原生 x64、workstation build 22000+、普通非管理员 token 和本机固定 NTFS。复用 4 组件 suite，未实现完整 Windows runtime。 |
| N4 | 异步 inbox、最小 DAG/版本审核票据、Windows 只读身份三卡；`DESIGN_PROPOSED / IMPLEMENTATION_NOT_STARTED`，验收 `PLANNED / NOT_RUN`，无新产品测试或迁移。 |

[N1 报告](n1-focused-diagnostics-20261102.md)及[索引](n1-focused-diagnostics-20261102-index.json)
保留受控反例：原 code08 容量方法在 18 秒原期限 Timeout；仅替换原 archive 一个 SSE
成员的派生 ELF 在相同业务条件、原期限通过，unittest 5.856 秒。这不是 fresh CMake 门禁。
新增 SSE 方法单项通过 0.265548 秒，属于行为覆盖，不能称为性能旧版红例；首次测试类型
比较编译错误 exit 1 保留，不能算产品反例。
[N2 常驻回归](n2-permanent-regressions-20261102.md)是已有正确行为的覆盖，不制造生产红例。
[独立定向 Release](n2-release-targeted-validation-20261102.md)输入为
`ce7798d7979801f5319ae7a79636dfdd002af2d2`，2/2 CTest、65 内部方法、exit 0。

工具原件缺失、收尾、解析、隔离失败回执和合法 fail 投影的真实反例依次为
1 方法/1 failure、7/7、5 方法/19 assertion failures、1/1、6 方法/8 assertion failures。
首个反例与最终 38 方法文件字节不同，仅反例方法及 `exercise_main` 的 AST 等价；后继
收尾/解析/隔离/fail 状态红绿各使用相同最终测试字节。子例不另算产品套件。原始记录见
[原件](n2-ctest-evidence-validation-20261102.md)、[收尾](n2-runner-finalization-validation-20261102.md)、
[解析](n2-public-parser-validation-20261102.md)、[隔离](n2-quarantine-failure-validation-20261102.md)、
[fail 状态](n2-ctest-failed-status-validation-20261102.md)。38/45/50/51/57 各轮不累加。

**本地完整验证。** 正式五行全部绑定 7bf3；原 92 名称/期限、严格参数和原件已交叉核对。
记录核对不是重新运行产品。所有套件都已有终态，证据收集完成不改变门禁失败。

| 运行 | 实际结果 | 退出 | CTest 秒数 |
| --- | --- | --- | ---: |
| GCC 13.3 Release，umask 0022 | 92/92；0 failed/timeout/skipped/notrun | 0 | 876.491067 |
| GCC 13.3 Release，umask 0002 | 92/92；0 failed/timeout/skipped/notrun | 0 | 763.984141 |
| Clang 18.1.3 Release，umask 0002 | 92/92；0 failed/timeout/skipped/notrun | 0 | 2145.512584 |
| GCC 13.3 Debug UBSan，umask 0002 | **66 Passed / 2 Failed / 24 Timeout**，0 skipped/notrun，共 92 | CTest 8；wrapper 1 | 6175.301555 |
| 共享 GCC 13.3 Debug ASan/LSan，umask 0002 | **56 Passed / 0 普通 Failed / 36 Timeout**，0 skipped/notrun，共 92 | CTest 8；shared 1；phase 1 | 7233.214670 |

本轮未执行独立的本机 **Make-ASan** 和 **Clang ASan**，状态为 **NOT_RUN**；
远端 Make-ASan、本机 Clang Release 与历史 Clang ASan 分开，不能互相代填。

三次 Release 的 console/JUnit/完整 LastTest 均为 92 个一致通过结果；内部 async/Store/SSE
方法分别 31/34/32。GCC 两种 umask 原件文件/inode 独立，0002 没有覆盖 0022。
UBSan CTest 从 04:06:19.647439 至 05:49:14.948866 UTC；wrapper 于 05:50:26.647683
结束、exit 1、reaped=true。原 92 名称/期限、串行、无 retry/期限覆盖；820 source 和
61 binary/build 前后清单一致。原件算术核对 exit 0 不改变产品失败，也没有重扫 ELF。

ASan 共享 runner 于 05:50:28 UTC 启动；build exit 0、1748.082902 秒，于 06:19:48.735825
结束。CTest 从 **06:20:14.434863 至 08:20:47.649469 UTC**，exit 8；共享 summary 于
08:21:17.980038 收尾为 NOT_COMPLETE、identities_unchanged=true。phase 于 08:21:34.859287
记录 exit 1，整个矩阵 session 81230 的实际工具退出也是 1；外层 PID 2986991 已不存在。
最终 collector 为 `collection_complete=true / gate_passed=false / COMPLETE / FAILED`，
不能将 collection_complete 或审计脚本 exit 0 写成产品通过。

ASan 的 console/JUnit/LastTest 恰有相同 92 名称，LastTestsFailed 恰列 36 项超时；
11 项原件的大小/hash、完整 LastTest footer、保留副本与原 build 日志均核对一致。
820 source/61 build 保存清单前后相同。原 Debug、ASan/LSan、严格警告、build 并发 2、
CTest 并发 1 和原期限均未改变；0 普通 Failed 不能证明没有内部失败或 sanitizer 问题。
`billing_baseline_e2e` 的实际终态为 **Timeout 900.02 秒**；不能继承 Release 或远端绿例。
`allocation_recovery_cli` 为 Passed 161.10 秒（原期限 180），`audit24_capacity_cli_smoke` 为 Passed 58.31 秒。

| UBSan 内部原方法标记 | 实际状态；不另算 CTest suite |
| --- | --- |
| Store | 34 START / 34 PASS / 0 FAIL，完整 RESULT 34 passed, 0 failed |
| async_coordinator | 30 START / 29 PASS，无 RESULT；新 N2 方法 14/15 均直接 PASS，旧方法 30 未完成、31 未开始；suite Timeout |
| sse_replay | 1 START / 0 PASS，无 RESULT；新 long-comment 方法序号 20 未开始；suite Timeout |

N2 新 async 两方法与 Store 序号 7/8 两方法均有直接 START/PASS，已另获非作者限定记录复核；
这不把 async suite 变为通过，也不把未开始的 SSE 新方法算作 UBSan 覆盖。失败原标签保持：

- `https_transport`：`len(peer.prefixes)` 实际 0、期望 1，outer Failed，19.07 秒。
- `r1_structured_cli_smoke`：内层原 50 秒 `subprocess.TimeoutExpired`，outer Failed，91.91 秒；outer 原期限 120 秒。
- `live_workflow`：先打印 `manager_limit` 条件为 False 的即时断言失败，随后 outer Timeout 300.03 秒，原期限 300 秒；此前失败不能被终态超时掩盖。

**ASan 的完整原方法核对。** 下列结论来自结束后的完整 LastTest，另有非作者限定核对；
不再用 06:52:39 的临时前缀快照代替终态。内部方法不另算 CTest suite。

| 套件 | 原方法证据 | 完整 suite 终态 |
| --- | --- | --- |
| Store | 27 START / 26 PASS，无 RESULT；新方法 7/8 直接 PASS；方法 27 仅 START，28–34 未开始 | Timeout 90.30 秒 |
| SSE | 1 START / 0 PASS，无 RESULT；新 long-comment 序号 20 未开始 | Timeout 90.39 秒 |
| async_coordinator | 31 START / 31 PASS，RESULT 31 passed, 0 failed；新方法 14/15 直接 PASS | Passed 154.35 秒（原期限 180） |
| HTTPS | 42 个方法均 ok，完整 Ran 42 / OK；partial-upload 方法原第 2209 行明确 ok | Passed 17.67 秒 |
| live_model_store_tls | 10 个方法均 ok，完整 Ran 10 / OK；capacity 原第 2255 行、large 原第 2257 行明确 ok | Passed 35.65 秒 |
| r1_structured_cli_smoke | 6 个方法均 ok，有完整 unittest 摘要 | Passed |
| live_workflow | 18 声明中有 10 个 ok，无终摘要；exhaustion 原第 2289 行 ok，其余 8 项状态未知 | Timeout 300.10 秒 |

ASan workflow 这段没有即时 FAIL/traceback；不得把 UBSan 的 `manager_limit` 条件 False
移植为 ASan 失败。ASan 的方法/套件绿例不会关闭 UBSan 原失败、历史超时或未查明根因，
也不构成 HTTPS 变化由 SSE 修复导致的因果证明。Store/SSE 的缺少完成标记不能定位内部
等待点、退出/LSan 阶段或性能根因。

| 已核对身份清单 | SHA-256 |
| --- | --- |
| 正式五行相同的 820 项 source 清单，前后相同 | `dfce8e4970a53589e9ee1bb38378f000724bd76282f9da6b086b5afcf628126a` |
| GCC Release 两种 umask 共用的 61 项 binary/build 清单，前后相同 | `4d83bac7b664607af1b9f853871795fe6c49cc03723796ad9d1a1860e31d627f` |
| Clang 的 61 项 binary/build 清单，前后相同 | `92244cfeb8a9c52b06ecc7bb975de26de490ae97cc63e2c2fd5def484461e61e` |
| UBSan 的 61 项 binary/build 清单，前后相同 | `d79504526ab2155bf9f8135e76ed273bd5e5c0ab17f57de384930ed392515d9f` |
| ASan 的 61 项 binary/build 清单，前后相同 | `24a47baa83a9e9313d5bb2e977de0b429ed3e1ec435845faed4c38568dbb2a0d` |

二进制不变结论限定于运行时前后清单和收尾收据；本次报告整理及原件审计没有再次扫描 ELF。
实际 build/CTest argv、cwd、起止时间和每层退出码均在下表索引的 step/execution 原件内，
包括 `cmake --build <实际 build> --parallel 2` 与 `ctest --test-dir <实际 build>
--output-on-failure --parallel 1 --no-tests=error --output-junit <实际证据路径>`。
各行使用各自原 build 和证据目录，没有重跑补写缺失结果。

工具集成输入必须区分：`integrated-selfchecks-01` 是
`3ce1e218731742d2a88fb5f08b77bb20f1aad835` 的 **51 方法**旧轮；
`integrated-selfchecks-02` 才是 **7bf3 的 57 方法**，原 stderr 为
`Ran 57 tests in 4.754s` / `OK`，外层 exit 0；作者报告中的 4.657 秒是另一轮。
最终 runner/test SHA-256 分别为
`8973416f725020cbd283e58310517820c75f40f59a1db9d99ffb7dc55e4717ac` /
`7c351fce2cc19ed249a219274e7fafc81270ad05a4764c5b105c93ff17151103`。
同一 7bf3 集成轮还通过即时日志 5/5（含真实 billing `__main__`）、transport observer 11/11、
Windows 证据工具 16/16、native observer 1 脚本 exit 0。私有分类器 12/12、
索引器 13/13 另计。以上均是工具验证，不能计入产品 92 或替代 billing E2E/Win11 实机。

**单次后续 workflow 诊断。** 全部正式进程结束后，只执行已准备的原
`test_model_exhaustion_escalates_original_rejected_submission` 一次；输入仍是 7bf3 与原
UBSan driver，未重建、未重试、未扩大 allowlist。真实 child/tool exit 1、1 method /
1 failure / 0 error，114.782890 秒，invoke_entered=1 / invoke_returned=1，进程已 reap，
原 source/driver 身份不变，未发生 outer timeout。首份返回报告显示 manager_calls=10，
小于原上限 12，elapsed=60812ms、stop_reason=deadline、provider_calls=0；原第 324 行
manager_limit 条件 False 的 traceback 仍保留。这只支持“首批在达到 manager cap 前
已到原 deadline”的限定解释，内部性能/阻塞根因仍 OPEN。
第二份返回报告有 1 个超出已审 allowlist 的 blocked code，projection_errors=1、
`diagnostic_complete=false / observation_complete=false`；不能恢复被屏蔽的 code 或将
整个观察标为完整，没有据此重跑或扩大 allowlist。正式失败和原期限保持不变。

**7bf3 远端验证。** 已审计的 [Push Linux](https://github.com/wmqfl861/wls861/actions/runs/41373884181)
与 [PR Linux](https://github.com/wmqfl861/wls861/actions/runs/41373887408) 各六 lane 均成功。
两组 Release 0022/0002、UBSan、GCC ASan/LSan、Clang 18 Release 各为 92/92；
Make-ASan 各为原 92 recipe 命令全部成功，不称新增 92 CTest。
Push 实际 checkout 为 7bf3；PR API head 同为 7bf3，实际 checkout 为 merge
`914a5de2331c6dc79a87d909f62105e43ea6e083`，双父为 base/head，tree 相同，均 attempt 1。
两次 fairness 的 A3 原基线业务次序红例 exit 1、候选绿例 exit 0，独立于 92 项计数。
[源码快照 run](https://github.com/wmqfl861/wls861/actions/runs/41373884150) 的 artifact
13110770276 共 820 文件，COMMIT/TREE、字节/hash/mode 与 7bf3 精确匹配。

两个 GCC 公开包各含 92 JUnit 投影、92 LastTest suite 投影块、9 个 exit 0 命令及
820 source/61 binary-build 身份；其他 lane 没有同等 binary artifact，不能补造。
完整私有 LastTest/JUnit 只确认在 hosted 临时 job 中生成，**未取得持久副本**，状态为
**NOT_PRESERVED_DURABLY**。原 job 日志/ZIP、公开投影和 hash 不能恢复被省略的字节；
`ctest_evidence_complete=true` 仅表示运行时捕获检查，不证明 job 销毁后仍可取得原件。

[Push Windows](https://github.com/wmqfl861/wls861/actions/runs/41373884133) 与
[PR Windows](https://github.com/wmqfl861/wls861/actions/runs/41373887418) 均在第 5 步工具自检
16 方法/1 failure/exit 1，第 6 步 native build 跳过；因此 7bf3 的远端 CI 整体未完成。
失败断言比较了短路径拼写与入口 resolved 路径。e23 修正
`e23f5d13385b3512d857be34b6003a782965979e` 只改测试夹具和新报告，使用真实
`path-spelling/..` 并期待 `resolve(strict=True)`；完整 argv、一次调用、30 秒及原守卫保留。
本机 Linux canonical-path 夹具红例为 1 assertion failure/exit 1，修正 1/1、完整 16/16/exit 0；
已有非作者限定静态/记录复核，未重新执行原 Windows job。
e23 已在正式收尾和上述单次诊断结束后正常 `cherry-pick -x`，产生实现提交
`b58f7b0e63ad68f67f124eddd303825ade817791`。本机 Linux 集成工具检查实际运行
`/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B
platform/native-foundation/tools/test_windows_ci_evidence.py`，08:29:46.638605–08:29:58.131978 UTC，
**16/16、exit 0、11.492969 秒、reaped=true**；821 项 source 前后相同。
这是 Linux 工具检查，不是原生 Windows 执行。最终发布 SHA 的真实 Windows CI 尚未发生，
必须在正常发布后以该 SHA 的实际 job 核验，不能继承旧 run 或本机工具绿例。
同 job image header `20261025.346.1` 与环境 `ImageVersion=20261026.347.1` 差异仍 OPEN，
不据此归因路径失败。真正 Win11 普通用户 4 suite/27 内部方法执行仍 NOT_RUN；
完整 Windows Store/VFS/进程/权限/cleanup 也未验收。

**保留的未闭环事项。** 当前新输入绿色结果不关闭下列历史记录或根因：

- N1 原 ASan partial-upload 诊断为 15 秒 Timeout，当时未复现 `prefixes=0`；本轮 UBSan 已直接记录 0/1 断言，根因仍 OPEN。GDB v1 stdout 污染仍为无效诊断。
- N1 large escaped output 原 18 秒 Timeout，当时未取得对应受控绿例；本轮 ASan live_model_store_tls 完整套件已绿，capacity/large 精确方法均有直接 ok，但不构成旧版同条件因果闭环。受控完整 SSE 90 秒超时的旧记录和根因不变。
- N1 selected CLI 26.361 秒 PASS 的历史 NOT_REPRODUCED 记录不变；本轮 UBSan 又出现原内层 50 秒失败。旧 async 诊断通过不替代旧矩阵 64 Passed/2 Failed/26 Timeout，也不改写本轮 66/2/24；根因均 OPEN。
- 历史 Clang ASan 等问题保持 OPEN，`historical_clang_asan_failures_closed=false`；当前 Clang 是 Release。
- Windows 镜像来源差异、真实 Win11/完整安全验收及 hosted 完整私有原件持久性缺口保持原边界。

WLS 产品真实供应商调用记录为 **0 次**。编码工具单列：ZCode 第一会话 240.016 秒超时，
无修改，usage/请求数/费用未知；第二会话 exit 0，报告 4 请求、91,366 input、1,362 output、
92,728 total、74,624 cache-read tokens，货币费用未知。不得写成编码工具零调用、总请求 4 或免费。

**资源、依赖与复核范围。** 资源审查只分析截至 05:27:33 UTC 的已有 2628 条遥测固定前缀，
含不同阶段可见 CPU/PSI/cgroup 观测；不覆盖 UBSan 全程或 ASan，不能用于环境因果归因。
依赖审查直接确认本地 Boost.JSON 在项目 TU 内从系统头编译并带 ASan；7 个具名包版本
记录相同，但远端实际 compile/include/link 正文未观测，不能证明有效依赖相同或差异，
也不能归因于 Boost、预编译库、隐藏优化或编译器。两份限定收据见下表。
报告整理者 `/root/delivery_report` 阅读最终小收据、受控解释、索引及冻结参考报告，
只准备交付文档和只读远端审计入口；没有执行产品或二次重跑矩阵。ASan/UBSan 的完整
92 项原件/算术核对来自 N2 工具和 collector 作者 `/root/concurrency_fix`，不能自称
非作者批准；另由 `/root/phase_observation_review` 对新增方法、未开始方法及原失败
标签作非作者限定交叉复核。ASan 的此项复核使用 11 个冻结 7bf3 源码 blob 和完整
LastTest，未重做全部配置/二进制审计。它没有批准实现或替主协调者完成正式验收。
实现层面另有 N2 8ead、后继 6f20 和 e23 的非作者静态复核；6f20 另含 3 个合成工具
方法复跑，均不是产品重跑。Release 原件核对、静态工程复核和产品执行分开登记。

**原件定位与冻结。** 下列私有定位符相对工作根
`/mnt/d/codex-workspace/wls861-n0-n4-20261102-0k9_2wlf`；它们不是仓库文件或公开下载链接。
这里只发布定位符、hash 和结论，不复制原 job、doctor/config、环境对象、请求/模型正文。

| 私有定位符 | SHA-256 |
| --- | --- |
| `evidence/review-three-local-complete-01/receipt.md` | `2880e3d2e6ef74e9342eaca3c249c6205d8370a95b77bbbf254e626959f40e0a` |
| `evidence/review-three-local-complete-01/audit.json` | `6ed96cfe8c8abcf4d94a1eb750757a0bd5d4c4af112f8149d89b70edc3ee48f6` |
| `evidence/integrated-selfchecks-02/summary.json` | `79854ddfd5227ae220fe0b9a5dbff35bfffc87767c65d3027f5da57aea128585` |
| `evidence/integrated-selfchecks-02/gcc-runner.stderr` | `b0374f185f1eb9eab0eb1d6036a00b7e6f648f7dc497026bea6a8ee5633a6b7c` |
| `evidence/integrated-selfchecks-02/source-before.json`；`source-after.json` 同字节 | `1fb1dde39aafaec59268257bc806a71af1b3c069c1fa559f5483a8b109465a6e` |
| `evidence/remote-final-audit-01/PRODUCT7BF-FINAL-AUDIT.json` | `bb592866d8c7c44e003ee0d9971871483c1004b2e57dc862f1896927a817644e` |
| `evidence/remote-final-audit-01/PRODUCT7BF-FINAL-SHA256SUMS.json`，919 原冻结文件 | `378aee28c27be6f18f6bd737dd38e5bea5bf6a8ec709efdb16a8dec081d75871` |
| `evidence/remote-final-audit-01/PRODUCT7BF-PRIVATE-LOG-DURABILITY-CLARIFICATION-v1.md` | `56c2309eb7ab5bff3de931df09794c533a90751a365d4a80e0869d12973c60ad` |
| `evidence/review-n3-capture-canonical-path-e23f/receipt.md` | `3acded3c3b251dd2a2ccfee6aec68d2184af6f653b8ef68365ed6e69b4ccb0f6` |
| `evidence/review-ubsan-complete-01/receipt.md`，作者原件/算术核对 | `c3b290924e28035c8048b29203eebbee17381ff524b2ced204a869054086d21d` |
| `evidence/review-ubsan-methods-nonauthor-01/receipt.md`，非作者限定方法/失败标签复核 | `d52adb18b79b29e27a079e69815873ebdcd2bba02d953cead8c4a5b9a019c738` |
| `evidence/resource-observation-review-01/receipt.md` | `4be32cab9efaa861216fcbaa858c38651607415e33982ffd4ca3098b34db47a6` |
| `evidence/dependency-observation-review-01/receipt.md` | `1b91860a2f3c314a4c9a1f05ef0827eb8c3f30c0ee37f56cc87d893fa4a9963d` |
| `evidence/review-n2-final-8eadf8d/receipt.md`，静态工程复核 | `50a8a7ea9ccb84bc39583a63af069770114d549f5cbce59d30592e8a33760d8f` |
| `evidence/review-ctest-status-fail-6f20bf8/receipt.md`，后继限定复核 | `2190f001ad57e6bf30c5e62eb3f4e7044688c468f0937d2a4e423ac71754f3fc` |
| `candidate/build-gcc-asan-20261102T055028Z-dad5f2cb/private-evidence/build.step.json` | `df321a9dbaf4480ea4da9d4fee4b6c9135d0f54dd2b158bf746e17cf4497c42f` |
| `evidence/n1-asan-store-stage-observation-01/receipt.md`，固定前缀快照，非 phase 终态 | `64db8988871313d8b1c5ce40a0b0d20239040276b380704168a14fb5805c4338` |
| `evidence/review-asan-complete-01/receipt.md`，ASan 作者完整原件/算术/身份审计 | `8ad2f8829b0ad5c9fafc042e348f6d508a7989b167fbff2fa51608bc38e45897` |
| `evidence/review-asan-complete-01/audit.json`，ASan 作者逐项审计 | `215fb0c5ad0f248ec12f71b6a6e05ff44e814d712db939704eb1c24f1b3de9d7` |
| `evidence/review-asan-methods-nonauthor-01/receipt.md`，ASan 非作者限定方法审查 | `d4373ca445aa20e58de30f8c69180d86c01af6db9713c96c9c2e4e162202cc3b` |
| `evidence/review-asan-methods-nonauthor-01/audit.json`，ASan 非作者逐项方法审计 | `fb89516458e058c4a75d9927478f5d28541fdc5c315726d207f828b034cdfeff` |
| `evidence/matrix-index-final-01.json`，正式五行 collector 终态 | `c37e5be139160d96bcef9578b2d8ee102f0902b9740ebca3e5c1cff461c16f28` |
| `evidence/final-matrix-parent-exit-01/observed-tool-result.json`，矩阵真实父进程退出 | `4cd62ef9a612b34a495927439352748ff87ba11fed35e1c85456eeffbb1b36c5` |
| `evidence/n1-current-workflow-probe-plan-01/runs/ubsan-exhaustion-one/execution.json`，单诊断真实执行 | `72ec8b929328bd80f6a009d13f46e674990e5b8358e85ff9c3741f0527843d35` |
| `evidence/n1-current-workflow-probe-plan-01/post-run-interpretation-01.json`，单诊断限定解释 | `798360e616714eddbe86f7385a092e5f216cd56a9d63d4567672dc3ef7b42990` |
| `evidence/integrated-windows-fixture-01/summary.json`，b58 集成 16 工具方法 | `e001adafea62cde2c4826676420c1fff67b67fdc80f7c6b6ca895953868e306b` |
| `evidence/publication-final-01/successor-scope-verification.json`，后继两路径及 122 blob 比较 | `1cb39f68f046153278df3f8bae8b6b9cf5e00692a15b19d13f8509634153d9eb` |
| `evidence/publication-final-01/implementation-prepublication-review.json`，b58 实现 diff/check/status/stat | `cea50727fb8870b6ec546f7d32218b0756d24f6b2ce1a32ff1b5d5d9d3bddc4d` |
| `evidence/publication-final-01/prepublication-remote-refs.json`，发布前远端 ref 读回 | `fc836a8978cdb4f1bfcf896b508e77abc9cf4f065c80a8fd071452bd5a56cbd0` |

**发布状态与剩余决策。** 本地正式矩阵、父进程退出、原件/方法审计、一次原方法诊断、
e23 正常整合及其适用 16 方法检查均已有真实终态。本轮适用实现和 N4 设计资料已完成；
完整本地门禁仍失败，根因未闭环，不作产品验收。b58 发布前 `git diff --check`、
status、stat、完整 diff 及路径检查退出均为 0；生产差异仅 SSE，历史保护路径差异为 0。
08:32 的 fetch/ref 读回 main 仍为原 a6b8559、PR13/14 head 仍为上述固定值，PR15 远端
仍为 7bf3。PR13/14 仍 draft/open/unmerged，PR15 尚无正式 review；main 未被移动。

本报告/索引作为新的文档输入由本地执行根代理复核后正常提交；最后 source 清单、无秘密/
临时数据库/构建产物审查、提交 SHA、push/ref 读回、实际 Push/PR CI 和源码快照由该次
发布的外部收据登记。发布前再核验 main；如变化，停止发布并交主协调者决定整合。
不能把 b58 的两路径等价检查或本报告的收集完成重标为一次最终 SHA 的产品矩阵。
真正 Win11 普通用户验收、完整 Windows runtime、安全边界实机验收仍未执行；
N4 实现尚未授权。本批不做新的真实模型调用、付费接入或无关重构。

| 六状态 | 本记录截点的真实范围 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | **true**：N0–N3 本轮范围及 N4 设计资料；实现 HEAD 为 b58f7b0。不是完整 Windows runtime 或产品验收 |
| LOCAL_VALIDATION_COMPLETE | **false**：正式五行已收集完成，但 UBSan 66/2/24、ASan 56/0/36；整体 VALIDATION_BLOCKED |
| PUSH_COMPLETE | 7bf3 历史输入为 true；b58 及文档发布尚未推送，其完成状态由外部最终 ref 收据确定 |
| REMOTE_CI_COMPLETE | 7bf3 为 false（Windows 两 run 失败）；最终发布 SHA 尚无实际 CI，由外部终态收据确定 |
| PR_REVIEW_COMPLETE | **false**：模块工程复核不代签主协调者/PR 正式接受 |
| MERGED | **false**：PR15 保持 draft，本轮不执行合并 |

[N4 三卡](../implementation/next-cycle-20261102/README.md)仍需协调者采纳事件关联、DAG/审核与预算及 Windows 政策；设计完成不等于后继授权。本批代码/证据交协调者审核，历史 OPEN 保持可追溯。
