# G2 首次真实运行

日期：2026-11-15 至 11-16。文件冒烟已交付；原三交付物请求的导出 UNKNOWN 已按 D-31 由当前 main 重放恢复，成果检查 12/12 通过，模型调用仍为 29。严格原 metadata 保留检查为 FAIL：只有 SHM mtime 变化，原字节/hash/身份不变，原 `pass=false` 检查结果保留。当前 main 的新请求 U1 与分层思考等级 U2 尚待 R2/R3 复验。11-15 的 41 次真实调用、超时和首次导出失败原样保留；D-32 已解除全局暂停。实现 0.12.0、Store 410，G2 出口尚未通过。

owner 已授权指定 Responses 中转、`gpt-6.1-sol` 与测试凭据，并取消金额门禁；D-35 增加其他思考等级授权，R2 仍全部用 `max`，R3 在探针确认后分层。新请求冻结 `observe-only-v1`；D-34 的 G2 余下额度为 250 次调用、500 万输入 token，11-15 的 41 次另列历史。单次期限 180 秒，自动重试为 0；R2 上限 60 次/150 万输入/25 分钟，R3 上限 80 次/200 万输入/40 分钟，两者输出上限均为 8 万。费用未知不等于免费，无需补价格表或重新提供密钥。

| 实际请求 | 结果与核验范围 | 调用（D/TL/W）；输入/输出 token；时长 |
| --- | --- | --- |
| 独立协议探针 | HTTP 200 / completed；模型 `gpt-6.1-sol`；返回 reasoning 与 function_call，工具执行 0，未经过 Store，不计闭环 | 1；255/172（reasoning 133）；8.45 秒 |
| WLS 冒烟 01 | Dispatcher 首次请求 HTTP 400：`Unsupported parameter: background`；`needs_accounting`；工具 0、成果 0 | 1（1/0/0）；未知/未知；5.52 秒 |
| WLS 冒烟 02 | `DELIVERED/quiescent`；固定 `write-hello/hello.md` 三行内容检查 PASS；1 个操作 SUCCEEDED、1 个任务 ACCEPTED | 9（2/3/4）；73,001/3,299；167.12 秒 |
| G2-4 三任务 01 | Dispatcher 首次请求达 60 秒期限，无 HTTP 响应；`needs_accounting`；任务、操作、成果均为 0 | 1（1/0/0）；未知/未知；61.12 秒 |
| G2-4 三任务 02 | Store 为 `DELIVERED`，但 `output_blocked`、`output_path=null`；Root `unknown_outcome`，成果发布未确认 | 29（2/3/24）；575,141/18,431；790.51 秒 |
| 11-16 R0：同一三任务 02 导出重放 | `DELIVERED/completed_replay`；原 `output/` 已导出，成果 12/12 PASS；严格保留检查 FAIL 仅 SHM mtime 变化，详情见下 | 新调用 0；原 29（2/3/24）与原用量不变；2.525 秒 |

按 Dispatcher / TeamLead / Worker 顺序，冒烟 02 输入/输出 token 为 14,622/1,006、30,323/1,356、28,056/937，平均请求体为 32,679.5、40,304、33,635.75 字节；三任务 02 为 31,194/4,887、76,857/7,098、467,090/6,446，平均请求体为 59,026、88,734、96,639.33 字节。缓存读分别为 0、7,680 token，缓存写均为 0；全请求按时间排序的 Worker 第 3..24 次缓存率为 7,680/446,197＝**1.7212%**（22 次调用，最小单次 0）。两次账目均 ready，总费用均 `null`，未知费用尝试分别为 9、29。冒烟保留 1 次 Worker `missing_leaf` 纠正，PASS 仅覆盖固定三行文件。

三任务 02 平均请求段字节（序列化 JSON value；other 含封装与分隔符，四舍五入）：

| 角色 | tools | system | contract | history | current_state | other |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Dispatcher | 13,174 | 6,319 | 2,179 | 0 | 37,006.50 | 347.50 |
| TeamLead | 11,141 | 5,912 | 19,026.67 | 0 | 52,303.33 | 351 |
| Worker | 10,775 | 6,111 | 16,066 | 57,995.25 | 4,977 | 715.08 |

三任务 01 的 TLS 连接及验证用时 0.401 秒，传输层报告上传 26,909 字节；HTTP 状态、响应头及响应体字节均为 0，用量和费用未知、账目未就绪，网络连接不能证明模型处理成功，超时根因未定。三任务 02 新建请求，只将实跑单次期限改为 120 秒，总墙钟 900 秒、40 次调用和原 token 上限不变，测试期限未改。所有失败原件保留，旧 attempt 未重发。

三任务 02 将三个需求合为 1 个 team、1 个 ACCEPTED task，5 次 Worker `fs_write` 均 SUCCEEDED。29/29 模型响应为 HTTP 200 / completed / usable；provider incomplete、输出到限、body/header 及工具 stdout/stderr 截断均为 0。当次 Root 报 `output intent publication incomplete`，首次 export 记录为 UNKNOWN。最终独立审计确认包内 13 文件的哈希/长度及 14 项数据库绑定全部通过；从不可变包复制出的私有投影运行固定检查 12/12 PASS，此次副本审计不证明原发布或用户收取成功。FAQ 两次写入 1,159→1,103 字节，第 3 答 78→22 汉字，其余 4 答不变，修订链绑定最终包；当次审计的原 DB/WAL/SHM 与包文件均未变。

Root 输出发布复用业务 Workspace，扫描兄弟数据库时受到 16 MiB 文件上限限制；旧 ELF 加 17 MiB 合成 fixture 的单方法回归已实测 RED。[修复 PR #111](https://github.com/wmqfl861/wls861/pull/111) 将发布检查与业务扫描分离，补齐收据与子目录重放的 fsync，保留副作用结果未知时的 UNKNOWN。独立最终静态审核 PASS；GCC 13.3 Release、普通用户、umask 0002 下 `workspace` / `delivery` / `run_cli_e2e` / `g2_outputs` **4/4 通过**，105.24 秒，0 失败/超时/跳过，run CLI 11 个方法通过，注册总数仍为 90；本机 LastTest/JUnit 完整保留。#111 已合并到 main `9b7f700`，[PR 三核心检查](https://github.com/wmqfl861/wls861/actions/runs/43737964662)及源码检查成功。#111 与 [#112](https://github.com/wmqfl861/wls861/pull/112) 合并后的 main `a83bdd8` [完整 CI](https://github.com/wmqfl861/wls861/actions/runs/43739309720)已通过：**450/450 CTest**，Make **60+25** 条，失败/跳过/未运行均为 0；JUnit/LastTest artifacts 不可用，保留 CI 原日志。本机合成通过不构成原真实请求复验。

端点适配已合并 [元数据兼容 #108](https://github.com/wmqfl861/wls861/pull/108)、[金额观察策略 #109](https://github.com/wmqfl861/wls861/pull/109) 和显式省略 `background` 的 [#110](https://github.com/wmqfl861/wls861/pull/110)；#110 本机专项 3/3 通过。实跑基线 main `cc65d9e` 的[完整 CI](https://github.com/wmqfl861/wls861/actions/runs/43724936562)九个构建/测试作业及[源码验证](https://github.com/wmqfl861/wls861/actions/runs/43724936571)通过：三核心各 90/90、ASan 与 UBSan 各 45+45、Make 60+25，无测试跳过；最大 ASan 套件占比 65.21/180 秒＝36.23%。JUnit/LastTest artifacts 为 UNAVAILABLE，CI 原日志留存。

[E-2 PR #113](https://github.com/wmqfl861/wls861/pull/113) 已合并到 main `900a879`。用途投影在本机对同一组 29 份捕获请求作离线编码测量（Dispatcher / TeamLead / Worker 各 2 / 3 / 24 份），按完整序列化 `current_state` 项计字节、包含外层封装：Dispatcher 均值 **37,006.50→18,336.50**（减少 **50.45%**），TeamLead **52,303.33→21,038.67**（减少 **59.78%**），Worker **4,977→2,436.375**（减少 **51.05%**），三个角色均达到状态项减半门禁。无 marker 的 a83 路径及仅有 `compact_model_state:true` 的 e81 路径各 **29/29** 旧 wire hash 全等，原样本未改。此项为本机离线测量；当时累计真实发送 41 次，未执行原发布重放或角色对照。

本机定向首轮 13 项为 **12 PASS / 1 夹具失败**（253.28 秒，0 超时/跳过/未运行），失败原件保留；原因为新增纯 codec 合成请求进入旧费用策略 Store 夹具时漏填正数费用预留。仅在该 Store 夹具补齐预留后，`live_model_store` 与 `g2_rehearsal` **2/2 PASS**（57.89 秒，0 失败/超时/跳过/未运行），产品 ELF 及四个产品源文件与首轮相同，本机 LastTest/JUnit 完整。D28 合成缓存固定序列在 Responses、Chat 均为 35 次调用、DELIVERED、3 个 Worker、11 个合格样本；冻结门槛分别为 `0.5633509583608723`、`0.5506825938566553`，确认运行均通过，原 G1 `>0.70` 门槛及固定序列/期限未改。CTest 注册总数仍为 90；首轮失败记录未改报为同轮 13/13 全绿。

最终 head `f773812` 的 [PR 三核心检查](https://github.com/wmqfl861/wls861/actions/runs/43752880351) **270/270 通过**，失败/跳过/未运行均为 0；[源码归档](https://github.com/wmqfl861/wls861/actions/runs/43752880296) **744 文件**与提交树逐字节一致。合并后的 main `900a879` 验证见[完整矩阵](https://github.com/wmqfl861/wls861/actions/runs/43753899472)及[源码归档](https://github.com/wmqfl861/wls861/actions/runs/43753899551)。

随后 main `900a879` 的 [Make-ASan（1/2）](https://github.com/wmqfl861/wls861/actions/runs/43753899472/job/132933710502) 检出新增 Dispatcher 测试夹具在同一 JSON 对象插入时引用失效（`heap-use-after-free`）；修复为任何插入前分别复制 task/instruction 两个值，全部断言及原失败日志保留。 修复后本机 GCC 13.3 Debug 的 CMake ASan/LSan `r1_structured_lean` 完整注册项 **1/1 PASS**（13 个方法全部通过，84.23 秒，低于原 180 秒期限；`halt_on_error=1:detect_leaks=1`），LastTest/JUnit 已保留；此结果不代表 Make-ASan 全门禁通过。

本机旧轮次保留 89/90（专项 4/4，加其余 85/86），失败为 `doctor_diagnostics_cli` 布局检查；同一 ELF 在受支持布局下的 13 个方法通过，不将原轮次改报为全绿。已记录的凭据模式扫描为原 132 文件和新增审计 36 文件，均 0 匹配。长期状态统一见 [STATUS](../STATUS.md)。

本机私有证据根：`/mnt/d/codex-workspace/wls861-g2-20261115-a78u4b6i/`。原实跑在 `evidence/protocol-probe-01`、`evidence/real-*` 及对应运行目录；最终审计在 `evidence/real-three-02/package-audit-oid7cqxk`，修复验证在 `evidence/export-final-release`，基线 CI 在 `evidence/ci/maincc65d9e`。11-16 恢复线证据在 `/mnt/d/codex-workspace/wls861-g2-resume-20261116-k48kol3a/evidence/real-line/`。公开仓库只保留本页摘要和 CI/PR 链接，不提交凭据、数据库、长日志或原始模型响应。

11-16 R0 只读现场：`output-intent.json`、`output-root.json`、`output/` 及 `.wls-run-receipt-*` 均不存在；原数据库 30,060,544 字节，WAL 为空。完整备份 `r0-backup-v2ypjf_q` 含 23 个文件、6 个目录、0 个符号链接，共 30,561,124 字节；每个文件的 SHA-256、类型、mode 和 mtime 核对通过，备份阶段原请求与原配置前后不变。只读备份数据库确认 DELIVERED、唯一 CONFIRMED 交付和 29 次模型尝试。

R0 使用 main `8d44256` 的 GCC 13.3 Release ELF（SHA-256 `0a6a538349487f93e599fcd26a8cc40e983badda731525fa1c3ca17ec345eca2`），只替换原命令的程序路径，原配置及其余参数不变，未提供密钥。一次重放退出 0、耗时 2.525 秒；`stop_reason=completed_replay`、原请求 `output_path` 非空、调用仍 29，`check_g2_outputs.py … three` 12/12 PASS，D-31 四项导出标准满足，原导出 UNKNOWN 已恢复。准入、路径身份、回执、包和原字节守卫继续生效。

严格保留检查仍为 **FAIL**，不改写 `r0-replay-qiel64xz/result.json` 的 `pass=false`：原 `control.sqlite-shm` 只有 `mtime_ns` 从 `1794757036440118593` 变为 `1794830980908696960`，32,768 字节逐字节相同，SHA-256 `fd4c9fda9cd3f9ae7c962b0ddf37232294d55580e1aa165aa06129b8549389eb`、mode、dev/ino 均不变。原数据库、WAL、writer-lock、交付包与其他原文件无变化，原配置无变化，新增路径仅限两份导出回执及 `output/`。只读差异核对见 `r0-post-run-facts.json`；原请求、备份及该检查结果保留到 2026-12-16，不还原 mtime，也不再次重放。产品导出成功不等于所有保留检查通过。
