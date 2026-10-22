# S1.6 发布复验：非 root 权限回归与完整门禁

本地复验日期：2026-10-22 UTC / 2026-10-21 America/Los_Angeles。
实现版本 **0.7.0-s1.6**，蓝图 **v0.4.1**。本报告替代首次本地记录作为本批发布复验依据，
不表示整个 S1 或 S1–S5 完成。首次报告和日志保留，原 303/68 计数属于当时源码。

## 接续来源与先前失败

接手时 main 仍为 `3d0fc858442254d059128a9032f8a60b7f023df6`，开发分支仍为
`458015e9a1cb1e84cc7e40f59f807b0a036958e6`。该分支保存 S1.5 基线及待应用的 S1.6 补丁。
恢复自 Actions run `39535127897`、artifact `12316144422`，不是根据聊天重建代码。
解压后的 Git tree 与远端 `1df3833fdc31acdf3f3aca74a695d84efe6eae76` 完全匹配。

| 原始对象 | SHA-256 |
| --- | --- |
| `source.tar.gz` | `f8b4c670bfdf83f4f0570ad0695836c6e5581a5fb235b94c73ccbdaa57f9c114` |
| 12 片拼接的 S1.6 压缩补丁 | `a7372cea303620a63289f30b91f9fc127a92e1f10e2461c5d264cad1af685099` |
| 解压后的原始补丁 | `a51996dbf99db795c24bc84413dd3c50d505a80e88a47bf983f6e29e937abbba` |

[先前发布运行](https://github.com/wmqfl861/wls861/actions/runs/39535128048) 的 Release
只有 30/32 组通过，UBSan 和发布均未执行。失败发生于三处测试设置：两个 C++ 用例和一个
CLI 用例试图移动或修改已经只读的交付包，普通用户先收到 Permission denied，尚未调用
待验证的清理检查。当前容器改用 uid 1000 后完整复现了这些失败。bootstrap 常规 CI 运行的
旧 S1.5 不能作为已经应用 S1.6 补丁的证据。

## 修复与新增回归

修改限于自建临时测试夹具、发布验证约束及文档；不降低生产交付或清理权限。
丢包场景只给测试包增加 owner-write，移动到本测试的保留路径，然后恢复原模式；
坏包场景只给测试 manifest 增加 owner-write，写入损坏字节后恢复原模式。
两者都先断言故障确实构造成功，再调用运行时，确认原 work 被保留。
坏包必须返回 Conflict，不以一个宽泛权限异常冒充损坏内容校验通过。

新增一个 C++ 和一个实际 CLI 用例：交付包目录为 0500、文件为 0400，普通用户不能直接
追加写入；成功清理后原文件集、原字节和模式完全保持，已发布交付仍可核验。
CLI 权限回归在创建任何夹具之前拒绝 root 测试用户，防止特权执行再次掩盖问题。
原权限拒绝、目录替换、UNKNOWN、故障注入和恢复测试继续执行，未删掉或跳过原失败用例。

## 实际环境与命令

Linux x86_64；GCC 14.2.0；Boost 1.83；SQLite 3.46.1；OpenSSL 3.5.7；LibYAML；
Python 3.13.5 / 预装 jsonschema 4.26.0。测试以 `oai`（uid 1000）运行。
GitHub 独立 runner 将使用仓库固定的 requirements-test.txt；本地不冒称采用该 pin。
构建和日志采集由宿主驱动，实际测试不是 root，也没有 sudo 运行测试。

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DWLS_ENABLE_UBSAN=OFF -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-release --parallel 2
runuser -u oai -- ctest --test-dir build-release --output-on-failure -V
runuser -u oai -- make -j2 test BUILD_DIR=build-make PYTHON=/opt/pyvenv/bin/python
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  runuser -u oai -- make -j2 ubsan BUILD_DIR=build-make PYTHON=/opt/pyvenv/bin/python
```

三套最终门禁以冻结后的同一份源码运行；复验前后活动文件哈希一致。
CMake FindBoost 的 CMP0167 开发警告仍可见，未降低 -Werror、权限检查或 sanitizer 要求。
另外用 root 故意运行新 CLI 入口，实际退出 1 并在创建夹具前拒绝；它不是通过测试套件的替代。

## 最终本地结果与可复核记录

| 层次 | 结果 |
| --- | ---: |
| S1 C++ | **304 / 304** |
| 实际 S1 CLI | **69 / 69** |
| M1 内核及合成 CLI | **55 / 55** |
| Python Schema / SQLite 参考 | **36 / 36** |
| CMake Release / CTest | **32 / 32 组，退出 0** |
| Make 严格门禁 | **退出 0** |
| Make UBSan 门禁 | **退出 0，无 sanitizer 运行时错误** |

其中本批 cleanup 模块 20 项，cleanup_runtime 20 项，s16 CLI 14 项。
相较已发布 S1.5，S1 C++ 新增 41 项、实际 CLI 新增 14 项；同一用例在多种构建模式重复
运行不累加为更多用例。两个 handoff 和固定第三方来源校验均保持通过。

[结果及原始日志指纹](s1.6-followup-results.json) 从实际 verbose 输出提取计数，
[188 个活动文件清单](s1.6-followup-source-SHA256SUMS.txt) 绑定复验源码。
原始日志作为会话附件 `wls861-s16-validation-logs.zip` 一并交付，JSON 列出每个文件的
原字节长度和 SHA-256。日志含真实临时路径，不伪称是无损之外的其他归一化格式。
本报告创建时尚未发布；GitHub 发布必须对完全相同的清洁源码树重新运行 Release 与
UBSan 各 32 组，再由最终 main 提交自己的 CI 复核。实际 SHA 和结果以对应提交的
Actions 记录为准，不预填未来成功结论。

## 边界及后续阶段

本增量真实执行 SQLite、受控临时文件系统、跨进程 CLI、unlink/fsync、SIGKILL 与恢复；
角色决策来自 FakeModel。未调用真实模型、未使用用户生产数据、未发生模型费用。
本轮由同一实现者进行源码复核、反例与故障注入，没有独立人工/第二模型评审。

成功只代表 OWNED 工作目录按已持久化的固定清单删除并记录新鲜不存在证明。
交付包、owner 收据和私有数据库记录继续保留，全局 cleanup_jobs 保持 PENDING，
`cleanup_complete=false`。取消/失败请求、私有行 purge、真实强沙箱退出/残留接管、
完整 daemon/API 和供应商验收仍待实施。S2 独立审核/DAG、S3 记忆、S4 MCP/评测及
S5 按证据扩展维持依赖顺序，不能由本批单项工作外推为全部阶段完成。

见[实现契约](../implementation/S1-owned-cleanup.md)、[路线图](../ROADMAP.md)和
[S1 任务卡](../spec/v0.4.1/07_S1任务卡.md)。
