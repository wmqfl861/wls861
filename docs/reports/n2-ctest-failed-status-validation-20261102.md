# N2 CTest 失败状态公开投影修正

非作者在 candidate `3ce1e218731742d2a88fb5f08b77bb20f1aad835` 的冻结 runner
`b53f132902d8c989e88f55cdfff4873a5d3001ca1d080e7bf639b13bc84a3360` 上确认：
CTest 3.28 的实际 JUnit 对普通失败和 Timeout 都使用合法 `status="fail"`。
原 code08 ASan 日志为 48 run / 44 fail，控制台对应 48 Passed / 2 Failed / 42 Timeout。
publisher 和公开 guard 只接受 run/notrun，使合法失败运行无法生成公开证据包。

此前 51 项工具自检没有覆盖公开 publisher 对真实 `status=fail` 的处理：合成 main
失败夹具仍写 status=run；真实轻量 CTest 失败检查只核对私有原件保留。原 51/51 记录
维持其范围，不改写为覆盖本缺陷。独立最小红例保存于本批私有
`evidence/review-ctest-status-fail-3ce1e218/`：3 方法，3 assertion failures，exit 1。

最小修正让 publisher 和 guard 接受 fail，并要求 fail 投影保留 failure/error 通用标记。
原始 XML、failure message/正文和来源 hash 保留私有；公开诊断仍为固定文本。
PASSED 公开摘要必须对应完整、无重复且没有失败/跳过/未运行标记的 JUnit 集合。
原 `check_junit` 和主流程成功条件未放宽，合成 CTest exit 0 搭配失败 JUnit 仍为
NOT_COMPLETE。产品源码、权限、期限、编译/sanitizer 条件和 CI 均未改。

新增 6 个永久工具方法覆盖普通失败、超时、exit 0 配失败 JUnit、guard 接受合法失败、
拒绝 PASSED 摘要与非通过 JUnit 并存，以及拒绝无失败标记的 fail。普通 UID/GID 1000，
数据盘 `/mnt/d`，既有测试 venv；以下都是工具自检，不是新的产品或 ASan 执行。

| 执行 | 私有证据目录 | 实际结果 |
| --- | --- | --- |
| 精确 b53 runner，相同最终新测试文件 | `evidence/n2/runner-b53-failed-status-final-red/` | 6 方法，8 assertion failures（含子例），exit 1，0.979s |
| 修正 runner，完整工具自检 | `evidence/n2/runner-failed-status-green01/` | 57/57 方法通过，exit 0，4.657s |

前一测试草稿的 `runner-b53-failed-status-red/` 也保留；其中另两项失败仅为错误文案
不匹配。最终以实际拒绝行为判定，将旧版已经正确拒绝的输入记为通过，不将它们计入
8 个行为反例。红/绿最终测试文件逐字相同；各目录含精确 argv/cwd、输入、原输出、
UTC、退出码及 hash。绿例于 2026-11-02 01:36:48 UTC 完成。

完整绿例命令：

```sh
TMPDIR=<本批数据盘>/evidence/n2/tmp PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
  /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B tests/test_run_gcc_asan.py
```

| 最终执行输入 | SHA-256 |
| --- | --- |
| runner | `8973416f725020cbd283e58310517820c75f40f59a1db9d99ffb7dc55e4717ac` |
| 工具测试 | `7c351fce2cc19ed249a219274e7fafc81270ad05a4764c5b105c93ff17151103` |
| 未变的 92 项目录 | `f8678cfa81b001191540b1372e0b83705e5c85fd5e148ad706d80250f82f08c6` |

本主题实现及工具自检完成，交非作者按精确提交复核。最终整合产品矩阵尚未启动，
整体 LOCAL_VALIDATION_COMPLETE=false、PUSH_COMPLETE=false、REMOTE_CI_COMPLETE=false。
原本机产品失败仍按其原记录保留，不由本工具修正关闭。
