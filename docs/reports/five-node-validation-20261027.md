# 0.9.0-r3.3b 五节点联合验证

日期：2026-10-27。基线 `56185a48303deb1c72b2ec53d896e8c0b32055f0`，蓝图 v0.4.1。
Store 从408新增409/410迁移；八份已发布 schema/迁移、原 handoff 与固定第三方依赖保持原字节。
本页记录本轮实际执行，旧报告保留原版本范围。

## 1. 五个交付节点

| 节点 | 本轮可执行范围 | 独立契约和报告 |
| --- | --- | --- |
| 1：R3.3b 原预约与发送 | 冻结 live profile，原 attempt/预算，一次性发送权，原私有传输观察 | [原 Store 契约](../implementation/R3-original-store-live.md)、[原 Store 验证](r3-original-store-validation.md) |
| 2：R3.3b 原结算与对账 | 不可变原结算，明确宿主恢复观察，逐维度追加核对、CAS/幂等及原预算重建 | 同上；两节点共享原事务与迁移，联合集成发布 |
| 3：R3.3c 本地框架 | 五项固定任务、十个 paired 样本位、共同条件、原证据报告与缺失/失败保留 | [评测契约](../implementation/R3-evaluation-framework.md)、[评测验证](r3-evaluation-validation.md) |
| 4：R1.4 管理窗口 | 新冻结有界视图、完整当前事实、原 hash/版本回读、持久查询额度和预约前诊断 | [管理窗口](../implementation/R1-management-window.md)、[管理验证](r1-management-window-validation.md) |
| 5：owned 工作区容量 | 文件效果、stage、库存和清理贯通的新冻结容量；旧大树明确保留出口 | [容量契约](../implementation/R3-owned-workspace-capacity.md)、[容量验证](r3-owned-capacity-validation.md) |

独立设计决定为 ADR-081～084。代码使用原 Store、原反馈和原预算，未新建绕过这些关口的业务执行器。
CLI/OfflineRuntime 继续为 FakeModel 入口；真实宿主库路径要求明确配置和独立执行授权，默认关闭。
测试只使用临时 SQLite、私有文件、合成凭据、FakeModel 和自行启动的 loopback TLS。
**真实供应商调用、付费调用、生产数据操作均为0。**

## 2. 同一交付输入

[five-node-inputs.sha256](five-node-inputs.sha256) 包含 **281 项**构建、源码、测试、SQL、
配置、提示词、Schema、脚本及固定依赖输入，全部与普通用户执行副本逐字相等。
清单 SHA-256：

`42b2f25aba2bc04b13777d9b5a5518beeb2a898e118eb37031a4e29a6c9cb59b`

报告和原日志在执行后保存，不改变运行输入。该清单含最终 JSON 引用生命周期修复和五个实际
SIGKILL 阶段。开发中较早的281项清单、两种 umask Release 候选成功及首轮 ASan 失败只作为
过程证据，不能替代修复后本清单的结果。

发布使用原 main 父提交的快进更新，检查期望旧 SHA、远端创建 tree 与本地暂存 tree 一致。
推送后必须按最终提交 SHA 重新核对六路 CI 与 source-snapshot；提交前不预填远端成功。

## 3. 本地门禁

构建、测试均使用普通 **UID/GID 65534**，严格 C++20 和 `-Wall -Wextra -Wpedantic
-Wconversion -Wshadow -Werror`。原生产权限、原测试断言、原套件超时及 sanitizer 检查保持。
新增管理长流程套件单独限300秒、新容量运行器套件180秒，其余新套件按各自显式上限。

| 检查 | 实际结果 | 准确范围 |
| --- | --- | --- |
| GCC Release，umask0022 | **70/70通过，110.97秒** | 最终冻结源码的完整CTest |
| GCC Release，umask0002 | **70/70通过，103.36秒** | 同一源码的完整CTest |
| GCC Debug ASan/LSan，umask0002 | **7/7通过，167.50秒** | 新增六套件及config_loader；`halt_on_error=1:detect_leaks=1`，无诊断 |
| Clang18严格关键TU | **9/9通过，9.383秒** | 普通用户syntax-only；完整Clang构建/测试由最终CI确认 |

环境：GCC13.3.0、Clang18.1.3、CMake3.28.3、Ninja1.11.1、Boost1.83.0、C++ SQLite3.45.1、
OpenSSL3.0.13、libcurl8.5.0。Python3.12.14、jsonschema4.10.3，Python自身SQLite3.53.1；
不与C++ Store链接的版本混同。

机器结果见 [five-node-local-results.json](five-node-local-results.json)；完整套件日志为
[Release0022](five-node-release-0022.txt)、[Release0002](five-node-release-0002.txt)、
[ASan/LSan](five-node-asan.txt)及[逐项输出](five-node-asan-detail.txt)。
Clang的[原命令/日志](five-node-clang18.txt)与[机器结果](five-node-clang18-results.json)列明九个TU。
所有日志hash及原ASan失败的原始hash保存在机器记录中。

70 是 CTest 套件数。新增各套件的方法数分别为：原 Store **15**、Store+TLS **10**、
管理窗口 **14**、容量文件 **11**、容量运行器 **13**、评测框架 **23**；它们不可与套件数或
重复运行次数相加。TLS 10个方法内包含5个 SIGKILL 阶段及其他参数子用例。

调用 umask 分别为0022/0002；部分既有夹具继续按原规则创建私有模式，因此不把调用 mask
冒称为所有内部创建动作的最终模式。所有破坏、替换、权限和进程终止注入都局限于一次性夹具。

## 4. 关键实际观察与修正

- **发送与崩溃：**在预约提交后、claim提交后、网络等待中、完整传输返回后和观察入库后真实
  SIGKILL 子进程，按原 Store 接管恢复；原 attempt 不再取得发送权，服务端请求计数不增加。
  无 claim 的恢复有独立宿主事实，可凭原未获发送权证据追加五维零核对；已获 claim 而无响应的
  原结算保持全 null，只能随后依据独立证据核对。
- **账目与权限：**原响应/用量/结算字节保持，部分维度只释放对应预留；矛盾和超预留继续阻止
  新付费执行。终态对账不恢复业务资格，OneRequest 恢复及对账不改变兄弟请求。
- **容量贯通：**实际合法大转义输出超过1 MiB的原结算事件可以完整保存；3 MiB累计传输上限
  的失败也保留原观察。只给新 live 结算明确的4 MiB事件payload界限，其他事件/旧Fake保持原规则。
- **管理长流程：**130次实际 Fake 管理问答、中途重开及窗口外原字节回读；OPEN、UNKNOWN、
  原捕获目标完整保留。管理和 Worker 对 Store410 live 都消费五维投影，不改原用量。
- **工作区：**以基线原源码复现4097条合法业务树无法进入4096条清理库存；新策略实际测试
  240 MiB业务内容+16 MiB遗留stage=256 MiB、4095+1条目，仍按原库存两轮删除；旧树超限零删保留。
- **评测关联：**加强唯一 Worker 身份、不同原任务的 update/keep、同一任务拒绝/返工/接受关联；
  读取容量逐行累计，到32 MiB即停，失败/未知样本保持可见。

首轮 ASan 发现并修正真实产品缺陷：management_view 向 Boost.JSON object 插入新字段时，
直接引用该对象原成员作为赋值源，扩容会造成 heap-use-after-free。现先复制三个计数标量，
再插入字段；没有改动测试断言、关闭 ASan 或仅凭 Release 成功放行。
完整失败诊断见 [five-node-asan-initial-failure.txt](five-node-asan-initial-failure.txt)（保存时仅去除行尾空格），节点报告保留
具体因果及修复后验证。新增测试开发中另修正少传编译参数、错误表名、错误期待错误码和捕获投影比较；
这些夹具修正与上述产品 UAF 分开记录。

## 5. 可复现门禁

以下命令在普通用户下运行，Python具有仓库固定测试依赖：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-release --parallel 2
umask 0022
ctest --test-dir build-release --parallel 2 --output-on-failure
umask 0002
ctest --test-dir build-release --parallel 2 --output-on-failure

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_ASAN=ON -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-asan --parallel 2 --target wls config_loader_tests live_model_store_tests live_model_store_driver r1_management_tests owned_capacity_tests owned_capacity_runtime_tests
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-asan --parallel 2 --output-on-failure -R '^(config_loader|live_model_store|live_model_store_tls|r1_management|owned_capacity|owned_capacity_runtime|r3_evaluation)$'
```

完整 UBSan、CMake ASan、Make ASan 和 Clang构建/测试保留原六路 GitHub Actions。
最终提交远端结果以对应 SHA 的工作流为准，本地专项和静态交叉复核不替代它们。

## 6. 保留边界

R3.3c 这里只完成本地框架，真实五任务/单 Agent质量对照尚未执行；没有通用单 Agent运行器或
生产 live CLI。供应商能力、底层模型身份、真实计费来源及质量不能由 loopback 成功推导。
可信宿主核验用量的回调是信任边界，不把模型自述或普通 JSON 当成经验证的供应商计费证据。

新工作区容量为 file-tools-only，`run` 在操作准备前拒绝，不提供进程磁盘quota。强bwrap/cgroup
部署正向、外部c-ares DNS、服务化并发租约、私有行purge、旧活动请求窗口迁移和LLM语义压缩
继续单独验收。小任务可先按获准端点/预算进入下一步，当前默认真实开关及自动付费重试保持关闭。
