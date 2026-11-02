# N2 隔离失败的最终回执修正

本次接续 `d9eb6e8983775fb7874a3181910cb6b3b4f7ec62`，处理非作者对
`47451537f1d7cc8d19a953777eb072fe2171ad68` 追加的收尾边界：publication 已完成
正式目录 rename 后发生可捕获中断，随后 quarantine 的 rename 自身再失败时，旧代码
会跳过私有最终异常回执。先前 checkpoint 仍是 NOT_COMPLETE，外层退出 1；这是缺失
终态原因和隔离限制的记录问题，不是旧版误写 PASSED 的再次发生。

最小修正独立捕获隔离异常，继续写入 NOT_COMPLETE、原 publication 错误、
`public_quarantine_error` 和 `failed_ci_required=true`，main 返回 1。已经检查但无法
隔离的候选目录可能残留，必须核验实际进程/CI 失败，不能把候选摘要看作 exit 0。
没有删除或改写原件，没有 raw 上传 fallback，也没有声称任何 SIGKILL 时刻具有跨文件
原子保证。产品、CI 路径、CTest 注册/期限及 sanitizer 条件均未改。

普通 UID/GID 1000，在 `/mnt/d` 数据盘使用既有测试 venv。证据根仍为
`/mnt/d/codex-workspace/wls861-n0-n4-20261102-0k9_2wlf/evidence/n2/`。
新方法先完成真实投影生成和正式目录 rename，再确定性注入 publication 中断及
quarantine PermissionError。夹具为工具合成命令流，不执行产品 C++ 或 sanitizer。

| 执行 | 证据目录 | 实际结果 |
| --- | --- | --- |
| 精确 474 runner，新方法 | `runner-474-quarantine-red/` | 1 方法、1 assertion failure，exit 1，0.230s |
| 修正 runner，完整工具自检 | `runner-quarantine-green01/` | 51/51 方法通过，exit 0，5.166s |

两边测试文件逐字相同。红例实际读取 checkpoint，确认 `public_evidence_error` 为 None
而不是预期原 publication 原因；不是启动、导入或无效夹具失败。绿例同时验证失败状态、
两层原因、failed_ci_required、未隔离候选的原字节、私有 LastTest 和 binaries-after。
绿例完成于 2026-11-02 01:08:24 UTC。每个独立目录保留源码、stdout/stderr、argv/cwd、
环境白名单、起止时间、真实退出码及输入/输出 hash。

```sh
TMPDIR=<证据根>/tmp PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
  /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B tests/test_run_gcc_asan.py \
  GateContracts.test_quarantine_failure_still_records_incomplete_terminal_receipt
```

完整绿例使用相同解释器和环境，去掉方法选择参数。

| 输入 | SHA-256 |
| --- | --- |
| 精确 474 runner | `76117a76f9433da727170e7868383658cd9f48f444ec2b09287e2f4f4bbcd4fa` |
| 修正 runner | `b53f132902d8c989e88f55cdfff4873a5d3001ca1d080e7bf639b13bc84a3360` |
| 两边相同最终测试文件 | `14cb6e506464defabf3bec2618468e245b5147854d6ed1f82f12ba3cee6918de` |
| 未变的 92 项注册目录 | `f8678cfa81b001191540b1372e0b83705e5c85fd5e148ad706d80250f82f08c6` |

本主题修正与工具自检完成；独立复核、最终整合产品矩阵和远端 CI 由主 Agent 继续。
整体 LOCAL_VALIDATION_COMPLETE=false、PUSH_COMPLETE=false、REMOTE_CI_COMPLETE=false。
所有早期报告/红绿原件保持，不将 51 个工具方法计为产品 CTest 或 ASan 套件。
