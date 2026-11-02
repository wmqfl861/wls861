# GCC 13.3 Debug ASan/LSan 验证入口

本入口对齐 GCC/Debug 的构建及测试方式，不修改运行时、原测试期限或历史失败结论。
Clang 18 `Debug -O2 -g` 与 GCC 13.3 `Debug -g` 是不同构建条件；即使后者通过，
也不能据此认定前者超时的唯一根因，或关闭原 `live_workflow` FAIL 观察。

## 执行

使用普通 Linux 用户、Python 3.11+、已有测试 venv、GCC **13.3.0**、CMake/CTest 3.28+
及当前项目的原依赖。脚本不安装包、不 sudo、不挂载 RAM、不联网拉取代码、不推送或合并。
在已核验且干净的数据盘 checkout 内运行，例如现有服务器的 `/mnt/d`：

```sh
python3 scripts/run_gcc_asan.py \
  --data-root /mnt/d \
  --compiler g++-13 \
  --python /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python \
  --expected-tests 92
```

`--python` 指向实际已安装 `requirements-test.txt` 的解释器；保留 venv 的符号链接路径，
不能解析成系统 Python。`--data-root` 要求该路径是非根挂载点且 checkout 位于其下，
不会因为目录恰好叫 data 就视作数据盘。CI 使用 runner checkout，不表示用户服务器数据盘已验证。

每次创建源码根下全新的 `build-gcc-asan-<UTC>-<随机值>/`，不复用旧 Clang/GCC cache、
不清理其他工作树；可执行文件保持原 executable-relative 源码布局。临时测试库、构建与
证据均在该目录中。源码树不干净、root 执行、编译器版本不符都会拒绝，不自动换编译器。

配置固定为 GCC 13.3.0、Unix Makefiles、Debug `-g`、ASan ON、UBSan OFF、
严格警告和 `-Werror`；不加入 `-O2`、`NDEBUG` 或 compiler launcher。
构建并行度 2，完整 CTest 并行度 1；构建 umask 0022、测试 umask 0002。
环境只继承基本路径/用户/语言设置，不继承供应商凭据、CXXFLAGS、LD_PRELOAD、
LSAN_OPTIONS 或其他 sanitizer 覆盖。`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`。
同时记录的 UBSAN_OPTIONS 不表示本次启用了 UBSan 插桩。

## 通过条件及证据

完整原件保存在新构建目录的 `private-evidence/`，不得将该目录配置为公开 CI artifact。
配置、构建、测试各自记录 argv、cwd、起止时间、真实退出码、stdout/stderr 原字节及 SHA-256。
运行前后保存全部 tracked 源文件的字节/模式、HEAD、实际二进制/静态库和关键构建文件身份。
非零 CTest 同样执行末尾身份核验。源码快照、HEAD、工作区清洁度和构建身份分别尝试采集，
一项失败不会跳过其他项的末尾证据；任何不完整或变化都不能得到通过状态。

成功、失败以及能够收尾的中断均复制原 `Testing/Temporary/LastTest.log`，并复制存在的
`LastTestsFailed.log`；中断遗留 `.tmp` 独立保留，不能改称完整日志。原 JUnit 不改写，
成功输出原有截断配置不变，完整逐方法原文以 LastTest 为准。`ctest-evidence.json` 索引原
文件的路径、字节数、hash、命令终态和前后身份。缺 LastTest/JUnit/命令退出记录会告警，
状态保持 NOT_COMPLETE；即使意外留下形似完整的文件，启动失败/中断也不能计为通过。

公开上传只能选择 `build-gcc-asan-*/public-evidence/`。该目录在封闭临时目录
`public-evidence-pending/` 内生成、通过隐私 guard 后才命名发布；CI 必须精确选择前者，
不能上传构建目录、私有目录或 pending 目录。没有可收尾结果时允许无公开 artifact，
不得从 artifact 缺失或之前的绿色推导本次通过。

`private-evidence/summary.json` 在公开投影返回前保持 NOT_COMPLETE。公开摘要来源为
排他创建后不再改写的 `summary-publication-input.json`，其状态是拟发布结果，不是最终
进程退出回执；公开索引指向该原件的实际字节/hash。只有发布成功返回后才写入最终摘要。
能够收尾的异常/中断会使最终摘要为 NOT_COMPLETE，并尝试将本次已命名的公开目录移到
`public-evidence-incomplete-<随机值>/`，保留原字节但不匹配正常上传路径。若隔离本身失败，
仍独立写入最终 NOT_COMPLETE、`public_quarantine_error` 和 `failed_ci_required=true`，
进程返回 1；此时可能残留已检查的候选公开目录，不能从其内容推导进程或 CI 成功。
SIGKILL 等不可捕获终止无法保证任何时刻都完成这些收尾动作，必须结合真实进程退出码及
外部 CI 结论判断；发布输入、单个摘要或 artifact 本身均不单独证明进程 exit 0。

本机原件可持久保存在数据盘；hosted runner 的私有原件仅存在于该 job 文件系统。
没有已授权的外部私有存储时，远端 job 销毁后的原件可获取性为
**`NOT_PRESERVED_DURABLY`**。公开 artifact 是投影及原 hash，不能称为可下载的完整原
LastTest/JUnit/命令日志，也不能仅凭 hash 声称仍能取得原件；本入口不新建存储或上传 raw。

公开投影使用当前冻结测试源码中的静态 native 方法名称、Python unittest 方法/类名、
已审阅 CTest 目录名称及数值结果/期限白名单。匹配的安全方法行原字节保留，FAIL 的静态
方法名保留而动态异常尾部隐藏；其他输出整段隐藏。Doctor 对象、配置全对象、原始正文、
headers/凭据存在性和 traceback 动态值不会被复制进公开日志或索引。
每个投影记录私有逻辑来源、原字节数/hash、公开字节数/hash及完整连续的源/目标字节范围；
`verbatim` 范围逐字不变。JUnit 公开副本只包含原 testcase 名称、状态、时间和失败/跳过标记，
它明确标为结构投影，不冒称原 XML；完整 XML 与命令仍留私有。

公开包包含经过字段检查的状态摘要、原源码/二进制身份清单和原目录期限，
不包含原 argv 路径/异常值或构建 cache 内容。隐私 guard 拒绝额外文件、未知字段、
不匹配 hash、非连续范围、动态日志和额外 JUnit 输出。工具自检用已知的合成
Doctor/config/body/header 拒绝样本作红例，精确生成的包作绿例；这不宣称无限范围秘密扫描。

公开 JSON 在索引、身份及结构投影的每一层拒绝重复键、NaN/Infinity/-Infinity 和解析后
溢出为无限值的浮点数，不能让默认解析器忽略前面的隐藏值。公开 JUnit 除字段/text 检查
外，还要求原始字节与本入口生成器的精确 XML 序列化一致；注释、处理指令、DTD 等被
默认 DOM 解析丢弃的语法不能混入待上传原文。guard 失败保留 pending 与 private 原件，
不生成正式 public 目录，也不降级上传 raw。

通过需要完整注册数与 `--expected-tests` 相符、没有 disabled 项、CTest exit 0，
JUnit 逐项与注册名匹配且没有 failure/error/skipped/notrun，以及输入和构建身份保持。
没有 `-R/-E`、重复直到成功、期限放宽或 `--rerun-failed`。原 89 项及其 TIMEOUT 不改。
新增 Python runner 自检不计入这 89 项 C++/Python 产品测试，也不算产品 ASan 证据。

后续版本还将注册名称和每项 TIMEOUT 与受版本控制的
`tests/gcc_asan_expected_tests.json` 比较。同样数量的测试替换、漏项、重复和期限变化
均不能仅凭计数通过。新增产品测试时，必须审阅 CMake 注册差异后，同时更新该清单、
`--expected-tests`、默认值及 CI 参数；不能从当前 CTest 输出自动接受一个新集合。
本次清单固定原 89 项；新增守卫自检共 23 项，与产品测试分开计数。

`test_live_workflow.py` 的命令行 runner 在原失败/错误登记后立即输出并 flush 原 traceback。
这不改变异常、断言、测试顺序、failfast 或期限。`tests/test_immediate_unittest.py`
用确定性信号中止一个已失败而仍在执行下个用例的子进程，验证原因在中止前已经落入日志；
其 4 项工具自检也不计入产品 CTest 数量。旧 Clang 历史失败仍必须单独诊断。

`summary.json` 的 PASSED 只表示**该次 GCC gate**通过。字段
`historical_clang_asan_failures_closed=false` 始终保留：用户原服务器的 Clang 29/39、
十项超时和整组内 FAIL 不能由另一机器、另一编译器或单项重跑改写。
全项目采纳、原环境失败处置、PR 正式审核、合并和真实产品验收分别决定。

CI 仅将原 CMake ASan 矩阵项接到此入口；另外五路与原 Make-ASan CPPFLAGS 目标保持。
这是共享验证配置，而不是已经证明了某个生产缺陷的修复。

本批整合新增 3 项注册：`async_coordinator` (180s)、`r3_offline_acceptance` (30s)、`r3_offline_acceptance_cli` (600s)。当前完整集合 92 项；原 89 项期限保持不变。
N2 增加原 LastTest 保留/缺失拒绝、实际轻量 CTest 成功/失败/中断/启动失败、
主流程前后身份、公开投影及隐私拒绝自检。实际方法数与命令结果见本批报告；工具方法不增加产品注册数。
