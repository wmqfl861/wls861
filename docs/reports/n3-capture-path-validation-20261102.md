# N3 Windows image capture 路径夹具修正

时间范围：2026-11-02 01:53:38 UTC 工具自检结束。作者为本批 N3 实现者；本报告和
本地通过结果等待 root、delivery 非作者复核，不构成作者的独立验收。

Push run `41373884133` / job `125049877811` 的 checkout 为
`7bf3a16d9683ed6a8de65d88023bae9687abd20d`；PR run `41373887418` /
job `125049887237` 的 checkout 为 `914a5de2331c6dc79a87d909f62105e43ea6e083`。
两个原始 attempt 1 Windows job 都在 stage 5 工具自检执行 16 方法、1 assertion
failure、exit 1，唯一失败是
`test_capture_entry_only_reads_checkout_and_writes_one_fresh_snapshot`。
stage 6 native build 被跳过。

原 traceback 的 expected `git -C` 路径含 `C:\Users\RUNNER~1\...`，actual 含
`C:\Users\runneradmin\...`。现有入口先对 source 调用 `resolve(strict=True)`；
测试却按 TemporaryDirectory 返回的原始拼写构造完整 argv 期望。
原始日志和失败源身份保留于本批私有 `evidence/remote-final-audit-01/`。
两个 same-job 的 Setup header 为 Windows 2022 / `20261025.346.1`，前置环境原件
为 ImageOS `win22` / ImageVersion `20261026.347.1`；这个真实冲突保持未解决，
路径断言的定位依据是实际 argv traceback 和现有入口源码。

本次在独立 n3 worktree 从 `c262943670eb32a13e4e6d22b9b68f390f71756d`
建立 `fix/n3-capture-canonical-path-20261102-0k9` 分支；原 N3 分支仍指原提交。
该基线测试 SHA-256 为
`b77ac85078616f3ba718a6abe1d498a6b70d5bb7a973d78adc446c3241936cdb`，
production runner 为
`3145783ac53afd928f34c2b876121372bbbb16932918f363b1561c1c7752112e`，
二者与实际远端失败输入相同。

先对上述原测试字节执行受控反例：真实存在的临时目录通过
`existing-component/..` 拼写传入，调用前验证拼写不同且 canonical 目标仍是同一目录。
原测试产生确定的 1 assertion failure、0 ERROR、exit 1，expected/actual 仍只差路径
规范化。这个夹具在 Linux 上复现原始拼写和 canonical path 不同的条件；实际 Windows
8.3 拼写观测来自保留的 Actions 原件。

最小修正仅更新测试：按 `source.resolve(strict=True)` 验证完整 `git -C` argv，
在永久夹具中保留已存在的 `path-spelling/..` 组件并断言规范化确实发生。
`assert_called_once_with`、30 秒 Git timeout、禁止 build/product 的替代进程守卫和
secret 不公开检查全部保留；另外核对目录只新增预期 capture 文件且 source 仍为空。
production、workflow、工具期限和权限条件均未修改。

| 工具执行 | 私有证据目录 | 实际结果 |
| --- | --- | --- |
| 原 b77 测试，受控规范化夹具 | `evidence/n3-capture-canonical-path-01/controlled-red/` | 1 方法，1 assertion failure，0 ERROR，exit 1；0.017s |
| 修正测试，同一反例 harness | `evidence/n3-capture-canonical-path-01/controlled-green/` | 1/1，通过，exit 0；0.089s |
| 修正后的完整工具自检 | `evidence/n3-capture-canonical-path-01/full-tools-green/` | 16/16，通过，exit 0；11.751s |

所有执行均为普通 UID/GID 1000、数据盘 TMPDIR、既有测试 venv。完整工具自检替代
native toolchain/build/install，只通过真实 CTest 执行生成的 Python 夹具；没有编译或
执行产品，也未占用产品编译槽。每次收据包含精确 argv/cwd、UTC、退出码、冻结输入与
stdout/stderr hash；执行前后输入身份一致，两个远端失败日志的 hash 亦保持一致。
固定测试 SHA-256 为
`66e93333d807091ab88f084dc6a97037a70143faa664976866862c64406b1a29`。

IMPLEMENTATION_COMPLETE 仅针对这项测试修正；本地工具回归已完成。最终整合产品矩阵
仍绑定原 `7bf3a16d...`，本修正尚未整合。PUSH_COMPLETE=false、REMOTE_CI_COMPLETE=false、
PR_REVIEW_COMPLETE=false、MERGED=false。新最终 SHA 的真实 Windows workflow/native build、
image 身份比较、Windows 11 普通用户机器和安全验收保持待定。
