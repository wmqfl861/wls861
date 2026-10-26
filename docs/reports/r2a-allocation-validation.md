# R2a.2：工作区分配恢复与明确弃置验证

实现版本 **0.8.4-r2a.2**；蓝图 **v0.4.1**。设计见 [ADR-075](../adr/075-owned-allocation-recovery.md)，
行为边界见[实现契约](../implementation/R2a-allocation-recovery.md)。本批补齐 PREPARED 创建中断后的
原代次恢复与元数据放弃出口，不扩大 Worker 业务工具授权，不执行生产清理或真实模型调用。

## 1. 被测代码、输入与发布绑定

| 项目 | 已核验值 |
| --- | --- |
| 本地执行提交 | `f0744e38b702c2493d107d06c0127361797cf430` |
| 对应远端代码提交 | [11f0eb1bb66a9cbdb7e8722435f05e5613961850](https://github.com/wmqfl861/wls861/commit/11f0eb1bb66a9cbdb7e8722435f05e5613961850) |
| 两者完整 tree | `7f1b5807213916e5e4535d3861d2279e0ec3ccdb` |
| 原测试源码/资产清单 | 236 个条目，SHA-256 `ea10c5664e89a81a690833d31f875452e185a2a68b01487d831a38557480cb57` |
| 新 C++ 回归源 SHA-256 | `e59076bf8adaab162f0603ab7b0e657f36453bb3d0fe9b6d3c837d69da0783a0` |
| 新 CLI 回归源 SHA-256 | `028ca1d13d96b492ccb0b7df9c0ff7a963177da154d0c0789463f26b2eaea5b9` |

236 个快照条目与本地执行提交逐文件相同。GitHub 提交元数据不同，所以不把本地 SHA 当作
远端 SHA；上传后核对完整 tree，并读取远端分支验证实际提交。

原清单按目录收集，包含 `db/README.md`。最终文档更新将该概览扩展为408说明，因此发布文档
相对于被测代码快照，在这236项中**只有该 README 的字节发生变化**；其余235项、全部运行时代码、
测试、SQL、配置、Schema和提示词保持一致。机器结果明确记录这一文档差异，
`all_current_repository_manifest_bytes_identical:false`；不将发布树误报为236项全部相同。
最终远端完整矩阵必须绑定本报告所在发布提交的实际 SHA，不能只用早先代码提交或设计提交的CI替代。

设计定稿、实现与文档/证据分批推送。已发布 schema、004–009 SQL、原 handoff、原审核材料
以及 vendor 字节未改写；新增结构迁移为独立010。

## 2. 实现要点

新分配先提交 PREPARED 意图与所需控制结算预留，记录随机 stage/final 名称及完整父代次。
文件系统创建私有 stage、完整 marker 和空 work，同步并复核后返回创建收据；Store 以原资源 CAS
保存这份不可变收据之后，才允许 `renameat2(RENAME_NOREPLACE)` 发布。发布前再次授权，
发布后同步和核验，最后以捕获的资源版本登记 OWNED。

有收据时只恢复原 stage 或原 final 对象；两者同时存在、应有对象缺失、代次/marker不符、
非空work都拒绝。无收据残留不会被补造收据或自动收编，保留 PREPARED。
原v1完整marker精确重试保留，原意图不改写。一般文件系统失败仍通过异常返回，
本批没有新增持久失败现场观察日志；持久事实包括原意图、已成功保存的收据及后续身份/弃置记录。

`abandon-workspace` 只把目标请求下未绑定PREPARED记录变为不可逆ABANDONED，要求当前Root、
资源CAS、理由和精确命令键。CLI先只读预检，再在目标请求写者中重复核验。
它不读取或删除路径、不生成absence proof、不清UNKNOWN、不恢复暂停业务。
终态请求、退役团队、父目录缺失和容量hold仍能使用该控制出口。
原分配键返回旧记录，父目录消失或被文件替换也不访问新对象；错误父路径/任务拒绝，新键才重新分配。

64个有效资源和256条历史分别有界，均不被容量检查点清零。新事件使用独立名称，原事件分类、
旧窗口与冻结政策不改写。有效资源用于业务绑定和清理，ABANDONED的stage/final历史路径继续保护。
旧清理manifest不扩展；closeout列出保留分配并明确`filesystem_observed:false`。

010保留所有旧列原值并增加收据、弃置和部分团队唯一索引。已有请求的407升级仅允许显式全库init；
限定写者在迁移与请求恢复前拒绝，只读不升级。原嵌入C++调用省略追加资产时仍按407工作。
010结构迁移和之后请求恢复属于两个事务；后续恢复失败不撤销已经成功提交的结构升级。

## 3. 本地实际门禁

全部正式本地门禁由普通用户 **UID/GID 65534** 执行。Release使用串行CTest全量回归；
本地sanitizer只执行直接相关五组。完整ASan/LSan、UBSan、Make-ASan和Clang结果由最终发布SHA的远端CI核对。

| 门禁 | 设置 | 实际结果 | 耗时 |
| --- | --- | --- | ---: |
| 首版新增回归 | Release，umask0002 | 2/2组；29 C++ + 8 CLI，exit0 | 4.947秒 |
| 最终新增回归 | Release，umask0002 | 2/2组；30 C++ + 9 CLI，exit0 | 5.633秒 |
| 完整Release | umask0002，串行CTest | 58/58组，exit0 | 161.366秒 |
| 完整Release | umask0022，串行CTest | 58/58组，exit0 | 162.173秒 |
| focused ASan/LSan | umask0002，detect_leaks=1 | 5/5组，exit0；无诊断 | 105.686秒 |
| focused UBSan | umask0002，halt_on_error=1 | 5/5组，exit0；无诊断 | 46.822秒 |

两份完整`LastTest.log`各记录474项S1 C++、158项CLI、52项M1 C++、3项demo、38项参考契约，
合计725个计数用例，另有两个完整性检查组。不同层次不相加为产品验收成绩。
每份focused结果为82项C++和9项CLI，五组分别为`allocation_recovery`、`allocation_recovery_cli`、
`cleanup`、`cleanup_runtime`、`r2_recovery`，不称为完整sanitizer套件。

所有构建保留C++20、原严格警告/`-Werror`、原sanitizer选项及既有CTest组限时。
新C++组90秒、新CLI组180秒；原组不改。本地ASan的`cleanup_runtime`为68.24秒，低于其原90秒限时。
本轮没有修改原CI30分钟总预算，也未放宽任何旧回归断言。

| 原始日志 | SHA-256 |
| --- | --- |
| Release0002 | `e5c6bb590d20c85fb617fe19033b69071bc1b549e8e63301808190f0ceef904a` |
| Release0022 | `a7a34c8f9bcec54fc50d0f21dd0ffba7e012e8978b9a711cb719d9e1e375717e` |
| focused ASan/LSan | `3e9c5d1c2073660287d7ff91ef66e8795d306536e1b7976ccb0c684ca350019d` |
| focused UBSan | `b55ad091e58349db04a00f545bcc0cf73737b981b99388b357ceb3e5d3d89a3d` |

环境：GCC13.3.0（Ubuntu13.3.0-6ubuntu2~24.04.1）、GNU ld2.42、CMake3.28.3、Ninja1.11.1、
Python3.12.14、jsonschema4.10.3。各构建CLI/测试二进制SHA、命令、实际UID/umask和逐组计数见机器结果及环境记录。

## 4. 新回归的具体覆盖和夹具性质

30个C++具名用例中共执行**6次真实跨进程SIGKILL**：部分marker、完整stage未保存收据、
收据已保存、rename后父同步前/后、发布返回但尚未登记。重开原库验证明确拒绝或恢复同一代次。
另用SQLite触发器注入保存收据/登记失败，验证效果前/后的原始边界；注入只作用于临时测试数据。

负面矩阵覆盖复制相同marker的错误代次、stage/work/parent替换、marker更改、双路径/路径缺失、
非空work；控制回归覆盖Root/旧代次、资源CAS、精确重放、错误参数、终态/退役/hold、UNKNOWN/AMEND保持。
旧完整v1重试、部分v1弃置、原窗口checkpoint与缺省容量策略均单独验证。

旧四状态迁移从真实PREPARED/OWNED记录构造，再注入**明确标注为合成**的DELETING/REMOVED库存、
proof和manifest，验证全部原列、原事件与冻结清单字节及约束保留；这些行不证明发生了真实删除。
64/256配额和控制水位使用合成惰性记录，实际API检验边界；不声称物理创建了256个目录。
生成路径上限测试使用合成父身份，验证保存意图之前拒绝，无文件效果。

9个CLI用例使用实际独立CLI进程、SQLite和私有目录。弃置残留与新的有效OWNED共存时，
经固定脚本提交/验收、实际发布/回执和经验完成后执行分批清理，核对原manifest只含有效资源、
弃置marker/work仍保留、原交付字节不变。兄弟请求隔离比较所有归属表及完整tuples，
包括终态清理租约，不只比较计数或少量字段。

## 5. 原版功能性复现与过程记录

在未修改的0.8.3-r2a.1二进制及其原233项输入上复现半marker中断：

- 原二进制SHA：`6d4b1d602df0fc16966871ed6e6d32978680128c4a093c6815607c079a953347`。
- 原输入清单SHA：`f6ebda73b85913cddcba9e735c927ee2d8aeb80b502c7b9ad21c69169ecf39e0`，233项全部复核。
- 测试专用LD_PRELOAD shim只匹配临时目录中marker的精确fd，写7字节后SIGKILL。
- 旧库407保持PREPARED；同键conflict仍是应保留的安全拒绝；新键被已有团队分配挡住。
- `abandon-workspace`返回unknown command，证明缺少明确出口。有效功能性结果为1个预期失败、
  0个harness error、exit1、0.200秒；原日志SHA为`241c9194c585acbec23f06c0349cf7756eacc72506626a35bef71e2257332c56`。

外层runner umask0002，probe明确设置0077，因此旧CLI实际有效umask为0077；不将这项复现冒称双umask验收。
首次probe把`--source-root`误传给`--version`，在创建夹具之前exit2。原源码/日志/JSON单独保留，
不计为功能性红测。注入器C源码、精确构建命令和生成物哈希保留；生成的.so不提交。

首版新增回归29+8已通过。交叉审查此前补足生成work路径长度的创建前检查；之后发现ABANDONED
原键仍会先打开丢失父目录，于是增加定向历史查询和不访问文件系统的规范路径核对，并新增一组
C++和CLI回归。最终30+9与两轮全量/两种focused工具链均通过。初始输入差异及原日志保留，
没有把首版通过结果当成后来修补的测试证据。

## 6. 可复核证据

- [机器结果](r2a-allocation-results.json)：SHA `d1447794c535f3a40bb231df5344d1c54dc44df2415a9d1a7f3226603042aebf`。
- [被测源码/资产清单](r2a-allocation-inputs.sha256)：236项及上述唯一文档差异的说明。
- [证据清单](r2a-allocation-evidence.json)：每个原文件的大小与SHA-256。
- [原日志、环境与复现源码包](r2a-allocation-evidence.tar.gz)：54个文件，162733字节，
  SHA `41757caf72eeb2b0fbccde1031fd1eea6825f750029acc805246c6c3c5124633`。

包中保留初始测试输入、首版结果、基线功能性失败与独立harness错误、最终逐项输出、实际环境和二进制哈希。
不包含构建二进制、临时数据库、生成注入器.so或凭据。发布前再次按清单读取每个archive条目验证原字节。

## 7. 最终远端门禁与完成边界

最终发布提交需同时通过完整六路矩阵：严格GCC/umask0022、严格GCC/umask0002、Clang18、UBSan、
CMake ASan/LSan、Make-ASan，并核验同SHA源码快照。实时事实见[GitHub Actions](https://github.com/wmqfl861/wls861/actions)。
本报告记录已冻结的本地事实；最终SHA、远端运行链接与实际结论在本次交付中给出。
中间设计/代码提交成功、缺少作业的运行或取消运行都不能替代这一最终提交门禁。

本批所有模型流程为FakeModel；真实临时文件/SQLite/进程中断与合成状态夹具分别列明。
同模型协作代理的交叉审查不冒称独立第二模型业务审核。锁和fd核验不等于恶意同UID宿主隔离，
SIGKILL不等于整机掉电；强bwrap/cgroup部署、私有行purge和付费模型质量仍单独验收。
下一项[ADR-076](../adr/076-doctor-deployment-diagnostics.md)是R2a.3 doctor部署诊断设计草案，尚未实现。
