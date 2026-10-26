# R2a.1：有界目录工具实现与验证

实现版本：**0.8.3-r2a.1**；目标蓝图：v0.4.1；设计见
[ADR-074](../adr/074-bounded-directory-tool.md)，执行契约见
[R2a.1 目录工具](../implementation/R2a-directory-tool.md)。

本增量在 R2.3 模型 AMEND 之上补足 Worker 新建输出子目录的能力。
PREPARED 所有权分配恢复没有混入本次实现；其
[ADR-075](../adr/075-owned-allocation-recovery.md) 明确为设计草案。

## 代码和输入绑定

- 本地被测提交：`930665abeed3d437ea5188d5be74fce5c03e43ad`。
- 对应远端代码提交：[ac7766d613efdff124820a1a9ffcc1f9895dc204](https://github.com/wmqfl861/wls861/commit/ac7766d613efdff124820a1a9ffcc1f9895dc204)。
- 两者完整 Git tree：`e1a61afeba3d0096b56f8db4e71cc2eee38c668e`。
- 233 个执行输入与该提交的 `git archive` 逐字节一致；输入清单 SHA-256：
  `f6ebda73b85913cddcba9e735c927ee2d8aeb80b502c7b9ad21c69169ecf39e0`。
- 最后两处变化仅修复测试夹具；生产代码没有因测试失败放宽。后续发布文档不得改变这些执行输入。

远端提交保留已发布 R2.3 父链；GitHub 提交元数据不同，因此以完整树和执行输入核对，
不把本地 SHA 当作已推送 SHA。设计、实现和夹具修正是独立提交。

## 已实现行为

Worker 的 `fs_mkdir` 仅接受必填相对路径 `path`。父目录必须存在；只创建一个原先不存在的
空目录，固定 0700。没有递归建父目录、exist_ok 或模型自选权限。原 fs_write 仍要求父存在。

创建前核验根/父目录、目标、整棵工作树、投影条目数和递归深度。通过固定父 fd 执行一次
mkdirat，固定子目录 fd、核权限，分别同步子/父目录并最终复核根、父路径、子目录身份和空目录。
不修改进程全局 umask。mkdir 尝试前的失败为 EffectNotStarted；尝试后任一失败保守为
OutcomeUnknown，保留原始观察，不在 catch 中盲目删除或再次创建。

业务工具复用原持久首次派发、配额、精确重放、完整版本、AMEND/UNKNOWN 和结算失败围栏。
新 general 模板显式给出 grant；旧请求不补写授权，共享策略版本不变。新配置仍产生其自身
冻结配置/执行策略哈希，不要求新旧请求策略哈希相同。诊断入口不能创建目录。

## 验证范围与结果

正式本地测试均为普通用户 UID/GID 65534，临时目录真实文件效果、真实 SQLite 和固定 FakeModel。
Release 使用双 umask 全量回归；本地 sanitizer 只执行与此变化直接相关的五组，
完整 sanitizer、Make-ASan 和 Clang-18 由同一发布提交的远端六路 CI 核对。
本地 focused 结果不能称为完整 sanitizer 矩阵。

| 门禁 | 设置 | 实际结果 |
| --- | --- | --- |
| 新 C++ 与 CLI 回归 | Release，umask 0002 | C++ 16/16、CLI 6/6；2/2 CTest，exit 0，1.271 秒 |
| 全量 Release | umask 0002，CTest -j2 | 56/56，exit 0，79.127 秒 |
| 全量 Release | umask 0022，CTest -j2 | 56/56，exit 0，78.355 秒 |
| focused ASan/LSan | umask 0002，detect_leaks=1 | 5/5，exit 0，36.218 秒；无 ASan/LSan 诊断 |
| focused UBSan | umask 0002，halt_on_error=1 | 5/5，exit 0，19.409 秒；无 UBSan 诊断 |

两次全量日志各记录 444 项 S1 C++、149 项 S1 CLI、52 项 M1 C++、3 项 demo、38 项参考契约，
另有两个完整性检查组。不同层次不相加为产品验收成绩。每次 focused sanitizer 各记录
70 项 C++ 和 6 项 CLI，不将其扩大为全量用例。

五个 focused 组为 `r2a_mkdir`、`r2a_mkdir_cli_smoke`、`workspace`、`worker_tools`、`config_loader`。
构建保持 C++20、warnings-as-errors 和原 sanitizer 选项，原各 CTest 组时限没有改变。
R2.3 的本地 ASan 90 秒超时、远端结果及 CI 总时长调整在
[其独立报告](r2-model-amendment-validation.md) 保留，不用本次 focused 结果覆盖历史。

## 新回归覆盖

16 项 C++ 测试覆盖真实 0700/多 umask、创建后嵌套文件写入、已有目标/缺父目录、路径链接、
权限/只读、投影条目/深度、mkdir 竞争、打开及子/父同步失败、根/父/子替换、严格参数、
角色/grant/诊断限制、精确重放、并发首次派发、UNKNOWN、AMEND 迟到事实、业务配额和
正常结果结算及兜底结算均失败后的首次派发围栏。

6 项 CLI 测试从实际冻结配置、模型声明工具、跨进程执行进入 SQLite/真实目录，
核对原用量与操作事实、旧冻结 grant/策略、新授权以及诊断调用前后完整表内容不变。
系统调用故障注入只存在于私有测试二进制，并按夹具目录身份隔离，不拦截 SQLite 同步。

`durable` 记录子/父同步实际均已完成；最终身份核验若失败，操作仍是 UNKNOWN、结果为 null。
不能为了让测试失败分支看似统一而抹去已完成同步的事实，也不能用该字段单独放行业务。

## 红阶段与夹具修正

修正后的 6 项 CLI 测试在精确 R2.3 二进制与原配置上全部出现预期功能断言失败，exit 1，
1.043 秒。四项因旧模型契约没有声明 fs_mkdir，另两项核对新增工具/grant 失败；
其中旧冻结授权的前置断言通过，不将整个测试计为兼容性通过。

候选首次新增回归为 C++ 15/16、CLI 5/6。前者直接改了解析后的限额却未更新原配置依赖字节，
被冻结输入一致性检查正确拒绝；现改为写临时 system.yaml 并重新经 ConfigLoader 冻结。
后者用 `ORDER BY rowid` 比较所有表，遇到 WITHOUT ROWID 表；现读取完整 tuples 后稳定排序，
保留所有列值和重复行，没有改成局部表或计数比较。生产源码及配额/授权断言保持不变。
初始失败日志、初始测试源和最终红绿日志分别保留。

## 证据与发布

- [机器结果](r2a-directory-results.json)：本地全量与 focused 范围分别记录，保留红阶段和夹具修正。
- [执行输入清单](r2a-directory-inputs.sha256)：233 个输入，绑定上述提交与实际快照。
- [证据清单](r2a-directory-evidence.json)：每个原文件的大小和 SHA-256。
- [原日志与环境证据包](r2a-directory-evidence.tar.gz)：51 个文件，含初始失败与最终红绿结果。
- 证据包 SHA-256：`50690a3a6691f1e9b418fad17f486570f16250602e3bcd9789269601025530e7`。

本地环境为 GCC 13.3.0 Ubuntu 13.3.0-6ubuntu2~24.04.1、CMake 3.28.3、Ninja 1.11.1、
Python 3.12.14 和 jsonschema 4.10.3；各构建的 CLI/新测试二进制 SHA 见机器结果。
远端完整矩阵必须绑定本报告所在发布提交的完整 SHA，并核验该提交保留上述
233 个执行输入；旧 R2.3、设计提交或不同执行输入的 CI 不替代本版本门禁。
实时运行记录见 [项目 GitHub Actions](https://github.com/wmqfl861/wls861/actions)。

## 完成边界

未执行真实/付费模型调用或生产服务操作。本轮由同一模型的独立代理编写测试及审查实现，
不冒称第二模型独立业务审核。合作锁与文件代理不扩展为恶意同 UID 宿主隔离，故障注入
不等同于物理掉电验收。强 bwrap/cgroup 正向部署、R2a.2 分配恢复/显式弃置、doctor 部署
说明、R3 单协议回放及其后的获准真实闭环继续按独立增量推进。
