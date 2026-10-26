# R2.3 模型 AMEND：实现与验证报告

实现版本：0.8.2-r2.3；目标蓝图：v0.4.1；设计：[ADR-073](../adr/073-model-amendment-dispositions.md)。
本报告承接[执行中断检查点](r2-model-amendment-checkpoint.md)，记录恢复后实际完成的验证。
原 R2.2 联合验证缺档已用精确基线重新执行并补齐，见[基线报告](r2-integration-validation.md)。

## 代码与被测输入

- 原始基线：`c975be3e7d30c1208a0a2c19494e2a186c7f77ca`。
- 本地实现提交：`699adea34a7ef4dea9ec5f7b1212cda8d222dc1f`。
- 已发布实现提交：[4737b6b0585d9828579ca244d96101bbdb104076](https://github.com/wmqfl861/wls861/commit/4737b6b0585d9828579ca244d96101bbdb104076)。
- 两者源码树均为 `aa6e12ddd127f2ac54bc94b353563f2345dbffa5`。GitHub 提交含已发布检查点父链，提交元数据和 SHA 与本地不同；不是以短 SHA 相似代替核对。
- 从本地实现提交 `git archive` 读取的 231 个执行输入，与实际测试快照逐字节一致。
- [执行输入清单](r2-model-amendment-inputs.sha256) SHA-256：`34a65a116faadbe43988af7f0762ecf143d3c18a49c6c0e0337ccdb799165e2e`。

四种 C++ 构建完成后，唯一执行输入变化是旧 Python CLI 测试的预期修正，已同步快照并重新冻结。
源码、二进制 SHA、命令、UID、umask、退出码、耗时及原日志见下方机器记录和证据包。

## 本次行为变化

工作流 Dispatcher 在没有非空宿主 dispositions 时，捕获完整原 AMEND 目标，并只声明当前阶段允许的工具。
`submit_amendment_plan` 原子提交当前计划与非空节点决定；`resolve_amendment` 在同一计划补办遗漏节点。
模型不能注入身份、版本、CAS 或执行代次，也不能选择捕获集合外的任务。

首次计划在最终事务内核对全部既有任务；后补决定只核对所选的捕获待办任务。
请求 CAS 与任务 CAS、团队身份、执行代次共同生效，避免请求行未变化却接受过期任务判断。
遗漏任务继续暂停并保留精确 inbox 项；keep 仍需新 TeamLead 说明和 Worker ack。
共享返工额度、UNKNOWN、原说明/成果、实际或未知调用用量、精确反馈重放和兄弟请求隔离保持。

原宿主 dispositions 路径、旧 schema404/单任务运行器继续兼容。未修改已发布 SQL 或 vendor 字节。
具体接口和限制见[实现契约](../implementation/R2-model-amendments.md)。

## 普通用户下的本地门禁

所有正式测试使用 UID/GID 65534。GCC 13.3.0 Ubuntu 包 `13.3.0-6ubuntu2~24.04.1`、
Boost 1.83、SQLite 3.45.1、OpenSSL 3.0.13、CMake 3.28.3、Ninja 1.11.1、Make 4.3；
Python 3.12.14 和固定 jsonschema 4.10.3。测试没有真实供应商调用。

| 门禁 | 设置 | 实际结果 | 耗时 |
| --- | --- | --- | --- |
| GCC Release | umask 0002，CTest -j2 | 54/54，exit 0 | 81.906 秒 |
| GCC Release | umask 0022，CTest -j2 | 54/54，exit 0 | 86.608 秒 |
| GCC UBSan | umask 0002，CTest -j2，halt_on_error | 54/54，exit 0，无 UBSan 诊断 | 370.250 秒 |
| 严格 Make | umask 0002，-j2，用户 CPPFLAGS | 54 个套件命令均通过，exit 0 | 159.502 秒 |
| GCC ASan/LSan 首轮 | umask 0002，CTest -j2，detect_leaks=1 | 53/54；acceptance_runtime 在原 90 秒限制超时，exit 8 | 666.854 秒 |
| 同一 ASan 组隔离复核 | 无其他门禁，-j1，原 90 秒限制 | 仍超时，exit 8 | 90.192 秒 |
| 同一 ASan 二进制有界诊断 | PTY 逐项记录；不计原门禁 | 19/19 断言通过，exit 0，无 ASan/LSan 诊断 | 91.073 秒 |

严格 Make 保留 `CPPFLAGS=-DWLS_R23_USER_CPPFLAGS=1` 和原 warnings-as-errors 要求。
四项通过的全量日志分别记录 428 项 S1 C++、143 项 S1 CLI、52 项 M1 C++、3 项 M1 demo、
38 项 Python 参考契约，另有两个完整性检查组。共 54 个测试组；不同层次不相加为产品验收成绩。

ASan 诊断最后一项在 90.900 秒输出 PASS，峰值 RSS 1,744,156 KiB。
隔离运行仍超时，因此不能仅归因于并行负载；诊断成功也不能改写为原 90 秒 CTest 门禁成功。
本地 ASan 时间门禁明确未通过，源码、断言、原超时和 sanitizer 选项均未放宽；不重复运行以碰运气。
最终发布还须查看同一执行输入的远端六项 CI 结果，独立记录其环境与通过情况。

### 可重复入口

```sh
cmake -S SOURCE -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/tmp/wls861-validation-venv/bin/python
cmake --build BUILD --parallel 2
umask 0002
ctest --test-dir BUILD --output-on-failure -V -j2
make -j2 test BUILD_DIR=BUILD_MAKE CPPFLAGS=-DWLS_R23_USER_CPPFLAGS=1 \
  PYTHON=/tmp/wls861-validation-venv/bin/python
```

UBSan/ASan 分别用 `-DWLS_ENABLE_UBSAN=ON` / `-DWLS_ENABLE_ASAN=ON` 配置独立 Debug 构建。
实际绝对路径和逐项环境记录在证据中；必须以普通用户运行，不以 root 权限跳过文件防护。

## 红绿证据与修正历史

新增九项 CLI 测试先在精确 c975be3 基线执行：7 失败、2 项兼容/隔离场景通过，3.497 秒。
原红日志存在，但原同名进程 JSON 不存在；补充观测文件明确由原日志解析，没有伪造退出码或启动时间。
候选新增 C++ 14/14、新增 CLI 9/9 全部通过，覆盖双输入格式、跨进程续办、部分决定、
任务独立 CAS、原目标防篡改、精确重放、不清 UNKNOWN、预算以及完整兄弟请求隔离。

首次全量 Release/UBSan 都是 53/54：旧 R2.1 测试仍要求 AMEND 后零次模型调用。
修正后该测试要求恰好一次 captured amendment 调用，并拒绝旧 change_task 工具；还核对原
100/20 用量与预算增量、原任务语义、计划、成果字节、精确 AMEND inbox 和工作文件全部保持。
修正后的旧 CLI 套件 10/10 通过。此变化没有放宽业务门禁。

首次新增 C++ 测试有 JSON/string 比较的编译错误，已修正比较表达式，保留失败构建日志。
早期红测试辅助模块加载错误单独保留，没有计入功能红阶段。
原全量失败、修正后成功及两次 ASan 超时均保存，不以最后一次输出覆盖历史。

## 可核对证据

- [机器结果](r2-model-amendment-results.json)：本地正式门禁、ASan 诊断与未完成项分开记录。
- [证据包清单](r2-model-amendment-evidence.json)：每个原文件的大小和 SHA-256。
- [原日志和环境证据包](r2-model-amendment-evidence.tar.gz)：60 个文件，含原红阶段、初始失败、最终测试日志、环境和提交输入校验。
- 证据包 SHA-256：`5b77fc7d6cb21c705748cf62c8171cef09dee49aa941acc920eace6a5a75364c`。

## 远端与完成边界

实现已推送到 `work/r2-model-amend`。实际实现提交 4737b6b 的原运行
[40286077662](https://github.com/wmqfl861/wls861/actions/runs/40286077662) 已结束：
五项成功，Make-ASan 取消，整体结论仍为 cancelled。源码快照
[40286077660](https://github.com/wmqfl861/wls861/actions/runs/40286077660) 成功。

| 原实现 CI | 实际结果 | 原日志中的完整 CTest 用时 |
| --- | --- | --- |
| strict / umask 0022 | 54/54，success | 227.89 秒 |
| strict / umask 0002 | 54/54，success | 144.14 秒 |
| Clang-18 | 54/54，success | 145.22 秒 |
| UBSan | 54/54，success | 663.04 秒 |
| ASan/LSan | 54/54，success；acceptance_runtime 79.96 秒 | 1060.11 秒 |
| Make-ASan | cancelled；未完成全套 | 不计为完整通过 |

原 Make-ASan 最后完成 r2_recovery_cli_smoke 六项，随后开始 r2_integration_cli_smoke，
约三秒后取消；后续 AMEND 和 review CLI 未运行完。日志未见 sanitizer 诊断，但这不能替代
完整 Make 门禁。远端 ASan 的真实成功与本地原 90 秒超时分别成立，保留各自记录。

文档及 job 总预算修正提交
[b5968ad524b6b29332db8f9a6c189abf0e20dfd6](https://github.com/wmqfl861/wls861/commit/b5968ad524b6b29332db8f9a6c189abf0e20dfd6)
保留完全相同的 231 个执行输入，其完整后续运行是
[40288012552](https://github.com/wmqfl861/wls861/actions/runs/40288012552)：**六路全部成功**。
同 SHA 的[源码快照](https://github.com/wmqfl861/wls861/actions/runs/40288012691)也成功。

| 后续 b5968ad CI | 实际结果 | 完整测试 / 作业用时 |
| --- | --- | --- |
| strict / umask 0022 | 54/54，success | CTest 143.97 秒 |
| strict / umask 0002 | 54/54，success | CTest 153.73 秒 |
| Clang-18 | 54/54，success | CTest 149.17 秒 |
| UBSan | 54/54，success | CTest 645.12 秒 |
| ASan/LSan | 54/54，success；acceptance_runtime 85.29 秒 | CTest 1229.76 秒；作业 22 分 42 秒 |
| Make-ASan | 54 个套件命令完整通过，保留用户 CPPFLAGS | 作业 21 分 51 秒 |

各原日志都核对了实际 checkout SHA、普通用户门禁和 sanitizer 选项。没有内存/泄漏/UBSan
诊断。Make 日志计数仍为 428 项 S1 C++、143 项 S1 CLI、52 项 M1 C++、3 项 demo、
38 项参考契约，另有两个完整性检查组。后续成功没有改写原 run 的 cancelled 或本地超时。

远端[机器记录](r2-model-amendment-remote-ci.json)分别保留原实现取消、设计基线比较和后续六路成功；
顶层原运行的 matrix_success_verified 仍为 false，后续成功在 followup_commit_ci 中。
[远端证据清单](r2-model-amendment-remote-evidence.json)与[原始日志包](r2-model-amendment-remote-evidence.tar.gz)
共 16 个文件，含 15 份原始 job 日志和冻结机器记录；包 SHA-256：
`c5762fe7d0f273edca3dcb1ea15ea885799f02e0df199f48810326c22a897e46`。

设计提交 b4977fa 的 CI 不属于新实现证据。最终文档提交或主分支同步不得改变未复验的执行输入。

### CI 工作流总预算

只读复核原设计/基线 run 40277764908：其 CMake ASan 的 acceptance_runtime 在原 90 秒内
通过（85.24 秒），完整 CTest 打印 52/52、1088.25 秒，随后任务在约 20 分 13 秒处取消；
同轮 Make-ASan 成功，但总用时约 19 分 46 秒。基线的整体 CI 结论仍是 cancelled，不能写成全绿。
结合这些耗时和原实现 Make-ASan 在约 20 分 15 秒取消的记录，推断原 20 分钟 job 预算
缺少稳定容纳配置、构建、完整 sanitizer 测试及收尾的余量。原日志只显示
“The operation was canceled.”，未直接提供取消原因，不能把该推断写成已确认平台诊断。
本次把 GitHub job 总预算调整到 30 分钟，继续执行全部六路检查。
各 CTest 组的原时限、sanitizer 选项、测试断言和产品执行预算保持原值；本地 90 秒超时事实保留。

本次审查由相同模型的独立代理进行，不冒称独立第二模型审核。
没有付费/真实 provider、生产数据操作或强 bwrap/cgroup 部署正向验收。
后续 R2a.1 的 fs_mkdir 见[独立版本报告](r2a-directory-validation.md)；PREPARED 分配恢复/显式放弃
以及 R3 协议回放继续按独立增量推进。并发写者租约、按需执行代次、独立业务审核、
完整记忆/MCP 和 S2–S5 未由本项宣称完成。
