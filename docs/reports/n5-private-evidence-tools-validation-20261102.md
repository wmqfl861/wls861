# N5-0 私有证据本地工具验证

本记录仅交付离线 command capture、私有 bundle、完整性核验及阻塞前提检查。
**hosted 私有原件持久化尚未完成，N5-0 的该项仍为 BLOCKED。** 没有新建存储、
上传、加密、CI 接入或生产 gate；原公开 artifact 继续只包含严格投影。

## 输入、权限和实际改动

基线 `aa2c592b8aed6509189710cc6f6d69e0959098a8`；本地分支
`fix/n5-0-private-evidence-20261102-csj777e9`。作者为本地执行子代理
`/root/concurrency_fix`。最终本地 commit 由正常 Git 提交产生，完整 SHA 与提交树
核验写入外部回执；本报告不预填自身提交、推送或远端 CI。

已完整阅读提交 `283d23070a1f36f2eea598bcb96b2a312cd71307` 对应两份冻结交接，
其 SHA-256 分别为 `14b3d003633951684d2c33c5fac8dab5f7c5e7d24e65136b278a45745583fec1`
（NEXT_AGENT_PROMPT）与 `c1274c17acd627679df924d2c211f2a37ab66a2bf278422deedd77415fb010b7`
（REVIEW_AND_HANDOFF），与输入清单相同。当前 AGENTS、共享 runner、自检和相关证据
契约按实际工作树读取。结构查询遵循 codebase-memory 技能；子代理没有可调用图工具，
使用父代理给出的旧图缺口信息及当前源码读取，没有用旧图证明 aa2 接口不存在。

| 修改 | 实际行为 |
| --- | --- |
| `scripts/private_evidence.py` | 实际 child wait/信号/退出/原 stdout/stderr；排他本地封装；原件关联、文件集/hash/mode 检查；缺远端配置始终 BLOCKED |
| `tests/test_private_evidence.py` | 16 个工具方法，真实轻量 CTest/Python 进程，独立临时目录和合成私有文本 |
| `docs/implementation/N5-private-evidence-tools.md` | 权限、退出、完整/不完整、同 UID 信任、共享 runner 最小包装方案和未采纳的存储前提 |
| 本报告及配套 inputs JSON | 实际命令、输入、原日志索引、退出及限制 |

`run_gcc_asan.py`、原 57 方法自检、CI、CMake、Store、公共头、产品测试期限、ASan/LSan
设置均未修改。源码/build/SQLite 迁移和历史报告不在本补丁内。

普通 UID/GID 1000，数据盘 `/mnt/d` 是 `/dev/vdb2` ext4，接手约 34 GiB 可用；任务根
0700。Python 3.12.3、CTest 3.28.3、Git 2.43.0。所有临时目录和原工具日志位于本任务
数据盘私有目录。只读 GitHub 预检的 6 条命令实际 exit 0：仓库 public，当前账号有
admin/push 权限，仓库级 Actions secrets=0、variables=0；这些结果不证明已有远端私有
目的地。没有读取或打印 token、私钥或环境变量值。

## 已执行反例与绿侧

本地执行根代理先安排 sanitizer 因果实验，因此新工具的测试/编译最初暂停；收到其明确
开放轻量自检窗口后才执行下述命令。代码已准备，**没有把暂停期间的测试标为已运行**。
本次反例是既有 projection-only 保存策略的能力缺口，不声称修复了一项 C++ 运行时缺陷。

```sh
TMPDIR=<任务证据>/private-tools-red-01/tmp \
PYTHONDONTWRITEBYTECODE=1 WLS_EVIDENCE_LEGACY_RETENTION=1 \
/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B \
tests/test_private_evidence.py \
EvidenceContracts.test_successful_job_originals_survive_ephemeral_job_deletion

TMPDIR=<任务证据>/private-tools-green-01/tmp PYTHONDONTWRITEBYTECODE=1 \
/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B \
tests/test_private_evidence.py
```

上述 cwd 为本分支 `evidence-tools` 工作树。实际完整 argv/cwd 与前后输入见私有回执；
红例环境中的开关只用于测试夹具，不是生产工具的可用降级选项。

| 执行 | 真实结果 | 证明范围 |
| --- | --- | --- |
| 11:47:07–11:47:11 UTC，旧 projection-only 生命周期 | **1 方法 / 1 failure，exit 1**；unittest 4.180s，外层 4.877769s | 原 CTest 已 exit 0，LastTest 超过 4096 字节，公开投影已移除合成私有文本；临时 job 删除后私有原件不可获取，指定断言失败 |
| 11:47:58–11:48:19 UTC，完整工具自检 | **16/16，exit 0**；unittest 20.294s，外层 21.183598s | 同一成功生命周期先本地 pack 后原字节可取得；失败、真实 CTest timeout、SIGTERM、缺件及安全拒绝一并通过 |

红例使用与 aa2 Git 对象逐字相同的现有共享 runner 和公开投影器。新的 capture 仅作
真实退出观察，红例未调用 pack；它不是一次 hosted CI 重跑，也不在不存在的新 API 上
导入/编译失败。红绿使用相同的业务夹具和断言，差别是是否先保存独立本地私有 bundle。
销毁的是测试自己的 ephemeral job 目录；这不证明 hosted VM 被销毁后本机副本仍存在。

四种执行终态的验证使用真实 CTest 和 Python 进程，无产品编译、真实供应商、网络或
付费请求。Python executable hash 是夹具执行身份，不冒充 WLS 二进制或 sanitizer 结果。
SIGTERM 验证实际 wrapper exit 1、child exit 1、reaped=true、forced_kill=false，原 `.tmp`
按原名保留。采集失败/timeout 原件可以 COMPLETE，但原 runner 为 NOT_COMPLETE、实际
非零退出仍保留；缺件/中断保持 INCOMPLETE。方法/子例不计入原 92 项产品 CTest。

另覆盖错轮回执关联、原 index 篡改、symlink/hardlink/FIFO、共享目标目录、目标覆盖、
复制后实际输入变更、包内容/额外文件/模式变更、JSON 重复键/非有限数、公开 stdout
无合成私有值。新包保留全部原件，manifest 检查不能代替主机认证或远端权限。

## 冻结输入与原日志

两次执行各自保存独立 stdout/stderr、实际进程退出、起止时间及下列 5 项输入的前后
SHA-256。两次执行中这些输入均未变化；配套 inputs JSON 保留完整列表。

| 输入 | SHA-256 |
| --- | --- |
| `scripts/private_evidence.py` | `e2e82ebfda89a2519ea6124fc2defa4e234eea8b72f72946efb5618f5be12e1a` |
| `tests/test_private_evidence.py` | `83b30e0992679845eeea76aac587cd521ad10cadb429ca8e31b1890359f872a5` |
| 原共享 runner | `8973416f725020cbd283e58310517820c75f40f59a1db9d99ffb7dc55e4717ac` |
| 原共享 runner 自检 | `7c351fce2cc19ed249a219274e7fafc81270ad05a4764c5b105c93ff17151103` |
| 新实现契约 | `39f55cb231a7d613d4182fdc442af6fea5ee5d34fdd6672e175efe1db2c7e7dc` |

原证据根：`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/evidence/`。

| 相对路径 | SHA-256 |
| --- | --- |
| `private-tools-red-01/receipt.json` | `cb0f1891721ee8b45350987d24a5c533e86a24f3aadbe127b64263ac43efe8f6` |
| `private-tools-red-01/stderr.log` | `1cd34d4bda6a25980b4cb0aef89187d3889710b6ec24d7744eb0795d0df8d997` |
| `private-tools-green-01/receipt.json` | `386b2b2f43486835a84b682661c12e15f2daeefc47c781140bd1ce7629276449` |
| `private-tools-green-01/stderr.log` | `3b9cf9dc3f2fe041a7adfe6aaae2f85cc17e5043f81a66532b1f344479c6d64d` |
| `private-tools-preflight-01/receipt.json` | `4cf9727f62b85fc5e9c72b40cfc7a3faed8f8dcc936704bc87e70c53196f06e8` |

两份 stdout 均为空，SHA-256 为 `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。
原日志没有进入公开仓库；报告仅列身份与限定结果。完成后进程检查没有留下本工具、
夹具 Python 或 CTest 子进程；没有杀死或修改其他 Agent 的进程。

## 非作者复核与剩余状态

本地执行根代理 `/root` 已读完整脚本并反馈未见阻断项，要求保留 child wait 不等于所有
派生任务退出、同 UID 可伪造历史，以及采集完整不等于 gate 通过的边界；契约已明确。
这是非作者工程阅读，最终回执复核及主协调者的存储政策采纳另记，不代签正式 PR 通过。

本次新增离线工具及适用 16 方法本地验证完成；真实 hosted 私有持久化仍缺目的地、
授权审阅者 ACL、保留/删除策略、最小写身份和远端读回。`preflight` 如实返回 BLOCKED，
没有提供手工填配置即可 PASS 的入口。没有自动 retention、加密或上传实现。

| 状态 | 本报告范围 |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true，仅获准的本地离线 capture/pack/verify/preflight 工具；hosted 持久化 false |
| LOCAL_VALIDATION_COMPLETE | true，仅上述 16 方法工具自检；N5 产品完整门禁与 hosted 证据验收 false |
| PUSH_COMPLETE | false，子代理不推送，由本地执行根代理统一发布 |
| REMOTE_CI_COMPLETE | false，本补丁未运行远端 CI |
| PR_REVIEW_COMPLETE | false，等待主协调者正式复核 |
| MERGED | false |

本补丁无 C++/构建改动，完整 Release、两种 umask 产品矩阵、ASan/LSan、UBSan、Clang、
Make-ASan 没有因本工具重新执行；它们属于根代理的独立最终输入验证。原
`historical_clang_asan_failures_closed=false` 和既有失败不变。后续存储政策须由主协调者
作出明确决定；本子任务不凭仓库写权限扩大为真实模型、付费或生产操作。
