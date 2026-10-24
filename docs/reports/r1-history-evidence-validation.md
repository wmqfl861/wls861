# R1.1 原始历史读取：实现与验证

日期：2026-10-24。实现 **0.7.3-r1.1**，目标蓝图 **v0.4.1**。
本轮交付范围为作用域分页索引、按哈希回读原响应/结果和只读 CLI，
**不包含 Worker 上下文窗口、自动压缩、原生模型请求或真实供应商调用**。
契约见 [R1-history-evidence](../implementation/R1-history-evidence.md)，设计见 [ADR-067](../adr/067-paged-history.md)。

## 来源、提交和发布

从上一轮 `wls861-audit24-delivery.zip` 恢复完整提交
`aa0f79b1eb306102449a6a463de3bcdc93d58a74`；验证恢复后的 Git tree 等于
`25ffbc27527ba3dd95f4f8a50fc4aa42edce5d8d`。未发现可恢复的先前 R1 完整提交，
没有把聊天中的半成品描述当作代码或成绩。本轮实际实现独立的 R1.1 小切片。

| 提交 | 内容 |
| --- | --- |
| `43b45168594f98ad62268102893c293a736f18e6` | 继承上一批已验证、尚未推送的冻结容量窗口修复 |
| `aa0f79b1eb306102449a6a463de3bcdc93d58a74` | 继承其验证报告；旧报告原样保留 |
| `62072928382eb86c3e7cc9895f6e2942537a24d1` | 本轮先定义 ADR-067 和 R1 分步验收 |
| `cb6a52a09c68f0f804e0b1dfee7d15472cdac363` | 本轮 C++/CLI/测试和实现文档；冻结代码树 `1c7174e826f788ea5a4c86fad5eb38c63ee30514` |

通过临时传输分支发送四个原始提交的补丁及原提交字节，整体 SHA-256 为
`18320fc418aa745aecec3451e51a9fc874aae903be448c3c32e878c50ac51fae`。
发布流程逐项验证提交 SHA 和 tree，再执行普通用户 Release/CTest，只快进指定工作分支。
临时数据与写权限导入工作流不属于交付树，也不是上述四个提交的祖先。
[候选导入验证](https://github.com/wmqfl861/wls861/actions/runs/40116579064)与最终 main 提交的六路 CI
是两份证据；最终发布状态以对应 SHA 的 Actions 为准，不从旧 SHA 推断。

## 实际实现与检查重点

- `attempt_history_page` 每页 1..64 条，只读取元数据。首页固定插入高水位，后续页不混入新增记录。
  请求、角色、principal、任务和团队联合限定；非零游标也必须属于同一作用域。
- `read_history_feedback` 读取数据库中已经保存的 response/result 规范化 JSON 字节，每片最多16 KiB，
  返回完整 section SHA-256、原绑定、长度、EOF 和下一偏移。先验证完整反馈哈希，再核对原调用绑定/响应。
  不能以重算哈希把改过的绑定伪装为原记录，也不会读取现在的工作文件补造旧结果。
- `history-page` / `history-read` 使用只读 Store；非法参数、跨任务请求和损坏检测不触发 Takeover。
  回归比较全部数据库表、请求版本和预算，确认读取无写入；等待和 UNKNOWN 对新业务仍生效。
- 高水位只固定插入成员，不冻结结算状态；缺失反馈允许随后由首次合法写入出现。旧记录仍带原版本。
  rowid 游标仅在当前数据库内有效；外部 VACUUM/重建后应重新枚举，永久证据引用使用 ID+哈希。
- 旧 `history` / `attempt_history` 的完整或失败语义保留。内部仍完整校验一条有界反馈记录，
  不是 SQLite 大对象的底层流式读取；新接口尚未注册为模型工具。

## 新增回归

新增 **10 项 C++、7 项实际 CLI**，已同时接入 Make 和 CMake/CTest：

| 覆盖 | 实际断言 |
| --- | --- |
| 270 次尝试 | 旧完整 API 明确报 quota；新索引分批枚举全部原记录，无遗漏/重复；交错兄弟任务和后来新增记录不会混入原范围 |
| 原始大结果 | 超过61,100字节，含中文/emoji/NUL和二进制编码；分片逐字节重组，核对完整长度及哈希；允许分片位于UTF-8字符中间 |
| 原件而非当前文件 | CLI 在保存反馈后改写工作文件，回读仍为原记录内容 |
| 作用域与指纹 | 跨任务/角色/请求、伪造principal、外来游标、错误哈希、偏移/大小越界均拒绝 |
| 损坏与缺失 | 缺失不补造；原字节损坏及自洽假哈希/绑定被检测 |
| 恢复及门禁 | 只读重开不提升代次；接管后旧绑定/原字节保持；旧身份拒绝；读取不能解除等待或 UNKNOWN |

首次损坏注入被数据库不可变触发器提前拒绝，这不是读取逻辑的验证成功。
修正测试夹具：仅在自建临时数据库中移除目标触发器、注入损坏，并在调用读取接口前恢复原触发器。
没有删除生产触发器、降低权限或跳过断言。初次构建中也修正了测试字符串比较/字段假设；过程日志保留。
本轮为同一实施者的定向代码复核，没有独立第二模型评审。

## 冻结源码的本地验证

所有构建/测试均为 **oai / UID 1000**。GCC **14.2**、CMake **3.31.6**、Boost **1.83**，
Python **3.13.5** / jsonschema **4.26.0**。不把本地版本冒充托管 Ubuntu24.04 的 GCC13/固定Python依赖。

| 门禁 | 实际结果 |
| --- | --- |
| Release / umask0002，verbose | 43/43组，通过 |
| 同一Release / umask0022 | 43/43组，通过 |
| GCC UBSan / umask0002 | 43/43组，通过 |
| GCC ASan / umask0002，含泄漏检测 | 43/43组，通过 |
| 严格 Make / umask0002 / 用户 CPPFLAGS | 完整目标通过 |

独立用例数为 **S1 C++348、S1 CLI97、M1 C++52与演示3、参考契约36**。
不同构建模式重复执行不累加用例数。ASan/UBSan未关闭检查，也未提高测试超时。
完整结果机器记录见 [results](r1-history-evidence-results.json)。

```sh
# 所有命令在源码根目录，以普通用户执行；Python须已安装测试依赖。
cmake -S . -B build-r1 -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-r1 --parallel 2
(umask 0002; ctest --test-dir build-r1 --output-on-failure -V)
(umask 0022; ctest --test-dir build-r1 --output-on-failure)
cmake -S . -B build-r1-ubsan -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_UBSAN=ON -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-r1-ubsan --parallel 2
(umask 0002; UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-r1-ubsan --output-on-failure -V)
cmake -S . -B build-r1-asan -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_ASAN=ON -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-r1-asan --parallel 2
(umask 0002; ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-r1-asan --output-on-failure -V)
(umask 0002; make -j2 test BUILD_DIR=build-r1-make CPPFLAGS=-DWLS_R1_USER_CPPFLAGS=1 PYTHON=/opt/pyvenv/bin/python)
```

211个执行输入复验后保持冻结，清单 SHA-256 为
`d521cd1e80842fd8f4bab1a08af58a650f8bf245b3ca3a3bfa62ab8df5030e3b`。
日志、环境和完整清单在会话附件 `wls861-r1-history-validation-logs.zip`，关键日志指纹：

| 日志 | SHA-256 |
| --- | --- |
| `release-full.log` | `845b15219dbd96d1c388283f92e54aee731200121df5c1186ad73f91faa26671` |
| `ubsan-full.log` | `7af55df9efb55656e4c9af19c67954c3aaaae15c8fdf1f8616ccb84aca31c1e4` |
| `asan-full.log` | `059c66fb9ff63b22dbd974738d9036d4116ab38bec786929cfcd706ceca55735` |
| `make-full.log` | `1891028b7a0841646667fa5f961dbca00c1c0eab416829bb76c7085b24d43cf0` |

## 兼容与尚未完成

对恢复基线检查，原SQL迁移、vendor、handoff和两份审核原件无改动。没有真实模型调用、费用或生产数据操作，
没有改变审核分支；本轮使用FakeModel与真实临时SQLite/文件/跨进程CLI。

**Worker仍采用旧的完整历史编译，因此大文件多轮触顶问题没有在本轮解决。**
下一切片必须先定义供应商无关的结构化请求、原生tool-call ID/结果和逻辑/wire指纹，再把冻结窗口、
明确的遗漏索引、原结果回读及必要约束/未决状态接入实际Worker循环。验收要求连续大结果及跨重开仍可继续，
不能靠截短旧API返回或静默丢原文达到通过。R2否定验收后的有界返工/按请求恢复、R2a分配恢复/fs_mkdir、
完整强沙箱正向部署及真实供应商验收仍开放，按ROADMAP推进。
