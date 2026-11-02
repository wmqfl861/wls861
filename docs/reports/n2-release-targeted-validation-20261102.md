# N2a/N2b Release 定向执行收据

本记录补充 [实施截点](n2-permanent-regressions-20261102.md) 当时的 C++ NOT_RUN，
不覆盖原截点。实际执行为 2026-11-02 00:35:51–00:45:19 UTC，
源码固定于 **`ce7798d7979801f5319ae7a79636dfdd002af2d2`**，
生产祖先仍为集成基线 `f3fc9bfaf345030ee994097d2a2443e3c290e9fc`。
本次没有修改或重写已确认的 AMEND/资源调度生产修复。

主执行 Agent 明确授予独占 Release 编译/测试槽，本子任务结束后已释放。
源码采用数据盘内新的 detached worktree，避免未提交 runner 工具变动混入 C++ 输入：

```text
/mnt/d/codex-workspace/wls861-n0-n4-20261102-0k9_2wlf/n2/build-n2-release-k13ssbza/source
```

构建为该目录下 `build-release/`，临时目录为其上层 `tmp/`。
UID/GID 1000、GCC 13、Release `-O3 -DNDEBUG`、C++20 和原严格警告/`-Werror`；
ASan/UBSan OFF。构建 umask 0022，测试 umask 0002。非完整双 umask 矩阵。

实际配置、构建、目录核验与测试均 exit 0；完整 argv/cwd/时间/raw stdout/stderr/hash
位于本批外部证据 `evidence/n2/build-n2-release-k13ssbza/private/` 的逐步 receipt。
主要命令（路径缩写仅供阅读，原件记录完整绝对路径）：

```sh
git worktree add --detach <source> ce7798d7979801f5319ae7a79636dfdd002af2d2
cmake -S <source> -B <source>/build-release \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DWLS_ENABLE_ASAN=OFF -DWLS_ENABLE_UBSAN=OFF \
  -DPython3_EXECUTABLE=/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python
cmake --build <source>/build-release --parallel 2 --target async_coordinator_tests store_tests
ctest --test-dir <source>/build-release --show-only=json-v1
ctest --test-dir <source>/build-release --output-on-failure --parallel 1 \
  --no-tests=error --output-junit <private-evidence>/ctest.xml -R '^(async_coordinator|store)$'
```

| 注册 | 结果 | 内部方法 | JUnit 时间 |
| --- | --- | --- | --- |
| `store` | PASS | 34 passed / 0 failed | 4.92981s |
| `async_coordinator` | PASS | 31 passed / 0 failed | 18.2262s |

合计 **2/2 CTest**，0 failed/timeout/skipped/notrun；CTest 进程 elapsed 23.1908s，exit 0。
目录检查完整比较 **92 个名称及原 TIMEOUT**，与已审阅清单相同；没有将 65 个方法另算为 suite。
这证明本批四个新增方法在当前生产基线上通过，属于新增常驻覆盖；没有伪造历史生产红例。
原 runner 缺日志红例与 38 项工具自检属于[另一模块记录](n2-ctest-evidence-validation-20261102.md)。

805 个 tracked 源码输入的字节/mode 和 7 个构建身份前后相同，测试源码工作树仍干净。
执行后精确测试程序路径检查无遗留进程。原 LastTest 为 10,685 bytes，完整保留；
公开静态投影为 10,082 bytes，65 个 START、65 个 PASS 和 2 个 RESULT 均逐字保留。
公开投影守卫 PASS，原/公开 hash 及连续范围另列
`evidence/n2/build-n2-release-k13ssbza/public-observations/projection-receipt.json`。
该投影明确是本次 Release 定向观察，不是共享 ASan gate artifact。

| 身份 | SHA-256 |
| --- | --- |
| `async_coordinator_tests` | `0d6d494d487331e65b2b272a885adbf45930840450d1ee5a11d57167a20b42ca` |
| `store_tests` | `0620df5e7fd0eaf421a1e217fc3051c8a735f18a9309dfc6b9ac55e0b9158ca7` |
| 原 `LastTest.log` | `199c3500237364fc510b33d3dc4c79bf164f3f421a9ebe421cb85c65ecc0eebe` |
| 静态投影 | `929bfec4da70c81a127dfcb63e62c1703cf8c437cd43a048930a2022f2440503` |
| 源输入清单 | `53b83a22bff3a8c7b49c5906bc208150c2e997c0fc846786c0fb97ced29f08a2` |
| 构建身份清单 | `1b12e04717f9cb002a58465e2111503e830f9ede7862d7389960677cbe657e97` |

`private/SHA256-manifest.json` 冻结原执行文件，`completion.json` 记录完整终态。
`post-run-audit-v2.json` 精确列出两个实际目标的配置命令；初版 audit 的后缀匹配曾把其他
包含 `store_tests.cpp` 后缀的 **计划** 编译命令也列入，原初版保留，不将它们称为实际已构建目标。
构建/运行 receipt 始终只有上述两目标、两注册，无额外运行或结果替换。

本次尚未执行 `event_store/runtime_types/r24_recovery/scheduler`、完整 Release 两种 umask、
ASan/LSan、UBSan、Clang 或最终集成候选远端 CI；由主执行 Agent 统一安排。
新的 runner 提交与文档是本输入的后继，因此最终集成源码还需自己的完整门禁。
当前构建可供读证据或后续诊断使用，不能替代最终候选 fresh build。
本子模块实际定向验证已完成；整体 `LOCAL_VALIDATION_COMPLETE=false`，非作者复核与
发布/精确 CI/正式审核/合并均未由本子任务完成。
