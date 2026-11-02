# N5-0 私有原件本地封装工具

本批提供 `scripts/private_evidence.py` 的离线 `capture / pack / verify / preflight`。
它保存实际命令退出及现有共享 GCC runner 的原件，不另建产品 gate，不修改其编译器、
CTest 目录、测试期限、sanitizer 或公开投影。真实 hosted 私有持久化仍为 **BLOCKED**。
这份实现说明不采纳新的存储或加密政策。

## 目的地前提

本次只读核查时，`wmqfl861/wls861` 的 GitHub `visibility=public`、`private=false`，
仓库级 Actions secrets 与 variables 的实际数量均为 0。当前授权账号具备仓库管理及写入
权限，但这些权限不自动产生私有 artifact、授权审阅者名单或可验证的保留策略。
现有 CI 只上传 `build-gcc-asan-*/public-evidence/`，保留期为 7 天；这仍然是公开投影。

主协调者须先采纳具体受限目的地及数据生命周期，再配置和验证：

1. 外部私有存储或独立私有仓库的精确目的地与所有者。
2. 授权审阅者读取 ACL，以及独立可核验的访问拒绝证据。
3. 明确短期保留期限、自动到期策略和删除核验方式；本机文件时间不是保留策略。
4. 最小写入凭据或工作负载身份，及不向 fork/untrusted job 暴露凭据的运行边界。
5. 实际上传、原件逐字/hash 读回和到期处置结果。

缺少任一前提不能声称 hosted 私有持久化完成。此工具不申请或读取生产密钥，不创建
默认接收者，不配置 ACL，不上传，不实现加密，不自动删除任何旧证据。若未来考虑在
公开 artifact 中保存加密文件，必须先作独立政策决定：那是公开可下载的密文，**不是
GitHub 私有 artifact**，不能仅靠改名来满足限权要求。

## 实际命令退出与原件关联

`capture` 是通用命令包装器：`Popen` 后实际 `wait`，分别保留 stdout、stderr 和
`command.json`。回执记录原 argv/cwd、时间、PID、实际 child return code、是否 reap、
收到的 SIGINT/SIGTERM、是否升级 SIGKILL，以及两份原日志的字节数/hash。
启动失败保持 `exit_code=null`；不会把已有文件或拟发布摘要变成成功退出证明。
包装器返回 0 仅表示被观察命令未中断且实际 exit 0；其他执行终态返回 1。

捕获 SIGINT/SIGTERM 后转发 SIGTERM 给自身启动的进程组，让共享 runner 按原规则收尾。
30 秒后仍未退出才升级 SIGKILL；这段时间是取消后的收尾上限，不改任何产品测试期限。
回执只证明直接 child 已 reap，不声称能证明任意脱离进程组的后代退出；共享 runner
仍负责它自己的 CTest 子进程组。SIGKILL、主机掉电或无法写入回执不保证完成收尾。

`pack` 显式接收本次 `private-evidence/` 和 capture 目录。它校验外层 cwd 与原摘要
source、外层开始/结束时间对摘要时间的包含关系，检查外层日志、原 CTest index、
step receipt 和 index 中每个 artifact 的原字节/hash。它不接受只填写一个退出码来
替代实际 capture，也不从公开投影重建私有 LastTest/JUnit。回执来自可信本地主机；
这些 JSON/hash 不是防范该主机管理员伪造记录的远程认证协议。

必需原件包括原 `LastTest.log`、JUnit、CTest stdout/stderr/exit、源码前后身份、
二进制前后身份、原注册清单和共享 runner 摘要/index。存在的 `.tmp`、失败索引、
其他命令原日志、发布输入与配置原文均原字节复制，不因缺件或测试失败而删改。
缺原件、原始关联不一致、可捕获中断或不完整 JUnit 保持 `collection_status=INCOMPLETE`；
可用原件继续封装，不把 `.tmp` 改称完整日志。完整采集的失败或 CTest timeout 可以是
`collection_status=COMPLETE`，同时原 `runner_status=NOT_COMPLETE`、真实非零退出和
失败/超时观察保留。采集完成不代表产品 gate 通过。

## 本地权限和完整性

所有新目录必须在已存在、当前 UID 拥有的 0700 父目录下排他创建；输出文件为 0600。
不自动 chmod 一个已有共享目录。私有输入根及子目录必须为当前 UID 的 0700 目录；
输入原日志可以保留原 0644/0664 文件模式，因为其路径被 0700 父目录隔离，复制件统一为
0600。工具拒绝 symlink、hardlink、FIFO、其他特殊文件和非当前 UID 文件。

复制时用受控目录 fd 和 `openat` 固定目录/文件，不跟随链接，核对读取前后对象身份、
目录成员和字节/hash，再独立检查源清单未变化。目录中断后保留已复制字节；没有
`manifest.json` 的目录视作未完成。已有目标永不覆盖，不能自动重试到原目录里。
`verify` 检查精确文件集、hash、大小、模式、原件关联和状态；额外文件同样拒绝。
父目录及同 UID 代码属于可信本地主机范围，不宣称抵御恶意管理员或任意同 UID 并发
篡改。SHA 清单检查损坏/关联，不构成签名、加密、受控远端 ACL 或介质安全擦除。

`pack / verify` 返回 0 表示本地封装及证据关联完整，返回 2 表示不完整或拒绝。
无论该返回码如何，`hosted_persistence` 始终是 `BLOCKED`，`storage_scope` 始终是
`LOCAL_PRIVATE_COPY_ONLY`。`preflight` 可检查本地目录权限，但它只列出尚未核验的远端
前提并返回 2。它不会把本地 0700、用户填写的配置或拥有仓库 admin 权限当成远端验收。

## 共享 runner 的最小接入方案

当前没有 CI 接入。获准的本地调用可以只包住原命令，精确路径由调用者传入：

```sh
python3 scripts/private_evidence.py capture \
  --directory /mnt/d/task-private/command-01 --cwd /mnt/d/clean-checkout -- \
  /mnt/d/test-venv/bin/python -B /mnt/d/clean-checkout/scripts/run_gcc_asan.py \
  --source /mnt/d/clean-checkout --data-root /mnt/d \
  --compiler g++-13 --python /mnt/d/test-venv/bin/python --expected-tests 92

python3 scripts/private_evidence.py pack \
  --evidence /mnt/d/clean-checkout/build-gcc-asan-EXACT-RUN/private-evidence \
  --capture /mnt/d/task-private/command-01 --directory /mnt/d/task-private/bundle-01

python3 scripts/private_evidence.py verify --directory /mnt/d/task-private/bundle-01
```

`/mnt/d/task-private` 事先为 0700；测试 TMPDIR 同样位于数据盘私有目录。命令示例是
接口说明，不是已执行产品结果。调用者分别保存 wrapper、pack 和 verify 的实际退出，
即使 wrapper 非零也尝试封装本次已生成的原件。准确选定本次 build，不能取 glob 的第一个
结果或最近一次绿色；`pack` 的 cwd/时间关联进一步拒绝混入另一轮回执。

后续 CI 若采纳，应只将原共享 runner 那一行放入 capture，并在 `always()` 收尾步骤
定位本次唯一 build，调用 pack，再交给已获准的受限存储适配器。公开上传步骤仍只接受
现有严格投影目录；**任何情况下都不得上传本工具目录或私有 bundle 为普通公开 artifact**。
该集成需要真实存储政策/凭据和远端读回，不能由本地自检替代。

## 工具验证边界

`tests/test_private_evidence.py` 使用临时 git 输入、真实轻量 CTest、合成 Python 子进程
与合成凭据文本。success/failure/timeout/SIGTERM 检查实际 wrapper 与 child 退出，保存
源码/Python executable 前后身份、LastTest/JUnit 和原日志，检查删除临时 job 目录后独立
本地 bundle 仍可校验。它不声称销毁整个 hosted VM 后仍能读取本机副本。

旧能力缺口使用同一夹具的 projection-only 生命周期探针：实际 CTest exit 0、原完整
日志存在且公开投影移除合成私有文本后，模拟 ephemeral job 目录销毁。仅保留旧公开
投影时，私有原件可获取性断言失败；先本地 pack 时该断言通过。红例是既有保存策略的
能力缺口，不是 C++ 编译错误、新 API 缺失或一项新产品缺陷。实际命令、原日志、退出码
和运行输入只在验证报告记录；本说明不预填通过结果。

这些工具方法与原 92 项产品 CTest 分开计数，不关闭历史 sanitizer 失败，不代表受限
远端存储已可用或主协调者已采纳存储政策。
