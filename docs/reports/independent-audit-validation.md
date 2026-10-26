# 0.8.6-r3.2 独立审核修复验证

问题与修复见[独立报告](../reviews/independent-2026-10-26.md)，设计见
[ADR-079](../adr/079-independent-boundary-review.md)，后继见
[R3.3/R1.4计划](../implementation/R3-controlled-live-plan.md)。本文只登记实际执行，
不把计划或局部通过写成产品验收。蓝图仍为v0.4.1，Store仍408；没有真实模型或付费调用。

## 1. 源码与输入绑定

| 项目 | 核验值 |
| --- | --- |
| 发布基线 | `72f74d6fd9261fab2fb372370080b93006fc4fdb` |
| 本地最终代码提交 | `6e2acef84edbf0985fa60d72770baa4d1f601bef` |
| 对应GitHub代码提交 | [64d870d0c29594743d734d9c3b9b24587c77e608](https://github.com/wmqfl861/wls861/commit/64d870d0c29594743d734d9c3b9b24587c77e608) |
| 两者完整tree | `7580c032842cd48951e834f0185e1ceddb76a991` |
| 构建、测试、运行资产输入 | **258项**，逐项匹配代码提交和工作树 |
| 输入清单SHA256 | `89cb77f080bd863de05b483fc76263b98d3152994e4f34886412965a9a4432d8` |

258项包含前一批255项及原有三个contract/integrity Python脚本；原handoff/审核档案另按
其校验器和基线字节核对。最终构建前保存清单，测试后再核Git字节。所有输入修改时间早于
最终三配置构建开始；时间只是辅助观察，身份由SHA256和Git tree确定。之后只追加文档和证据。
本地与GitHub提交元数据不同，源码tree一致。未修改SQL、vendor、原handoff、已有审核文件或workflow。

## 2. 最终普通用户门禁

正式构建和测试均为UID/GID **65534**，保留C++20、严格警告和-Werror。
Release完整63组分别以调用umask0002、0022执行。本地sanitizer覆盖12个直接受影响组，
全量工具链另外核验最终发布SHA的远端矩阵。

| 门禁 | 实际结果 | runner耗时 | 原日志SHA256 |
| --- | --- | ---: | --- |
| final-release-0002 | 63/63，exit0 | 174.920 秒 | `95290bd6ae35035b2b06e1d400d9489240ebb5252279d33d9e4054102de2536d` |
| final-release-0022 | 63/63，exit0 | 175.912 秒 | `be15e7437282e41b1489f5728b07b160b807daf389fd18c92aa67043bb1a24cf` |
| final-focused-asan | 12/12，exit0 | 213.573 秒 | `add33176a3993084497466c612dd0242f35ff5c8dd2f992591f1fdde09bfcc08` |
| final-focused-ubsan | 12/12，exit0 | 109.093 秒 | `6b24542dda6dff5ed6165c0b3c0f86246da5a8fc9a37488b3c8550dfcec3480b` |

完整Release每次：M1 C++52、S1 C++573、CLI188、Python参考契约38、合成demo3，
共854项计数用例，另有2组原件完整性检查。这些不同层次的测试不能合称854项产品验收；
重复umask/sanitizer不增加独立用例数。

新增**16个注册用例**：doctor2、cleanup3、sandbox1、SSE3、配置读取1、运行输入读取1、CLI5。
原SSE全切分用例增加默认填充，原doctor CLI特殊输入用例增加真实FIFO打开观察，二者未重复计数。
12组sanitizer为config_loader、offline_runtime、sandbox、worker_tools、cleanup、cleanup_runtime、
doctor_diagnostics、sse_replay及doctor/protocol/s12/review四组CLI；每次171项C++、47项CLI。
地址/泄漏与未定义行为检查未报告诊断；不称为本地完整sanitizer套件。

**有效umask：**doctor C++及doctor/protocol CLI保留调用mask；共享C++ test_support和review CLI
按原夹具规则在案例内设置0077，部分sandbox夹具另测子进程0002。不能从调用mask推导全部有效mask。
原超时保持：一般90秒、cleanup_runtime180秒、capacity_repro300秒，CI30分钟。

## 3. 基线失败与开发过程

| 反例 | 原实现实际结果 | 修复验证 |
| --- | --- | --- |
| doctor运行源FIFO与伪空文件 | 30通过/2失败，实际IN_OPEN与非空零声明长度proc文件 | 最终doctor32项及sandbox EOF预算 |
| 合法Workspace名称清理 | 20通过/1失败；另两个守卫当时未加入 | 最终cleanup23项 |
| 默认SSE obfuscation | 最终同源测试27通过/4失败，含加强旧例及3新例 | 最终SSE31项 |
| CLI输入/错误 | 14项中2失败、2错误；非法key原先成功，命令SIGABRT，长消息缺截断事实 | 最终review CLI14项 |
| 共享运行输入读取器 | 9通过/1失败，FIFO产生IN_OPEN | 最终offline_runtime10项 |
| 共享配置读取器 | 18通过/1失败，FIFO产生IN_OPEN | 最终config_loader19项 |
| doctor manifest/config入口 | 原12注册项内两个subtest失败 | 同一12项纳入最终CLI/sanitizer门禁 |

旧库相关源码与发布基线逐字匹配；红测CLI的SHA与上一批保存的二进制证据一致。
初次共享构建在测试文件编辑期间遇到重复函数定义并失败，修正后重建；原日志保留。
第一次两轮Release和十组sanitizer通过在共享reader补齐之前，只作开发阶段证据；
最终四道门禁对补齐后的同一258项输入重跑，不复用旧成绩。新增守卫不都声称在基线失败。
早期文件系统编译的精确时间未记录，metadata保持null；记录运行另实测身份/mask/时间/hash。
一次临时清单写入因目录所有权失败，改由目录所有者保存，生产权限未放宽。

## 4. 命令与原始证据

以下命令须普通用户执行；完整argv、cwd、UID/GID、调用mask、epoch和退出码见机器结果及归档。

```sh
cmake -S . -B /tmp/wls861-audit-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/tmp/wls861-validation-venv/bin/python
cmake --build /tmp/wls861-audit-build-release --parallel 2
ctest --test-dir /tmp/wls861-audit-build-release --output-on-failure
```

ASan使用Debug/WLS_ENABLE_ASAN=ON、halt_on_error=1:detect_leaks=1；
UBSan使用Debug/WLS_ENABLE_UBSAN=ON、halt_on_error=1:print_stacktrace=1。
实际工具链GCC13.3.0、GNU ld2.42、CMake3.28.3、Ninja1.11.1、OpenSSL3.0.13，
Python3.12.14/jsonschema4.10.3；二进制摘要与实际版本见environment记录。
原CTest保留主机时区标签，runner时间为epoch，不跨环境比较时钟或速度。

| 文件 | 内容 |
| --- | --- |
| [independent-audit-inputs.sha256](independent-audit-inputs.sha256) | 258项源码/构建/测试/运行输入 |
| [independent-audit-results.json](independent-audit-results.json) | 门禁、计数、环境、过程元数据 |
| [independent-audit-evidence.json](independent-audit-evidence.json) | 每个成员的字节数/SHA256 |
| [independent-audit-evidence.tar.gz](independent-audit-evidence.tar.gz) | 99份原日志、元数据、测试副本和采集脚本 |

归档203713字节，SHA256 **`da1961189e7493d4f2eddde51f8662ff9693aac887209e3f40634e8d3ffe15f3`**。
逐项读回核验全部成员；不含构建二进制、临时DB、核心转储或凭据。

## 5. 最终发布与边界

最终文档提交改变commit SHA但不改变258项输入。发布须核对该最终SHA的strict、
strict-umask-0002、Clang-18、UBSan、ASan、Make-ASan及source-snapshot，
中间代码CI不能替代。交付回复给出最终SHA、实际run链接与结果，此处不预填尚未完成的远端成绩。
没有现场供应商/模型质量费用/强bwrap-cgroup正向/整机掉电/任意同UID对抗或第二模型业务验收。
附加用量核对、管理视图、大树容量、服务化和private-row purge仍为后续工作。
