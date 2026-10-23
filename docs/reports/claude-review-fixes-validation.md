# Claude 审核后首批修复：实际验证报告

基线 `fcdbdd0eff537fa1d157e7207d5803830e8ef0ec`；实现补丁 **0.7.1-s1.6**；目标蓝图 v0.4.1。
本报告记录当前执行环境的实际复验；上传审核件的日期标签 2026-10-23 原样保留，不作为本轮执行时间的依据。
[逐项采纳](../reviews/claude-disposition.md)与[当前能力](../STATUS.md)分别列明已修和开放问题。

## 1. 独立于原审核记录的复现

在原始源码树、普通用户 oai（uid 1000）下，umask 0002 复现了八个 C++ 套件的105项失败；
s15 CLI 的17项中有1项失败。Clang17 ASan 实际报告 config_loader_tests 的 heap-use-after-free，
在首个问题处终止；另一处相同引用/插入模式由源码确认，并一并修复。没有把原审核的
“全部 ASan 路径干净”或 GCC13 的警告复现，冒称为本轮已完成的基线检查。

第一轮修复验证出现一项旧断言不匹配：上下文容量现在正确返回 quota，旧 event_runtime
测试仍期待 invalid_input。保留该失败日志，更新精确错误码断言后对冻结源码重新跑完完整门禁。
没有跳过套件或降低权限、警告、sanitizer 要求。

## 2. 本批实现及回归

- 子进程在 fork 后设 umask 0077；实际进程监督夹具验证父0002下创建文件0600/目录0700、父mask不变。
- 自建测试夹具明确私有创建权限，README示例说明新目录和既有目录的区别；恶意 chmod 反例保留。
- 两处JSON对象引用先复制再插入；简单内存/时刻格式改用ASCII检查，新增合法/非法格式回归。
- Make的必需include不再被用户CPPFLAGS覆盖；主CLI字符转换显式，保留严格编译告警。
- CLI与Store共用精确金额解析，覆盖0..20000分及指数/越界；实际2.01账本为2010000微美元。
- 约束catalog键名和类型前置校验，输入错误统一invalid_input；语义、版本及权限仍由Store再次校验。
- wait/wake先只读检查归属、问题、终态、等待及语义绑定，取得writer后再次授权。
  20次无效wait不改变空闲请求数据库；合法writer全库恢复仍未修复。
- check/explain新增runtime_support与validated_only字段；不改变冻结配置内容/哈希，不假装限制已执行。
- 上下文容量错误为quota；旧历史不会被截断。本批没有上下文压缩和新的恢复窗口。

## 3. 最终本地结果

| 检查 | 实际结果 |
| --- | ---: |
| S1 C++ | 310 / 310 |
| S1 实际CLI | 78 / 78 |
| M1 内核 + 合成CLI | 52 + 3，全部通过 |
| Python Schema/SQLite参考 | 36 / 36 |
| CMake GCC Release，umask0002 | 33 / 33组 |
| 相同Release产物，umask0022 | 33 / 33组 |
| Make严格构建，命令行CPPFLAGS，umask0002 | 完整门禁通过，退出0 |
| Make GCC UBSan，命令行CPPFLAGS，umask0002 | 完整门禁通过，退出0 |
| Clang17 Debug ASan+泄漏检查，umask0002 | 33 / 33组，退出0 |

相对原S1.6新增6项C++、9项实际CLI；构建模式的重复运行不累计为更多用例。
本地GCC14.2、Clang17、Boost1.83、SQLite3.46.1、OpenSSL3.5.7、Python3.13.5/jsonschema4.26.0。
本地没有冒称使用仓库的jsonschema4.10.3 pin或参考GCC13/Clang18。GitHub新增矩阵分别核验它们。

实际命令（由普通用户运行，宿主只负责启动和保存日志）：

```sh
cmake -S . -B build-review-release -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-review-release --parallel 2
(umask 0002; ctest --test-dir build-review-release --output-on-failure -V)
(umask 0022; ctest --test-dir build-review-release --output-on-failure)
(umask 0002; make -j2 test BUILD_DIR=build-review-make CPPFLAGS=-DWLS_REVIEW_USER_CPPFLAGS=1 PYTHON=/opt/pyvenv/bin/python)
(umask 0002; UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 make -j2 ubsan BUILD_DIR=build-review-make CPPFLAGS=-DWLS_REVIEW_USER_CPPFLAGS=1 PYTHON=/opt/pyvenv/bin/python)
cmake -S . -B build-review-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DWLS_ENABLE_ASAN=ON -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-review-asan --parallel 2
(umask 0002; ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-review-asan --output-on-failure -V -j2)
```

156个执行输入复验前后SHA-256一致，见[源码清单](claude-review-fixes-source-SHA256SUMS.txt)。
[结果与原始日志指纹](claude-review-fixes-results.json)列明各日志字节数和哈希；完整原始日志作为
会话附件 `wls861-claude-review-validation.zip` 交付，包含原基线失败和中间断言失败，没有删去不利结果。
本报告创建时GitHub新矩阵尚未执行；不预填未来成功，最终结果以同提交Actions日志为准。

## 4. 开放项与验收边界

审核1的完整压缩、2的事件归档/恢复范围、3的拒绝后出口、4的PREPARED分配恢复仍开放。
12目录工具、13交付逻辑路径仍待独立设计。14只完成未生效控制披露，墙钟/告警/并发尚未执行。
本批是同一实现者复核与反例测试，不宣称独立人工或第二模型评审。

实际执行真实SQLite、临时文件、子进程及跨进程CLI；模型为FakeModel，零真实模型调用。
进程umask夹具是内部受控监督路径，不替代bwrap/cgroup强沙箱部署正向验收。
没有生产数据操作、付费请求、weak回退、私有行purge，也没有把整个S1–S5标为完成。
