# R1.3 Worker 历史窗口：接续验证与发布

日期：2026-10-25 UTC。实现 **0.7.5-r1.3**，蓝图 **v0.4.1**。
契约见 [R1-worker-history-window](../implementation/R1-worker-history-window.md)，设计见
[ADR-069](../adr/069-worker-history-window.md)。机器结果见 [results](r1-worker-history-window-results.json)。

## 恢复来源与交付范围

本次先读远端，而非采用聊天中未核验的进度。main 仍为
`0dbf478892a381aed6fee99d6e1243363b005297`；实际实现已在 work/r1-worker-window：

| 提交 | 内容 |
| --- | --- |
| `c8f6a7585aa5b9c1e01a821a43cbf7a3b0db235e` | ADR-069、冻结窗口与回读的设计和验收目标 |
| `05c33b278a4e1cd80179ddbc77243663b2cb1b3c` | Worker 窗口、原始证据回读、10项C++与8项CLI回归 |
| `15611e215196ddfc1cdad04468847cba846824be` | 捕获高水位的交错修复，另加2项C++与1项CLI回归 |

从 GitHub source artifact **12592714950** 恢复15611e2源码，source.tar.gz SHA-256为
`99439b5261f828b29ea203912c00f4a5b013dc05b64918fa5981b58082052694`；Git tree逐项核对为
`0c1e7765f643127b6f7fea4a1fae8c3d6333955c`，并从已保存bundle恢复真实祖先。
另收到的开发检查点是基于R1.2的不同未完成重建（曾留下编译/定向退出2），不等于当前工作分支。
已比较并原样保留检查点，没有用它覆盖新代码，也没有把其成绩算入本报告。

本次没有重写已经通过的实现；对该代码重新构建、完整运行门禁、复核读取/权限/版本边界，
修正文档中已过时的开放项状态，补齐原来缺失的发布报告。最终文档提交不改变219个执行输入。

## 已实现的行为

- 新请求显式冻结 structured-v2 / window-v1，保留近期完整往返；旧/大轮次整体改为带哈希的原证据引用。
  不裁断工具调用与结果配对，不递归嵌入旧上下文，不删除原始反馈，也不从当前文件补造历史。
- 完整当前需求、全部AMEND、约束、任务、说明、权限相关绑定及预算仍是权威输入。
  未决操作和不确定/缺失反馈的调用独立于近期尾部，固定保留在首条history_window消息；超过安全边界明确拒绝。
- 模型控制history_page/history_read均接入run-fake和pump-fake。使用当前身份/作用域/政策及普通调用预算，
  原始结果不可变，同响应精确重放不再次读取；失败查询同样消耗持久频率额度，重开不会清零。
- 缺字段的旧快照保留原full-history/framed-v1行为。改变当前YAML不改变已有请求；本批没有自动迁移、LLM摘要或网络发送。
- 高水位修复针对COUNT/MAX与近期SELECT间插入新调用的确定性交错：该批不混入新行，下一批可看到它。
  固定成员范围不是冻结结算状态；未决事实仍单独读取。没有生产测试钩子、删除审计或放宽门禁。

## 实际回归与大结果复现

相对R1.2新增 **12项C++、9项实际CLI**，两套构建入口均纳入：

| 检查 | 结果与断言 |
| --- | --- |
| 连续大结果及重开 | 61100字节文件，第一进程16次Worker读取，重开再16次；无新增TeamLead尝试，32份原反馈完整保留 |
| 原证据回读 | 旧结果移出窗口后，由模型分片读取；修改现在的工作文件不改变返回的原字节和原绑定 |
| 作用域/额度 | 跨角色/任务、错误指纹、非法偏移/长度均拒绝并保留反馈；有效和失败查询都受跨重开的频率限制 |
| 当前状态 | WAITING、UNKNOWN、未处理AMEND、未确认说明、撤销政策不被窗口或回读绕过 |
| 超过旧历史规模 | 作用域近期查询不再读取全部历史；窗口外缺失反馈仍固定保留；旧完整API保持明确超限行为 |
| 输入与兼容 | 非法模式、组合、未知字段及越界值拒绝；旧快照不因新配置自动迁移 |

本次额外以Release二进制独立重复已有CLI场景（不增加用例数），记录：
**32次Worker读取，TeamLead前后均1次，32份原文件结果，已有反馈逐行不变；逻辑JSON输入最大29923字节，
最后一次28472字节。** 两轮均仅到显式每批16次调用的worker_limit，不是上下文quota或未知结果阻塞。
这里统计的是这一固定场景的逻辑输入字节，不是模型token、网络wire长度、吞吐提升或真实模型判断质量。
复现脚本和原始JSON随日志包保留。

## 本次冻结源码验证

所有测试由 **oai / UID1000** 执行，仅使用自建临时SQLite、工作文件及FakeModel。
本地GCC14.2、CMake3.31.6、Boost1.83、Python3.13.5/jsonschema4.26.0；工具链实际版本与托管GCC13/Clang18分开。

| 门禁 | 本次实际结果 |
| --- | --- |
| GCC Release / umask0002 | 47/47组，通过 |
| 同一Release / umask0022 | 47/47组，通过 |
| GCC UBSan / umask0002 | 47/47组，通过；halt_on_error开启 |
| GCC ASan / umask0002 | 47/47组，通过；地址/泄漏检测开启 |
| 严格Make / umask0002 / 用户CPPFLAGS | 完整目标通过 |

独立用例为 **S1 C++376、实际S1 CLI112、M1 C++52及演示3、Python参考契约36**。
重复构建不累加用例数；没有提高既有测试超时、关掉sanitizer、删用例或放宽生产权限。
本次本地没有重跑Clang；[代码分支六路CI](https://github.com/wmqfl861/wls861/actions/runs/40178677852)
对应15611e2，strict、strict/umask0002、UBSan、CMake ASan、Make-ASan、Clang18均已核实success。
其中五项CMake门禁运行47组，Make-ASan运行完整Make目标。
最终main文档提交的CI是另一份证据，须核验其SHA，不能从这个工作分支成绩直接推断。

```sh
# 源码根目录，以普通用户执行；Python须已具备测试依赖。
cmake -S . -B build-r13-release -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-r13-release --parallel 2
(umask 0002; ctest --test-dir build-r13-release --output-on-failure -V -j2)
(umask 0022; ctest --test-dir build-r13-release --output-on-failure -j2)
# UBSan/ASan分别配置Debug与WLS_ENABLE_UBSAN=ON或WLS_ENABLE_ASAN=ON，再完整构建：
(umask 0002; UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-r13-ubsan --output-on-failure -V -j1)
(umask 0002; ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-r13-asan --output-on-failure -V -j1)
(umask 0002; make -j2 test BUILD_DIR=build-r13-make CPPFLAGS=-DWLS_R13_USER_CPPFLAGS=1 PYTHON=/opt/pyvenv/bin/python)
```

219项执行输入在全部门禁后重新核对；清单SHA-256：
`d8c1774095666c2aba6e653916c3f72349957d24c967e5340ed1246b112305e7`。
完整命令、退出码、原始日志、环境、清单、检查点比较及复现脚本保存在会话附件
`wls861-r13-final-validation-logs.zip`。关键日志指纹：

| 日志 | SHA-256 |
| --- | --- |
| `release-tests.log` | `4512745584a3a6f0976b066d7c59ab1cec0ce102414c1ff58777cb0abe1509ef` |
| `release-0022.log` | `6066b0ca3a8211126406271acafba1fca5f0059fef87fcfbc7bc4ed225040932` |
| `ubsan-tests.log` | `31f8190ac5f1d1881406f4be824b2ee8664c9b140a76197c088572847de92f2e` |
| `asan-tests.log` | `c0237e40ab5b0ded3d06fc10f60fb077cc315a60d8b4b432ade7d2f079831ff1` |
| `make-test.log` | `9177504d8f2a6ac7b8e637b35cc35b97311a51d41b48ef319b74445ab3484d27` |

## 发布与剩余边界

保留main祖先和三项原分项提交；通过正常快进发布，提交前核对预期远端头，不强推、不改审核分支。
已发布SQL、vendor、交接档案和审核原件与基线逐项无差异。此次复核由同一实施者执行，
没有独立第二模型评审、真实供应商调用、模型费用、生产数据操作或强沙箱部署正向成绩。

**本批解除显式新窗口请求的已复现大工具结果历史死锁，不代表整个R1或S1全部完成。**
旧请求迁移、管理角色窗口、完整当前契约/超过64个未决事实的可恢复超限、语义摘要、实际provider传输及wire收据仍开放。
按ROADMAP继续R2否定验收/返工耗尽的受控出口与按请求恢复，R2a目录分配中断/fs_mkdir；
R3协议回放可按接口并行，付费验收仍须所有者提供P-01/P-05。
