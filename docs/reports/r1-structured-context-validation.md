# R1.2 结构化模型请求：实现与验证

日期：2026-10-24 America/Los_Angeles / 2026-10-25 UTC。
实现 **0.7.4-r1.2**；蓝图 **v0.4.1**。契约见[实现说明](../implementation/R1-structured-context.md)，设计见[ADR-068](../adr/068-structured-model-input.md)。

## 来源与交付范围

以已发布 `b493752a10d2ad66670d6b201b24b1f489df7931` 为基线，从上一轮 GitHub 源码快照恢复，
索引树核对为 `739edf56f336b61c30c34143d5501fd5a5f5b2b1`。本地仅恢复精确基线提交及树，
以浅仓库继续；不宣称恢复了完整旧历史，远端祖先不改。

| 提交 | 内容 |
| --- | --- |
| `b0e5b60a7e1dd88e7ca24dc29f717f5f3fc39a23` | 先定义结构化输入、冻结选择、工具往返及逻辑/wire证据边界 |
| `f89cb041b38b58755b2c944fd28b9d0ec4946e0e` | C++实现、运行器、冻结配置、16项C++与6项实际CLI回归；代码树 `f2efcb38eac7194508225534319c9f7ea33ac7ef` |

**本批完成结构化请求，不完成上下文窗口。** 新请求显式选择 `structured-v2`，兼容样例和缺字段的旧快照仍为 `framed-v1`。
run-fake/pump-fake 的 Dispatcher、TeamLead、Worker 使用同一持久化边界；实际管理验收/经验目标与原工具结果均有CLI回归。
尚未发送网络请求，所有wire指纹为null，不用逻辑哈希冒充线上发送证据。

## 关键行为及新增回归

- 系统具名段、工具参数Schema、完整任务契约与动态消息分别保存；动态system消息拒绝。
  assistant/tool调用结果严格配对，孤立、重复、交叉与未完成的轮次拒绝，历史工具不新增当前权限。
- 逻辑ID按attempt ID及原tool-call ID派生。不同尝试复用供应商ID也不会串结果；原ID、原绑定和反馈哈希仍保留。
  缺失、迟到、未采用或运行时拒绝的记录作为原始观察，不补造工具成功结果、不重新执行。
- prefix/cache、完整逻辑输入及完整预约请求分别计算指纹。恢复时重编译核对结构/元数据/哈希；重算假哈希不能绕过配对规则。
  全请求指纹还绑定身份、版本、输出上限、超时和预留；Store最后的身份/版本/CAS与冻结格式门禁保留。
- 旧v1往返字节兼容；缺字段快照保持旧格式。修改当前YAML不改变已有请求，格式错配在新预约前拒绝且不消耗预算。
  实际跨进程重开验证原文件结果、原绑定、无新增TeamLead调用与新旧逻辑ID隔离。
- 连续61,100字节结果的回归明确验证当前仍会quota且不静默裁剪。这是开放问题的保护性回归，不是窗口修复成绩。

## 冻结源码的本地验证

普通用户 **oai / UID1000**；GCC14.2、CMake3.31.6、Boost1.83、Python3.13.5/jsonschema4.26.0。
完整环境与日志哈希见[机器结果](r1-structured-context-results.json)。本地版本不冒充托管GCC13/Clang18或固定Python测试依赖。

| 门禁 | 实测结果 |
| --- | --- |
| Release / umask0002 | 45/45组，通过 |
| 同一Release / umask0022 | 45/45组，通过 |
| GCC UBSan / umask0002 | 45/45组，通过 |
| GCC ASan / umask0002，含泄漏检测 | 45/45组，通过 |
| 严格Make / umask0002 / 命令行CPPFLAGS | 完整目标通过 |

独立用例：**S1 C++364、实际S1 CLI103、M1 C++52及演示3、参考契约36**。
本批新增16项C++、6项CLI；重复构建不重复累计用例。FakeModel决策与真实临时SQLite/文件/跨进程CLI分开，不等同真实模型质量或强沙箱部署验收。

```sh
# 源码根目录，普通用户；Python已具备测试依赖。
cmake -S . -B build-r12 -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-r12 --parallel 2
(umask 0002; ctest --test-dir build-r12 --output-on-failure -V -j2)
(umask 0022; ctest --test-dir build-r12 --output-on-failure -j2)
# UBSan/ASan分别配置Debug与WLS_ENABLE_UBSAN=ON或WLS_ENABLE_ASAN=ON，完整构建后：
(umask 0002; UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-r12-ubsan --output-on-failure -V)
(umask 0002; ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-r12-asan --output-on-failure -V)
(umask 0002; make -j2 test BUILD_DIR=build-r12-make CPPFLAGS=-DWLS_R12_USER_CPPFLAGS=1 PYTHON=/opt/pyvenv/bin/python)
```

冻结215个执行输入，清单SHA-256：`bedfefcf78602d8a19354846e369c2fa4143b08f13110d4f65cb540895aeb1b8`。
完整日志、环境与清单在会话交付包中；关键原始日志指纹：

| 文件 | SHA-256 |
| --- | --- |
| `release-frozen.log` | `ab8a28a38c35fa3e13ac8ddab3d2d251a28cb6ec321162928c58440092ef8928` |
| `ubsan-frozen.log` | `01cdb6571f27ebd108db3053f1efab348d610e65013a99eb6ce4abe7d609ed3b` |
| `asan-frozen.log` | `6fdd28ffbbfabdaf71d86140b2e2a9b720ef06b107b6eeeb598ef8173f36a7cd` |
| `make-frozen.log` | `03c48ed52859a26d261b6fe4d58eafb86150fcb3a57ae7981abed1b87977f034` |

## 失败过程与复核

初次构建修正了Boost JSON字符串插入和测试比较类型；一次构建被工具超时中断，不算测试失败或通过。
首次全量Release为44/45组：新CLI临时夹具文件继承umask0002导致组可写。仅将夹具设为0600并断言，生产权限未放宽。
定向复核增加缺失/损坏原反馈、重复ID、元数据及全请求指纹覆盖。源文件变更期间的一次旧UBSan构建混入旧对象，
不作为最终证据；冻结上述代码后完整重编译并重跑，采用本报告四份frozen日志。没有删用例、关闭sanitizer或提高测试超时。
复核由同一实施者完成，没有独立第二模型审核。

## 发布与剩余范围

两项原始提交通过SHA-256校验的补丁及原commit字节传输；归档指纹
`47d7b9cebae3dd59f701c31686051a1e70d58eaf14a7d9cec92ace9e372cf671`，本地重建逐项SHA/tree验证通过。
[候选导入验证40154245286](https://github.com/wmqfl861/wls861/actions/runs/40154245286)已完成：精确树校验、普通用户Release全回归和开发分支非强制快进成功。
临时传输文件/写权限工作流既不在实现树，也不是其祖先；正常六路CI保留。
本报告提交只增加证据和修正STATUS标题；最终main发布及六路CI以本报告对应提交的GitHub记录为准，不用候选/旧提交代替。

原SQL迁移、vendor、handoff及审核原件无改动；没有真实模型调用、费用、生产数据操作或审核分支写入。
**R1.3仍需实现冻结上下文窗口、遗漏索引、原证据回读模型工具和未决状态保护，验证连续大结果与跨重开继续执行。**
当前Worker仍完整读取历史，1MiB/256消息及旧历史容量边界仍在。网络适配器、wire回执、真实缓存/质量评测、
R2拒绝返工/按请求恢复和强沙箱正向部署仍按路线图开放。
