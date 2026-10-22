# S1.6 受控工作目录清理：实际验证报告

> 本页保留首次本地验证记录，不代表当时开发分支发布成功。随后GitHub非root运行暴露
> 只读交付包的三处故障注入失败。当前发布依据、复现和修复后的完整结果见
> [S1.6发布复验](s1-owned-cleanup-followup-validation.md)；下文303/68为首次本地用例数。

日期：2026-10-21 UTC（用户时区为2026-10-20）。实现 **0.7.0-s1.6**，目标蓝图 **v0.4.1**。
本报告的结果只适用于所附源码清单及下述测试，不表示完整 S1 或 S1–S5 已验收。

## 1. 可验证的接续基线

远端 main 在接手时为 `3d0fc858442254d059128a9032f8a60b7f023df6`（S1.5）。
上一会话未发布的 S1.6 不作为源码或测试证据。新建 `work/s1.6-owned-cleanup` 分支，
先增加只归档 `git archive HEAD` 的源码恢复工作流，提交
`860e27388889e868f85d79dbdda5f9a3ed486e2d`，tree
`0eab68ee68d6559dfb98e2fb8c7995f4b70a0f62`。

恢复源为 Actions run `39528769059` 的 artifact `12314785966`，归档内
`source.tar.gz` SHA-256 为 `2c03f5ee4707f7cebeeaf04b1dedf565a68fb38e98d5ae1e4a821c524cb48a81`。
在新开发容器展开后，325个文件的 Git tree 与上述远端 tree 完全一致；未采用未知旧工作区。
该恢复工作流不打包运行目录、数据库、构建物或凭据，归档内容已经是已提交源码。

修改前原始基线 `make -j2 test` 退出0。此后追加本批代码/测试/契约，原 handoff、
vendor、schema/004–008迁移、历史报告不变。本批活动输入指纹见
[SHA256SUMS](s1.6-source-SHA256SUMS.txt)；每个历史报告的原清单只证明当时版本。

## 2. 环境与命令

本地 Linux x86_64，kernel 6.18.55，GCC 14.2.0，Boost 1.83.0，SQLite 3.46.1，
OpenSSL 3.5.7，LibYAML运行库；Python 3.13.5，预装 jsonschema 4.26.0。
GitHub标准CI仍按仓库 requirements-test.txt 在独立venv安装固定版本，本地没有冒称使用相同pin。
测试不调用模型提供商、不产生模型费用、不操作用户生产目录、进程或服务。

```sh
make -j2 test
make -j2 ubsan
cmake -S . -B build-cmake-s16 -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/opt/pyvenv/bin/python3
cmake --build build-cmake-s16 --parallel 2
ctest --test-dir build-cmake-s16 --output-on-failure
```

源码冻结后重跑三套门禁，各自独立构建目录。UBSan设置
`halt_on_error=1:print_stacktrace=1`，严格编译保持 `-Werror`。
CMake沿用FindBoost的CMP0167开发警告，未以关闭编译警告或降低测试条件换取通过。

## 3. 结果与分层计数

| 类别 | 结果 | 说明 |
| --- | ---: | --- |
| S1 C++ | 303/303 | 原263加资源物理20、运行/Store集成19、冻结配置1 |
| 实际S1 CLI | 68/68 | 原55加新13；Python调用真实C++程序、临时SQLite与文件 |
| 原M1内核与CLI | 55/55 | 合成内核保持独立，不冒充强沙箱或模型质量 |
| Python Schema/SQLite参考 | 36/36 | 参考契约，与C++实现分列 |
| 严格Make完整门禁 | 退出0 | 原件及固定依赖校验也通过 |
| Make UBSan完整门禁 | 退出0 | 无UBSan运行时错误 |
| 独立CMake/CTest | 32/32组 | 原29加cleanup、cleanup_runtime、s16_cli_smoke |

同一用例在不同构建模式重复执行不计为更多产品验收。所附日志仅将开发容器的绝对checkout
路径替换为 `<CHECKOUT>`，没有修改测试输出/结果；不是声称原始日志字节完全未变：
[Make](s1.6-make-test.txt)、[UBSan](s1.6-ubsan-test.txt)、[CMake/CTest](s1.6-ctest.txt)。
远端最终提交与CI以GitHub对应commit/workflow记录为准，不在提交前预填未来CI成功状态。

## 4. 新增验证的实际效果

**物理目录层（20项）。** 真实创建、字节读取、unlink/fsync、空目录、精确重复和有界恢复；
旧目录不采用、清单新增/改字节/同字节换inode拒绝、符号链接/硬链接/FIFO拒绝；
所有权收据/父目录/envelope/work替换拒绝；字节/深度/条目/单次步数上限，合作锁冲突和只核验不删除。
所有测试目录由测试创建并自行回收，不以字符串前缀扫描生产路径。

**Store与运行器（19项）。** FakeModel驱动实际请求、验收、经验、交付/回执后，真实删除已登记
work并保留原包/私有证据；默认关闭、未登记旧目录、丢失/损坏包、陈旧claim和原清单变化拒绝。
分批执行重开恢复、SQLite触发器注入删除后记录失败恢复、真实fork子进程在删除后被SIGKILL，
父进程重开同库只补记原清单；非Root拒绝、幂等绑定、资产指纹、非空406升级407、跨请求嵌套拒绝。

进程退出门禁的正/负例使用明确标记的**合成原始进程记录**，没有启动伪装成强沙箱的进程。
先写UNKNOWN原观察，再仅核对业务结果FAILED，不能让缺少后代退出证明的记录通过清理。
有完整原退出事实时核其哈希；这是门禁回归，不是bwrap/cgroup强环境正向验收。
原有进程监督用例继续运行，但同样不升级其原证据边界。

**真实CLI（13项）。** 跨进程创建、查询、固定脚本执行/验收、发布/精确ACK、owned清理与再次核验；
默认拒绝不接管写者，错误确认/只读/跨请求task不改库；无回执、丢包、坏包拒绝；分批重开不扩清单；
删除后记录失败恢复、收据/链接拒绝、同键准确重复和取消工作留存。复用S1.5帮助方法，但测试套件
只运行本类的13项，不把继承的旧17项重复计数。

## 5. 开发中发现并修正

- FakeModel显式pump的角色调用序号从该次运行起算：计划后的测试续跑脚本必须扣除已经消费的
  Dispatcher步骤；没有以改运行器语义来迁就夹具。
- SQLite故障注入的RAISE(ABORT)按既有分类返回Conflict，坏包的权限变化可能先返回Unauthorized；
  修正测试断言，不放松实际删除门禁。
- 非空406迁移测试需比较 `request_snapshot` 中的原始text，不能从requests行读取不存在的字段。
- 中断期 `deletion_performed` 改为null，另给 `deletion_may_have_started`，不虚报尚未发生删除。
- 在打开数据fd前用O_PATH核对象类型；读取后再次核总字节上限；资源边界重叠同时在分配、
  绑定和交付入口检查。原closeout快照在407明确写为待独立授权，旧历史快照不重写。
- Python 3.13暴露旧CLI夹具未关闭SQLite连接的ResourceWarning；添加显式closing并保留原事务上下文，
  完整重跑后验证无此警告，没有过滤或屏蔽测试警告。
- 首次独立CMake误指定不存在的 `/usr/local/bin/python3`，Configure退出1；改为实际
  `/opt/pyvenv/bin/python3` 后重新配置、构建和完整CTest通过。

这些是本次实际发生的修正/重跑，不将首轮失败日志算为最终通过。本轮由同一实现者进行
代码复核、反例和故障注入测试，没有调用第二个模型或独立人工审查者；不宣称多Agent独立审核。

## 6. 未完成与不能外推的结论

`workspace_cleanup_runs=DONE`只记录本阶段，**全局cleanup_jobs仍PENDING**。
私有数据库行、原提交、调用、经验、owner收据和交付包保留；没有私有purge、VACUUM、
TTL销毁、Git历史/备份/介质擦除。查询检查点不等于当前文件系统重新观察，实际完成接口
单独给 `workspace_absence_verified_now`。

没有启动/接管部署cgroup、杀任意PID、验证完整强沙箱正向、真实供应商或daemon/API；
取消/失败目录必须等待失败成果包保留政策。合作锁和目录身份不保证抵御任意同UID恶意宿主。
也没有证明整机掉电或多个资源共同原子删除。S2独立审核/DAG、S3记忆、S4 MCP/评测、S5扩展仍待开发。
完整行为、限制与下一依赖见[S1.6契约](../implementation/S1-owned-cleanup.md)和[路线图](../ROADMAP.md)。
