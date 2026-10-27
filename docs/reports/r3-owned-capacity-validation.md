# owned 工作区贯通容量验证

日期：2026-10-27。开发基线为 `56185a48303deb1c72b2ec53d896e8c0b32055f0`。
本节点属于 0.9.0-r3.3b 五节点接续；当前整批 Store 为 410，但容量策略自身不新增 SQL，
也不修改任何已发布 SQL。设计见 [ADR-084](../adr/084-owned-workspace-capacity.md)，
接口、配置和退出含义见[实施契约](../implementation/R3-owned-workspace-capacity.md)。
整批输入身份、完整门禁和最终提交 CI 见[五节点验证](five-node-validation-20261027.md)。

## 1. 先验证原容量缺口

从上述 Git 基线提取原 `include/`、`runtime_types.cpp`、`boost_json.cpp`、
`workspace_posix.cpp` 和 `cleanup_posix.cpp`，以当前新增的独立复现夹具
`tests/owned_capacity_baseline_repro.cpp` 编译，未将新容量源码链接到基线程序。
编译与执行均使用 UID/GID 65534，GCC C++20，并保留 `-Wall -Wextra -Wpedantic
-Wconversion -Wshadow -Werror`。

真实执行输出为：

```text
REPRODUCED uid=65534 workspace_default_entries=20000 legal_tree_entries=4097 cleanup_default_entries=4096 inventory_error=quota deletion_performed=false
```

夹具先在独立临时 owned 工作区创建 4096 个合法文件，随后调用原 `Workspace.write_atomic`
成功生成第 4097 个文件。原完整库存因此报告 `quota`；第 4097 个文件和整个工作树仍在，
没有执行清理删除。复现程序以 0 退出表示**确认了该缺口**，不是将原清理拒绝写成修复通过。
基线重建 10.008 秒、执行 0.743 秒。第一次最小源码提取遗漏 `identity.hpp`，编译按预期失败；
补齐同一基线的原头文件后重建通过，没有改动基线实现或放松断言。

## 2. 已执行的新增联合测试

两组程序均由整批严格 Release 构建链接，实际执行身份为 UID/GID 65534，调用 umask 为 0002。
临时夹具的目录和数据继续按原生产规则使用私有权限；调用 umask 不表示每次内部创建都采用该值。

| 程序 | 最终候选聚焦执行 | 具体范围 |
| --- | --- | --- |
| `owned_capacity_tests` | 11/11 通过，4.26 秒 | 真实目录、恰上限/超限、原子 stage 中断、原库存删除和身份围栏 |
| `owned_capacity_runtime_tests` | 13/13 通过，4.22 秒 | 真实 SQLite、原 WorkerTools、FakeModel 工作流、原交付回执、分配与清理重开 |

首轮独立中断用例使用 16 MiB 原文件加 16 MiB stage；随后将同一用例增强为真实 240 MiB 业务树
加 16 MiB stage，同时维持 4095 个业务条目加 1 个 stage。上表与保存的逐项原日志均对应增强后
的源码；库存实际观察到总文件内容 256 MiB 和 4096 项，再按原库存两轮清理完成。
重跑不会增加独立场景数量。上述两组通过不意味着同轮其他节点测试全部通过，也不替代最终全量门禁。

### 真实目录的 11 项

1. 旧策略合法第 4097 项能够写入，原清理库存拒绝且完整保留。
2. 新策略第 4095 项成功；第 4096 项及新目录在效果前拒绝；已有文件替换成功，完整清理包括根删除。
3. 较小精确字节上限允许恰好写满；新增、增长替换和增长 patch 均无效果，等长 patch 可继续。
4. 库存预算使用控制字符文件名的实际 JSON 转义长度；恰好预算内可写，新增项和目录无效果拒绝。
5. 在真实 staged 文件 fsync 成功后对夹具子进程 SIGKILL，原文件保持，完整库存包含遗留 stage；
   达到 4096 项/256 MiB 的实际清理硬边界，进一步业务不能堆积超额 stage，删除按原完整库存分两轮完成。
6. 4061-byte 父路径不能产生超出原路径表示上限的 stage，拒绝前不创建目标。
7. 部分删除后新增条目被拒；移除夹具新增条目后，仍以原库存 hash 完成。
8. 相同内容、不同 inode 不满足原删除身份，原目录和替换文件均保留。
9. 已绑定 Workspace 限制、原 intent 策略与库存策略不能被扩大或移除。
10. 旧库存保持原三字段形状和完整原字节，经部分删除与重放不添加新策略。
11. 配置上界及额外字段拒绝；新策略推导出的最大清理容量仍为 4096 项和 256 MiB。

### SQLite 与真实工具的 13 项

| 覆盖 | 实际断言 |
| --- | --- |
| 原分配到文件效果 | intent、marker、执行策略及实际 Workspace 限制一致；写入/目录/patch 超限结算为明确 FAILED，不制造 UNKNOWN |
| 进程排除 | 新声明不提供 `run`；直接调用在操作准备前拒绝，ops/events 数量及工作树均不变 |
| 新旧请求隔离 | 外部配置更新只影响新请求；原 snapshot、intent 和执行策略不变 |
| 无效配置 | 在新 Store 行和目录效果发生前拒绝 |
| PREPARED 恢复 | v2 创建收据及原 inode、原容量在重开后继续使用 |
| 原库存恢复 | 部分删除后重开，用原库存和原限制完成；外部更小配置不回写旧快照 |
| 历史库存 | 旧 SQLite 库存、intent、snapshot 都不获得新字段或新解释 |
| 历史大树退出 | 返回 `RETAINED_CAPACITY_EXCEEDED`；本次库存与清理 claim 均未提交，零删除，树完整保留 |
| 宿主直接超额写入 | 不能据此扩大新策略删除许可，保持明确容量退出 |
| 库存换策略 | Store 在领取清理权和删除前拒绝 |
| 原 inode 替换 | 重新打开后仍拒绝删除替换项，不重采样覆盖原库存 |
| 删除后 SIGKILL | 已持久化原库存不会被重新生成，重开后依原库存继续 |
| 库存事务故障 | 自建触发器拒绝库存提交，完整树保留；移除已确认的夹具故障后可按原流程完成 |

## 3. 复现命令和证据边界

先完成通常的 CMake Release 构建，再以普通用户执行：

```sh
umask 0002
./build-release/owned_capacity_tests
./build-release/owned_capacity_runtime_tests
```

也可通过原 CTest/Make 测试入口运行这两组。13 项 runtime 夹具明确使用到 Store 408 的原 SQL
构造器，验证容量策略不依赖 live 迁移；整批 CLI 和 live 专项另行覆盖 Store 410。
这里只记录本节点的具体命令和已观察结果，不预填全量 ASan、UBSan、Clang 或远端 CI 成功。

已保存的机器记录和原日志：

- [基线重建与红测原输出](r3-owned-capacity-baseline.txt)。
- [最终候选 Release 逐项原结果](r3-owned-capacity-release-detail.txt)：从同一 CTest 原日志提取两组完整段落，包含 11 项和 13 项结果。
- [机器结果与输入/日志 SHA-256](r3-owned-capacity-local-results.json)：12 个容量组件/测试输入与普通用户运行副本逐字匹配；整批完整依赖身份另见五节点报告。

未放宽清理条目、内容、库存字节、单文件、目录深度或删除身份硬限制。业务限额内预留一次
原子 stage 的空间；旧超额工作树通过结构化保留退出。原所有权、持久库存、当前清理 claim、
费用/操作结算、原交付、精确回执和经验结算依然是独立门禁。

测试均为临时目录、临时 SQLite 和 FakeModel，真实供应商与付费调用为 0。SIGKILL 证明的是
进程中断恢复，不是整机掉电。新策略是 file-tools-only，不提供任意子进程磁盘配额或同 UID
恶意宿主隔离；大树分片、人工处置入口和 private-row purge 仍不在本节点范围。

`workspace_absence_verified_now` 只描述 owned `work` 根的删除观察。owner 收据、交付包及全部
私有 Store 行继续保留，global cleanup 仍为 PENDING，不能把工作目录阶段完成写成全部清理完成。
