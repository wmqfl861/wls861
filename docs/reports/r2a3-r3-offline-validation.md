# R2a.3 / R3.1 / R3.2：部署诊断与 Responses 离线协议验证

实现 **0.8.5-r3.2**；目标蓝图 **v0.4.1**。本批交付三个限定切片：
[R2a.3 只读部署诊断](../implementation/R2a-deployment-diagnostics.md)、
[R3.1 HTTP 编解码与 R3.2 SSE 回放](../implementation/R3-offline-responses.md)。
设计先行见 [ADR-076](../adr/076-doctor-deployment-diagnostics.md)、
[ADR-077](../adr/077-responses-offline-codec.md)、[ADR-078](../adr/078-responses-sse-replay.md)。

## 1. 被测输入与源码绑定

| 项目 | 已核验值 |
| --- | --- |
| 原 main 基线 | `33264b16b3a2b4df64d1060cccd60e0ce86d5a79` |
| 本地代码提交 | `bebd3c561eadaf9ed46b7f375dd88623e81ae312` |
| 对应远端代码提交 | [4066fc83de056fef8e76e0e9f76654e63182d879](https://github.com/wmqfl861/wls861/commit/4066fc83de056fef8e76e0e9f76654e63182d879) |
| 两者完整代码 tree | `f64af42adc6989d6445d7048655f32e8d050dba3` |
| 构建、源码、测试和运行资产输入 | **255 项**，逐项匹配上述代码提交和当前工作树 |
| 输入清单 SHA-256 | `a11c6cc0edaaac489e57a3d400c2022e19736d5ea8c5b50ac658ce2f712073dd` |

构建开始于开发工作树；冻结输入在全量门禁前保存，并在测试后与代码提交逐字节复核。
本地与 GitHub 提交元数据不同，故提交 SHA 不同；上传后核验了完整 tree 相同。
发布文档及证据在代码提交之后追加，255 项输入均未因此变化。
不把工作树开始构建时的 HEAD、代码里程碑 SHA 或设计提交 CI 冒充最终发布 SHA。

设计和实现已分批推送；源码按 doctor、profile/HTTP、SSE、CLI 集成分为四个提交。
本批不修改 `db/`、已发布 SQL、`third_party/`、原 handoff、原审核材料或 CI workflow。
Store 继续为408；没有新增结构迁移，也没有放宽旧测试断言或生产权限守卫。

## 2. 实际本地门禁

正式命令均由普通用户 **UID/GID 65534** 执行。Release 完整回归串行运行两次；
本地 ASan/LSan 与 UBSan 只运行直接相关八组，完整工具链由最终发布 SHA 的远端矩阵核验。

| 门禁 | 设置 | 实际结果 | runner 耗时 |
| --- | --- | --- | ---: |
| 最终新增回归与路径修补 | Release，外层 umask0002 | 5/5组；88 C++ + 25 CLI；exit0 | 2.705秒 |
| 完整 Release | umask0002 | 63/63组；exit0 | 190.755秒 |
| 完整 Release | umask0022 | 63/63组；exit0 | 167.201秒 |
| focused ASan/LSan | umask0002，detect_leaks=1 | 8/8组；exit0；无诊断 | 145.705秒 |
| focused UBSan | umask0002，halt_on_error=1 | 8/8组；exit0；无诊断 | 75.338秒 |

每份完整日志分别记录：M1 C++ **52**、S1 C++ **562**、CLI **183**、Python参考契约 **38**、
M1合成demo **3**，合计838个计数用例，另有handoff和review两个原件完整性检查组。
这些是不同层次的测试，不合并描述为产品验收成绩；重复umask和sanitizer执行不增加独立用例数。

本批新增五组共 **113** 项：doctor C++30、profile/HTTP C++30、SSE C++28、doctor CLI12、
协议CLI13。重点sanitizer八组为这五组加`sandbox`、`worker_tools`、`s12_cli_smoke`，
每份包含123项C++和33项CLI；不称为完整sanitizer套件。

**有效umask：**doctor C++与两个新增CLI组实际保留并打印0002/0022。纯codec/SSE C++
使用既有`test_support::run()`，用例期间设置0077，结束恢复；这两个内存协议组不能称为
在有效0002/0022下执行。其他旧夹具保持原私有权限安排，不从外层runner推断全部有效mask。

所有编译保持 C++20、严格警告和`-Werror`。新增五组各90秒，既有组限时与CI30分钟预算未改。
本地ASan的SSE组71.74秒，低于90秒。基线`cleanup_runtime`实际为180秒，
`audit24_capacity_repro`为300秒；前一R2a.2报告曾把cleanup原限时写成90秒，本报告依据原CMake
更正该文字事实，未改历史日志，也未调整任何限时。

环境：GCC13.3.0、GNU ld2.42、CMake3.28.3、Ninja1.11.1、Python3.12.14、jsonschema4.10.3、
OpenSSL3.0.13。实际解释器为`/tmp/wls861-validation-venv/bin/python`。
各构建二进制摘要、实际命令、UID、外层umask和逐组计数见机器结果及环境记录。
执行器和GitHub使用各自时钟；本地耗时使用runner的subprocess差值，不跨环境计算时差。

| 原始门禁日志 | SHA-256 |
| --- | --- |
| Release0002 | `82ff22482b73828845e2dd60b3ba958e0d849aea7e85ea1ace5fcd7b47bd0829` |
| Release0022 | `aeaf5f82f19d8815421c8a7ed1c4e99d88b8bedd3956f331a133ecee9c0d91f0` |
| focused ASan/LSan | `d03aa6ea289a6ee041043d1d5d8eb2249c24c2e9e36e7f4228bb2c461edbebab` |
| focused UBSan | `3a5df913ca65c1c5a57222177416abf26bb3f76b7337ba4f023db7a0facbd004` |

## 3. 三个节点的具体证据与边界

### R2a.3：只读部署诊断

CLI区分默认/显式/未配置输入，记录本次配置与manifest原字节、实际词法路径、执行策略指纹。
这些是当前显式配置，不是某个既有请求的原冻结快照。真实文件校验仍遵守原无符号链接路径、
owner/mode、大小、哈希和读前后身份条件；一个文件失败后继续观察独立文件。

C++使用私有真实临时文件覆盖缺失、链接、权限、类型、稀疏超限、哈希冲突、读取中增长和注入EIO；
共享小预算及固定cgroup分类明确为合成分支测试。root所有的系统ELF只读观察，不执行。
CLI持有实际SQLite writer lock并比较全部表行及文件字节/mode，验证doctor不打开或接管Store；
假launcher带效果标记，但标记未出现。报告超限后持续保持不完整，不会因再序列化恢复就绪。

cgroup普通目录反例、控制器/权限观察和八个未来operation路径模式，不能证明真实委派、
namespace、挂载、网络隔离或后代退出。doctor不执行launcher/环境检查、不写控制器、不扫描或收编残留。

### R3.1：外部 profile 与 HTTP 编解码

外部profile严格校验协议、模型、HTTPS地址、认证名称引用、能力和限额；live必须false、重试为0。
使用独立合成structured-v2请求，验证任务/系统/原生工具配对和宿主tuple保留，不将Fake预约改标签。
编码body/profile/逻辑请求指纹与未发送的wire事实分开；认证环境变量canary没有被解析或输出。

完整文本/function_call可解码；错误HTTP、部分/错误终态、未知工具、参数错误、reasoning/refusal/
多模态/托管工具等不支持输出均明确拒绝。原body无损hex保留，错误UTF-8仍可成为原始证据。
缺失用量保持null，已知分量不得大于已知总量，费用不推算。完整记录指纹检测本地意外变异，
不是提供方签名、网络凭证或Store授权。

### R3.2：有界 SSE 回放

完整Unicode/BOM/CRLF夹具验证**每个两块拆分位置**，另有逐字节、三种换行、多行data、注释、
交错工具、序号/身份/生命周期、完整快照、错误UTF-8、重复键、全部容量边界及尾部事件回归。
这些循环共享一个具名用例，不虚增独立测试数量。

取消和deadline由输入块的时间/标记确定；不sleep、不创建网络线程。未完成、取消、超时、协议错误
都清空可用text/tools，保留原tuple。完整终态已通过共享decoder的计量，即使随后流异常仍保留；
无效终态不成为该恢复路径的计量来源。消费的原始块与解析到的字节分别计数；CLI文件SHA和已消费流SHA
覆盖范围明确分开。`completed`只表示协议子集解码完整，宿主仍须检查当前Store、Schema与权限。

样例域名为`gateway.example.invalid`，模型名、ID、响应、计量和全部SSE事件均为合成fixture。
新增入口的网络请求、付费attempt和工具派发均为0，不生成真实wire/attempt记录。

## 4. 开发过程中的失败与修正

- 初次选择的Python缺jsonschema，改用已配置venv后配置成功；此工具返回错误不计为测试失败或成功，也不冒称已归档日志。
- 两次开发构建分别遇到新C++测试的Boost JSON/string比较编译错误，改为显式类型比较；原失败构建日志保留。
- 首轮CLI把既有小写status误写为大写期望，并把词法目录末尾斜线当成不等路径。修正测试期望，保留原源码和失败日志；生产序列化未为测试改变。
- 一次证据复制权限错误使`dev-focused-fixed`重复了原期望；其失败日志仍保留，不当作已修正结果。
- 交叉审查发现本批中间CLI收到非UTF-8 argv路径时，原读取错误的JSON序列化会再次抛异常。真实子进程红测保留（协议进程SIGABRT；doctor输出非JSON）；新增参数4096字节/UTF-8和路径读取前检查后，红转绿及全部正式门禁通过。此复现针对本批中间实现，未冒称对上一发布基线执行过。

路径修补同时拒绝协议命令无关的`--source-root`，不再静默消费；有效请求仍无需仓库配置、SQL或Store。
所有失败记录与成功记录分别保存，不用早期绿测替代后续变更的验证。

## 5. 重复执行与原始证据

从普通用户执行，选用含`requirements-test.txt`依赖的Python：

```sh
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/path/to/python-with-test-dependencies
cmake --build build-cmake --parallel 2
umask 0002
ctest --test-dir build-cmake --output-on-failure -V
umask 0022
ctest --test-dir build-cmake --output-on-failure -V
```

ASan/UBSan分别配置`Debug`及`-DWLS_ENABLE_ASAN=ON`/`-DWLS_ENABLE_UBSAN=ON`。
本地重点选择器为：

```text
^(doctor_diagnostics|protocol_codec|sse_replay|doctor_diagnostics_cli|protocol_replay_cli|sandbox|worker_tools|s12_cli_smoke)$
```

完整实际命令和环境以包内`gate.py`、各JSON记录及原日志为准；示例CLI见README和实施契约。

- [机器结果](r2a3-r3-offline-results.json)：SHA `aeadb67a9618662bba65c345e5d3d559305f318de0b90225ce8dc268894eaad7`。
- [255项输入清单](r2a3-r3-offline-inputs.sha256)：SHA `a11c6cc0edaaac489e57a3d400c2022e19736d5ea8c5b50ac658ce2f712073dd`。
- [证据清单](r2a3-r3-offline-evidence.json)：逐文件大小与SHA，SHA `fef9905d32ec2c9b250396436f66656b5f32264cdafbdb946ed0107e4f7b5adc`。
- [原始日志与环境包](r2a3-r3-offline-evidence.tar.gz)：47个文件、174903字节，SHA `ccece51e1d4f605305d1faaffeb15b773b80d6c70451e12ff6606c32f6773317`。

证据包保留每轮原日志、两套完整LastTest、重点sanitizer LastTest、初始CLI测试源码、执行runner、
输入清单、环境及二进制摘要。逐个archive条目重新读取验证字节；不提交构建二进制、数据库、
fixture生成器可执行文件或凭据。机器结果不嵌入尚未存在的最终发布SHA，避免自引用伪造。

## 6. 最终远端门禁与剩余工作

最终发布必须核验**包含本报告的具体提交**上六路完整矩阵：严格GCC/umask0022、
严格GCC/umask0002、Clang18、UBSan、CMake ASan/LSan、Make-ASan，以及同SHA源码快照。
中间设计/代码提交的成功、缺少作业或取消运行都不能替代这一门禁；最终SHA、运行链接与实际结论
在本次交付中给出，原始状态可从[GitHub Actions](https://github.com/wmqfl861/wls861/actions)核验。
本报告正文记录冻结的本地事实，仓库报告所在HEAD与其CI决定最终发布结论。

R3.3实际HTTPS/TLS、实时取消、profile冻结与Store原attempt/wire/预算结算、供应商能力及获准真实
小任务另行验收。强bwrap/cgroup部署正向、独立第二模型业务审核、私有行purge和并发租约仍开放。
本批同模型协作交叉审查不冒称第二模型业务审核；原有Fake流程、真实文件/SQLite/监督夹具与协议
合成样本分别记证据，不据此宣称模型任务质量或完整S1真实闭环完成。
