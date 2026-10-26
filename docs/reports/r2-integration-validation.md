# R2.2 接续联合复验

记录日期：2026-10-25（America/Los_Angeles）。
被测原始提交：`c975be3e7d30c1208a0a2c19494e2a186c7f77ca`，版本0.8.1-r2.2。
该分支原 STATUS 引用了本报告，但文件未提交；本次依据实际源码重新验证并补齐，
不将 R2.1 历史报告或聊天状态当作联合验收结果。

## 原始源码与环境

通过 git archive 从上述精确提交生成隔离源码快照，以普通用户 UID/GID 65534 构建和运行。
Python3.12.14、jsonschema4.10.3、CMake3.28.3、Boost1.83、SQLite3.45.1、OpenSSL3.0.13。
GCC主版本为13.3；环境准备时系统发行包从 `13.3.0-6ubuntu2~24.04` 更新到 `~24.04.1`，
发生于此次基线构建期间。因此本基线作为接续复现证据；稳定工具链下的最终发布门禁见
[R2.3报告](r2-model-amendment-validation.md)，不把基线冒充统一工具链的最终发布矩阵。

## 实际执行

```sh
umask 0002
cmake -S SOURCE -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/tmp/wls861-validation-venv/bin/python
cmake --build BUILD --parallel 2
ctest --test-dir BUILD --output-on-failure -j2
```

实际 SOURCE/BUILD 和命令退出码记录在 `r2-integration-results.json`。
配置、构建和CTest均退出0；构建136.10秒，测试66.55秒；**52/52组通过**。

| 范围 | 通过 / 失败 | 含义 |
| --- | --- | --- |
| S1 C++ | 414 / 0 | Store、运行器、文件/进程证据、版本/权限等临时夹具 |
| S1 CLI | 134 / 0 | 真实跨进程CLI/SQLite/临时文件，包括6项联合隔离场景 |
| M1 C++ | 52 / 0 | 原内核兼容；单独计算 |
| M1演示 | 3 / 0 | 原合成演示；不充当S1产品验收 |
| Python参考契约 | 37 / 0 | Schema与SQLite参考语义 |
| 交接/审核原件 | 通过 | 原始文件指纹检查 |

完整CTest摘要保存在 `r2-integration-ctest.txt`，SHA-256：
`31c185e1eaa962cc9ccff9a8d5498378d1c65caa7253868ee340060ddf3a8c6a`。
机器可读结果附逐套件计数；不将不同层级加总为产品验收数量。

## 覆盖及限制

原拒绝纠正、共享返工额度、用户升级/回答与请求限定恢复已联合通过：修改/回答/准入/重放
不改变兄弟请求行、等待和终态收尾租约；旧响应、UNKNOWN和未处置AMEND仍被围栏。

本基线没有模型AMEND能力。R2.3新增CLI反例在此提交先失败，修复及红绿证据另见R2.3报告。
这份报告不声称R2.2已运行全部 sanitizer/Clang/Make门禁；最终交付候选运行其完整矩阵。
未调用真实供应商，未使用生产数据，也没有强bwrap/cgroup部署或独立第二模型评审结论。
