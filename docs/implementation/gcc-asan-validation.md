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
  --expected-tests 89
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

所有证据保存在新构建目录的 `evidence/`；CI 即使失败也尝试上传该目录。
配置、构建、测试各自记录 argv、cwd、起止时间、真实退出码、stdout/stderr 原字节及 SHA-256。
运行前后保存全部 tracked 源文件的字节/模式、HEAD、实际二进制/静态库和关键构建文件身份。
非零 CTest 同样执行末尾身份核验；SIGKILL 等不可捕获终止只留下未完成回执，不能算通过。

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
