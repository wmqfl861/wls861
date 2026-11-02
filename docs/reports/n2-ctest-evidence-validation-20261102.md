# N2d 完整 CTest 原件与公开投影

时间截点：2026-11-02 00:43 UTC。原始基线
`f3fc9bfaf345030ee994097d2a2443e3c290e9fc`，工作分支
`test/n2-permanent-evidence-20261102-0k9`。本提交只修改 runner、自检和当前契约；
不修改产品源码、CTest 目录/期限、ASAN_OPTIONS、编译 flags 或 CTest 输出截断设置。
`historical_clang_asan_failures_closed=false` 保持。

## 修正与边界

旧共享 runner 的成功 JUnit 可截断输出，且 artifact 没有完整 LastTest。新版本在成功、失败及
能够收尾的中断后保存原 `LastTest.log`、存在的 `LastTestsFailed.log` 与中断 `.tmp`，保留完整
JUnit、命令退出回执、源码/构建前后身份和原始 stdout/stderr。原件复制使用排他创建，
不改写 CTest 原始日志。缺必需原件、缺完整退出记录、启动失败或中断均不能得到 PASSED。

全部原件放在新 build 的 `private-evidence/`，原 native 文件仍在 `Testing/Temporary/`。
公开目录独立为 `public-evidence/`：静态测试名称和数值结果白名单、连续 byte-range 投影、
来源/公开 hash，以及不含动态异常值的结构摘要。完整原件不会直接进入当前自动 artifact 路径。
公开包只在 pending 目录内经过文件集/字段/hash/range/log/JUnit guard 后才被命名发布。
未脱敏的范围保持逐字相同；JUnit 公共副本明确为状态投影，完整 XML 保留私有。

动态 Doctor/config/body/header、凭据存在性、原 argv 路径和异常正文不进入公共摘要或索引。
此白名单检查是限定的工具策略，不是对任意数据的无限秘密扫描。CI 临时机器上的私有原件
不经公开 artifact 自动上传；不能据此声称它们在 runner 销毁后仍有外部私有持久副本。
需要外部私有归档时，必须使用另行明确的受控存储，而不能扩大公开上传范围。

CI 所需最小整合为 `.github/workflows/ci.yml` 原 ASan artifact 步骤的路径从
`build-gcc-asan-*/evidence/` 改成 **`build-gcc-asan-*/public-evidence/`**，并把步骤名称说明为
approved public projections。本子代理没有修改 CI 所有权文件；已向主执行 Agent 交付两行 patch：
`evidence/n2/ci-public-upload.patch`。未应用此补丁的 CI 会找不到新公开目录，不能记为上传完成。

## 实际验证

普通 UID 1000，数据盘 `/mnt/d`，Python 为现有
`/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python`。
测试 TMPDIR 为本批 `evidence/n2/tmp`。工具自检实际运行：

```sh
TMPDIR=<本批数据盘>/evidence/n2/tmp PYTHONDONTWRITEBYTECODE=1 \
  /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B tests/test_run_gcc_asan.py
```

最终 **38/38 工具方法通过，exit 0**，unittest 5.853s；原日志与精确 argv/输入见
`evidence/n2/runner-round03/{stdout.log,stderr.log,receipt.json}`。
前一隐私版本 37/37 通过，后续增加 JUnit 根文本拒绝守卫和对应第 38 方法，再执行最终自检。
更早的 32 方法原件保留于工具调用记录；不将这些工具次数计入产品 CTest 或 sanitizer 结果。

这些自检包含实际 CTest 调用及合成 Python 子进程的成功（输出 >1024 bytes）、失败、
确定性信号屏障中断、真实启动失败；同时以明确标识的 mock 命令流程检查主 runner 的
成功/失败/中断收尾、前后身份和终态。没有编译假二进制冒充产品 ASan。
公开 guard 对已知合成 Doctor/config/body/header 文本为红；额外 raw 文件、重算 hash 后的
动态 log 和 JUnit 根文本也被拒绝；精确生成包为绿，原 private bytes/hash 和公开范围同时检查。

旧版反例采用 `git show f3fc9bfaf345030ee994097d2a2443e3c290e9fc:scripts/run_gcc_asan.py` 的实际原文件，运行相同的
`GateContracts.test_successful_main_keeps_complete_original_lasttest_and_index`。
旧 main 返回成功、身份未变、源 LastTest 超过 1024 字节后，断言 **原 LastTest 未被保存** 失败：
**1 方法 / 1 failure，exit 1**。不是启动失败、编译错误或新 API 缺失。
这是 runner 主流程的合成命令自检，不能误称完整产品 ASan 红例。
完整日志/原 runner/测试源码/命令见 `evidence/n2/runner-baseline-red/`。
该反例方法及其 `exercise_main` 夹具在最终版本的 AST 完全相同，另有
`counterexample-equivalence.json`；最终增加的无关第 38 方法不改变该反例条件。

最终执行输入 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `scripts/run_gcc_asan.py` | `c639df7bb5d1f0ad07795e64558976d15fd117ddab123365040c332009c990d2` |
| `tests/test_run_gcc_asan.py` | `6fbb814f328480774a7bf4f4a5705743e745212d29d7fb16ce09deac21edbf2d` |
| 旧 runner | `654fc19fb84732aeb998f7b175482e5f780b67ba3fbd2471e75a0143f01e83bd` |

该工具模块本地自检已完成；完整 GCC ASan 产品门禁 **NOT_RUN**，原本机失败仍 OPEN。
C++ N2a/N2b 的独立 Release 定向执行、N2c 五方法、集成 CI 和非作者复核由统一执行另记。
本子任务 `PUSH_COMPLETE=false`、`REMOTE_CI_COMPLETE=false`、`PR_REVIEW_COMPLETE=false`、
`MERGED=false`；整体 `LOCAL_VALIDATION_COMPLETE=false`，未宣称批准合并或产品发布。
