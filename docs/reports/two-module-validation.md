# 0.10.0-r3.3c-runtime 两模块联合验证

基线为已完整推送的 `1c572d3aa9121e85cf22aa1e4bb330b25a34a143`，蓝图v0.4.1，Store410。
本批完成[受控在线工作流](../implementation/R3-live-workflow.md)和
[迁移注册表／可安装资源](../implementation/runtime-resources-and-migrations.md)。
原SQL、vendor、handoff与旧结算字节不变。这里记录本批实际执行，历史报告保留原范围。

## 1. 交付与实际边界

| 模块 | 交付内容 | 本地验证性质 |
| --- | --- | --- |
| 在线工作流 | 原capture→原预约→唯一发送→私有观察→首次结算→可选受信核验→原业务应用；event pump、精确授权CLI、有限batch及五维账目停批 | 自建loopback TLS、合成凭据和模型响应、真实临时SQLite与文件；不是实际供应商质量验收 |
| 安装资源 | 固定35项SQL/Schema/公共提示词、完整manifest与编译注册表比对、一次捕获、迁移连续前缀、CMake prefix/DESTDIR与可执行文件相对发现 | 真实安装、重定位、无关cwd、删除安装来源后运行、实际资源破坏与特殊文件拒绝 |
| 工作流评测配套 | v2模型来源证据，原response/target/feedback/event和同一业务结果关联 | 原生合成响应正例、宿主直写/错绑/跨任务与旧版本串证据反例；v1仍是结构验收 |

CLI没有通用JSON计费导入器。当前Responses子集的费用保持null；首个原本允许应用的响应
完成后停在 `needs_accounting`，不会继续新预约。完整连续流程测试通过仅接受local_tls
的独立合成账单核验器追加证据，原响应和原结算不被改写。实际供应商核验器仍待另行实现。

**本批真实外部供应商调用、付费调用及生产数据操作均为0。** 单Agent逻辑身份执行器、
真实同模型质量/成本对照、强部署沙箱正向、daemon/API、并发租约和私有行purge仍未验收。

## 2. 统一输入与最终本地门禁

[two-module-inputs.sha256](two-module-inputs.sha256) 冻结 **296项**构建、源码、测试、
配置、SQL、Schema、提示词、固定依赖和CI输入。普通用户执行副本逐字一致，清单SHA-256：

`c58ebacb60620cf6e1a8c5435d26233edc2837177050ca6c33bbad36f149b06e`

该清单包含来源关联修正与ASan实际阶段同步夹具。较早候选的成功及失败均另行保留，
不替代本清单的门禁。本批CMake/CTest注册74个套件，保留全部旧套件。

构建与测试为普通 **UID/GID65534**、空附加组，严格C++20及
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`。同一最终输入的实际结果：

| 门禁 | 实际结果 | 范围 |
| --- | --- | --- |
| GCC Release，umask0022 | **74/74通过，124.75秒** | 完整CTest |
| GCC Release，umask0002 | **74/74通过，126.52秒** | 完整CTest |
| GCC Debug ASan/LSan，umask0002 | **8/8通过，230.47秒** | 新资源、Schema、原live Store/TLS、新live工作流/CLI、安装CLI和评测；无诊断 |
| Clang18，umask0002 | **11/11通过，27.797秒** | 新增与受影响关键编译单元严格syntax-only；完整Clang由CI验证 |

机器记录见[two-module-local-results.json](two-module-local-results.json)，原测试输出见
[Release0022](two-module-release-0022.txt)、[Release0002](two-module-release-0002.txt)、
[ASan](two-module-asan.txt)、[ASan逐项](two-module-asan-detail.txt)和[Clang命令](two-module-clang18.txt)。
74是套件数；新资源14方法、安装10方法、live工作流18方法、live CLI11方法、评测31方法
不能与套件数、参数子例或重复运行相加。实际证据与源码范围在各专项报告中进一步展开。

环境为GCC13.3.0、Clang18.1.3、CMake3.28.3、Ninja1.11.1、OpenSSL3.0.13、libcurl8.5.0；
测试Python3.12.14、jsonschema4.10.3、Python SQLite3.53.1。Python SQLite不是C++ Store链接库。
本地ASan使用 `halt_on_error=1:detect_leaks=1`，没有关闭泄漏检查或减少旧测试。

## 3. 已执行的专项与开发过程

三个专项报告按各自源文件范围记录，不代替最终整树门禁：

- [核心兼容](r3-live-core-compatibility-20261026.md)：普通UID/GID65534、umask0002，
  `event_runtime` / `workflow_runtime` / `acceptance_runtime` / `live_model_store` 4/4通过。
- [资源与安装](runtime-resources-validation-20261026.md)：资源14、旧Store31、旧Schema12；
  真实CMake安装10方法及Make两参数安装入口，外层0022/0002。原C++夹具内部使用0077，
  不把外层umask冒称为内部每次创建动作的模式。
- [原模型工作流与评测](live-workflow-evidence-validation.md)：完整原生工具反馈链、AMEND、
  否定验收/有限纠正/耗尽升级、经验、独立账目、取消/deadline与来源证明，详见其实际结果。

root集成过程中实际观察：

1. 首次新资源测试编译因Boost.JSON value与std::string直接比较失败，已在测试中使用原字符串
   类型修正；生产资源校验规则未放宽。原失败日志保留。
2. 第一轮联合专项为9/10套件通过。唯一失败是早期耗尽返工夹具期待在一批内继续升级，
   但原冻结manager调用上限已经达到。最终夹具断言第一批的manager_limit和保留目标，
   再由显式第二批完成升级，未提高生产额度或删掉断言。
3. CLI授权11方法在初次运行全部通过：只读状态/禁用模板、完整绑定负例、特殊授权输入、
   凭据拒绝零预约、原计划及未知停批、重复零重发、授权原字节hash、HTTP429零自动重试、
   冻结端点、工作区外授权、参数预检和兄弟请求隔离。
4. 评测独立复核实际复现了两处证据关联缺陷：不同任务的往返与提交可被独立判据拼接，
   旧版本模型返工链可为当前要求提供错误来源。修复限定v2，让来源和业务判据使用同一
   原绑定、原提交及当前需求/计划版本。升级问题另增加显式当前版本检查；原Store在
   AMEND时已经关闭旧问题，因此没有将该处检查收紧称为已复现的可达漏洞。v1历史解释
   不变；具体反例、断言和实际执行结果见专项报告。
5. 修复来源规则之前的整树Release候选74/74通过，113.34秒。该成功不能代替修复后的
   统一输入验证，也不能证明独立复核发现的问题不存在。
6. 首轮ASan专项7/8套件通过，唯一失败套件的18方法中8项失败。正常流程的合成宿主
   10秒batch在插桩下真实到期；固定160/180毫秒的取消/期限用例有时尚未进入传输。
   没有ASan/LSan内存诊断。正常夹具现显式采用有限60秒batch；取消由peer实际收到
   请求并开始响应后的marker触发，有界watcher须证明观察到该marker；专用5秒deadline
   还核原持久body恰为80字节前缀、零工具/业务应用与未知费用。生产期限、预算及权限
   不变，测试断言保留并加强。专项复验Release18/18（20.677秒）、ASan/LSan18/18
   （195.202秒）通过，原300秒套件上限不变。

保留的原过程证据为[首次编译失败](two-module-build-initial-failure.txt)、
[首次联合专项失败](two-module-focused-initial-failure.txt)和
[首次ASan失败](two-module-asan-initial-failure.txt)，后者的输入身份见
[旧夹具输入清单](two-module-asan-initial-inputs.sha256)。日志保存时仅移除行尾空白，
原始与保存后hash均列于机器记录；不把较早Release成功冒充修正后ASan成功。

## 4. 可复现命令

以下均以普通用户执行，Python具备仓库固定测试依赖：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-release --parallel 2
umask 0022
ctest --test-dir build-release --parallel 2 --output-on-failure
umask 0002
ctest --test-dir build-release --parallel 2 --output-on-failure

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_ASAN=ON \
  -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-asan --parallel 2 --target wls wls861 runtime_resources_tests \
  schema_tests live_model_store_tests live_model_store_driver live_workflow_driver
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-asan --parallel 2 \
  --output-on-failure -R '^(runtime_resources|schema|live_model_store|live_model_store_tls|live_workflow|live_workflow_cli|runtime_resources_cli|r3_evaluation)$'
```

完整UBSan、CMake ASan、Make ASan、Clang18和两种Release umask仍由既有六路CI执行。
此前70套件完整ASan的CTest已实际耗时1333秒，本批单个新增完整live套件实际约195秒，
还需构建、依赖安装和其余新增测试，因此只把整个CI job的容量上限从30调至40分钟；
所有逐测试期限和sanitizer断言保持，没有删除旧测试、改宽生产权限或降低断言。
发布采用资源基础与工作流/CLI集成两个依赖提交，最终以期望旧SHA快进更新main，
远端tree与本地暂存tree核对；最终提交六路CI和source-snapshot必须按该SHA检查。
提交前不预填远端通过，最终远端证据由对应提交的Actions记录提供。
