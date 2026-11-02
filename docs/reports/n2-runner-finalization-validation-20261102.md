# N2 runner 收尾独立复核修正

本记录追加于 `97cf68a1ce92823be303a2246ec95da85e5d0245` 的非作者复核之后。
旧报告和原始红例全部保留；本次只修改 runner、工具自检和当前契约，不修改产品代码、
CTest 注册/期限、编译选项、sanitizer 开关或公开上传范围。

## 已证实的问题与修正

旧 runner 在公开投影前将私有 `summary.json` 写成 PASSED，且 publication 只捕获
`Exception`。可捕获的 `KeyboardInterrupt` 会留下错误通过回执；如果中断发生于实际
rename 之后，也会留下正式 `public-evidence/` 目录。新版本直到 publish 返回前都让
最终摘要保持 NOT_COMPLETE，并在可收尾的异常/中断后把已生成目录移到
`public-evidence-incomplete-<随机值>/`；逐字保留内容但不匹配 CI 的正式 artifact 路径。

公开摘要来源改为排他创建、不再改写的私有 `summary-publication-input.json`。它保存
拟发布状态，公开索引记录该真实原件的字节/hash；它不是最终进程退出回执。只有发布
成功返回后才更新最终摘要。不可捕获的 SIGKILL 不能保证任意时刻均能收尾；必须核对
实际退出码及外部 CI 终态，候选输入或单个 artifact 不能证明 exit 0。没有新建私有外部
存储，也没有上传 raw；hosted runner 销毁后私有原件仍为 NOT_PRESERVED_DURABLY。

旧 runner 的源码快照、HEAD、工作区和二进制末尾核验位于同一个 try 中。前三类出错
会跳过 `binaries-after.json`。新版本独立执行四项检查、聚合错误，任一变化/缺失均不能
通过，同时尽可能保留所有末尾身份。原始 LastTest、JUnit 和错误收据保持。

## 红例与绿例

环境为 `/mnt/d` 数据盘，UID/GID 1000，使用既有测试 venv：
`/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python`。
证据根为
`/mnt/d/codex-workspace/wls861-n0-n4-20261102-0k9_2wlf/evidence/n2/`。
各目录均独立创建，包含原 runner/测试输入、stdout/stderr 和真实 argv/cwd/环境白名单/
起止时间/退出码/hash 回执。本次均为 Python 工具自检，主流程注入明确使用合成命令
夹具；不是产品 C++/ASan 反例。

首次收尾反例 `runner-97-closeout-red/` 为 5 方法、5 assertion failures、exit 1；
追加发布反例 `runner-97-publication-red/` 为 2 方法、2 assertion failures、exit 1。
最终使用完全相同测试文件在精确 97 runner 和修正 runner 上执行：

| 执行 | 证据目录 | 真实结果 |
| --- | --- | --- |
| 97 原 runner，7 个收尾方法 | `runner-97-closeout-final-red/` | 7 failures，exit 1，1.151s |
| 修正 runner，完整自检 | `runner-closeout-green02/` | 45/45 方法通过，exit 0，3.549s |

绿例前一轮 `runner-closeout-green01/` 为 45/45、exit 0；随后补充了拟发布输入的
来源/hash 和成功前后字节一致断言，才执行最终 green02。它们是同一工具测试文件的
不同轮次，不能计为多个产品测试套件。

精确红例命令（在冻结 source 目录中）：

```sh
TMPDIR=<证据根>/tmp PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
  /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B tests/test_run_gcc_asan.py \
  GateContracts.test_publication_interrupt_cannot_leave_a_passed_private_summary \
  GateContracts.test_source_change_still_captures_binary_after \
  GateContracts.test_head_change_still_captures_binary_after \
  GateContracts.test_dirty_checkout_still_captures_binary_after \
  GateContracts.test_source_snapshot_error_still_captures_binary_after \
  GateContracts.test_publication_interrupt_after_rename_quarantines_public_directory \
  GateContracts.test_private_summary_stays_incomplete_until_publication_returns
```

完整绿例使用相同解释器、环境和文件，仅去掉方法选择参数。运行结束于
2026-11-02 01:01:24 UTC；真实退出状态由外层记录器保存。

| 输入 | SHA-256 |
| --- | --- |
| 97 原 runner | `c639df7bb5d1f0ad07795e64558976d15fd117ddab123365040c332009c990d2` |
| 修正 runner | `76117a76f9433da727170e7868383658cd9f48f444ec2b09287e2f4f4bbcd4fa` |
| 两边相同最终测试文件 | `8977719e5c42be811d0da15ed87d5eb560ddf0c869a69a34c543f93aaa9e58a6` |
| 未变的 92 项注册目录 | `f8678cfa81b001191540b1372e0b83705e5c85fd5e148ad706d80250f82f08c6` |

本主题工具修正与本地自检完成。复核发现的 JSON/XML 公开 guard 解析缺口由后继独立
提交修正，不能因本次 45/45 而视为已关闭。主 Agent 当前独占产品诊断资源，本次未运行
任何 C++ 构建或产品 sanitizer。整体 LOCAL_VALIDATION_COMPLETE=false，
PUSH_COMPLETE=false，REMOTE_CI_COMPLETE=false；不宣称合并或产品验收完成。
