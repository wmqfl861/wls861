# wls861 独立审核与下一阶段交接报告

日期：2026-11-02
对象：PR #25 产品候选 c2a5cea64aeea261b214cdfd9a968b53f491110a
报告提交：PR #26 / f5d1fc77fb886a78bd80ccb901f7dc851ee00b7a
仓库：https://github.com/wmqfl861/wls861

## 1. 审核结论

**VALIDATION_BLOCKED；PR #25/#26 保持 Draft；不批准合并或发布。**

N5 已有实质产品实现：013 / Store 411 持久事件关联、有限异步 inbox runtime、Windows 公共资源只读身份组件。原 R-A3-01 / R-A4-01 修复被继承。本轮限定范围内没有发现足以要求撤销 N5 产品补丁的确定性新漏洞，但存在发布阻断和安全策略待明确项。

主要阻断：
- 本机 ASan/LSan：54 PASS、38 TIMEOUT、2 FAIL（94 项全部尝试）。
- 本机 Make-ASan：Store 子程序 21 PASS / 4 FAIL；四项命中原 5 秒 IPC poll 断言，其余 74 条配方未执行。
- hosted 完整私有 LastTest/JUnit 未持久保存；公开投影省略 59 个方法的逐方法输出。
- Windows image header 与环境 ImageVersion 冲突未解决。
- Windows 11 普通非提升用户、NTFS 实机验收 NOT_RUN。
- Compatibility02/03 的旧 410 拒绝符合预期，但 raw WAL/SHM sidecar inventory 仍失败。
- f5 严格证据聚合 exit 1 / NOT_COMPLETE。

## 2. 精确输入

| 对象 | SHA | 说明 |
|---|---|---|
| PR #25 产品 head | c2a5cea64aeea261b214cdfd9a968b53f491110a | 产品候选 |
| PR #25 tree | cb9c1abce1ba2efa3f190933a7844555863d4fab | 产品树 |
| PR #25 base | 79654ca9cca80d5ae8f5204df580797b04963460 | PR #22 |
| PR #26 报告 head | f5d1fc77fb886a78bd80ccb901f7dc851ee00b7a | 报告/索引提交 |
| PR #26 tree | 7c691d984a536247c5b2d78947112ba6db2d9d19 | 报告树 |
| PR 临时 merge checkout | 7c0cb09bc5ccc3e672a40ecbe16f2bd1c305b5b9 | tree 与产品树相同 |
| N0 双父整合 | f3fc9bfaf345030ee994097d2a2443e3c290e9fc | PR14 第一父、PR13 第二父 |
| PR #21 设计 | 4326ea5004889c4e14b31e26c6b86e6b0a8509b3 | 014/412 未获实施授权 |
| main | a6b8559018a599d7a806b05942857a56d69a00ce | 未合并上述成果 |

PR #25：https://github.com/wmqfl861/wls861/pull/25
PR #26：https://github.com/wmqfl861/wls861/pull/26
PR #21：https://github.com/wmqfl861/wls861/pull/21

PR #25 依赖 PR #22，并包含 PR #23/#24 的 native 修正。PR #26 是报告提交，不是新的产品矩阵输入。不同 SHA 的测试结果不得拼接。

## 3. 产品实现审核

### Store 411 / Migration 013
新增 request policy、claim snapshot、attempt/link association、selected item facts、typed receipts、unresolved/open projection。T0 claim、T1 reservation/link、T2 unique dispatch、T3 settlement、T4 database action + feedback + per-ID proof + finish subset。旧历史不合成关联；新模式显式启用新请求；旧程序拒绝 411；没有 downgrade SQL。

判断：总体遵循单 Store owner、原 Actor/CAS、UNKNOWN、预算和事务契约。T4 仅支持 plan、AMEND、instruction、数据库 note，不是完整 file/process/live executor。进程内 event_application_started/halted 不是跨重启凭据，恢复必须依据持久 receipts、原 attempt、业务事实和 open-ID projection。后续必须补 T4 前后崩溃、commit outcome unknown、反馈/CLOSE 失败、Store 重开、迟到响应和部分 ID 组合。

### Persistent Event Runtime
显式有限批次 host 复用 AsyncCoordinator；Store 仅 owner 线程访问，callback 不访问 Store。首片 FakeModel，支持 plan/AMEND/instruction/owner ack/note；没有后台 daemon、live、文件或进程执行器。

已报告：Store 22 方法、runtime 13 方法通过；21 个已知 PID SIGKILL/reap receipts；远端 Store source head 93 suites。最终产品 c2 的正式矩阵为 94 注册项。不同输入不混算。21 个 crash receipts 不等于覆盖全部机器/磁盘故障。

### SSE
within 改为 string_view，只在拒绝路径构造 owning string；原 14 项条件/限额/错误语义保持。方向合理，但没有全局性能基准，不能声称整体吞吐优化完成。

### Windows 只读身份
ReadOnlyRoot 使用 HANDLE、FILE_ID_128、卷序号、同句柄属性/ACL、NTFS persistent ACL、reparse/hard-link 拒绝、普通非提升 token 与明确 build policy；只捕获 manifest 注册公共资源。

边界：不是完整 Windows runtime、Workspace、SQLite/VFS、进程、强沙箱或 cleanup。实际证据仅 Windows Server 2022 elevated，Win11 普通用户未运行。image header/ImageVersion 冲突未解决。

额外策略问题：trusted_sids 拒绝 Everyone/Users/Guests，但未显式拒绝 Builtin Administrators SID S-1-5-32-544。ordinary_token 拒绝当前调用 token 为管理员，并不等于管理员组是否可作为资源 trusted owner/writer。此项是策略缺口待决，不宣称已证实漏洞。要求明确管理员组是否永久禁止并补负例。

## 4. 本机验证

均绑定产品 c2，不是 f5 文档提交的本机重跑。

| 配置 | 结果 |
|---|---|
| GCC 13.3 Release umask 0022 | 94/94 |
| GCC 13.3 Release umask 0002 | 94/94 |
| Clang 18 strict Release | 94/94 |
| GCC UBSan | 94/94 |
| GCC Debug ASan/LSan | 54 PASS / 38 TIMEOUT / 2 FAIL |
| Make-ASan | 21 recipe 成功；Store 子程序 21 PASS / 4 FAIL；余下 74 recipe 未进入 |

Make 四项命中原 5 秒 IPC poll CHECK(polled == 1)，poll 返回值/阻塞阶段/根因未捕获，保持 OPEN。Make 不是 CTest。123 个 unittest 方法和 native observation 工具自检不是额外产品 CTest。

## 5. 远端与证据边界

PR #25 产品 c2 的 7 workflows/17 jobs attempt 1 全成功。PR #26 文档 f5 自身 7/17 也成功，但严格证据聚合仍 exit 1 / NOT_COMPLETE。

- ASan public LastTest projection 中，9 suites 的 59 方法原逐方法输出被隐私投影省略；不能用 Make、另一 job 或另一 SHA 补造。
- hosted 完整私有 LastTest/JUnit 未持久保存。用户决定原件暂留本机；托管继续 BLOCKED，直到受限目的地、ACL/retention、authenticated readback 获准。
- tuple/list 类型比较导致 local_head_tree_delta_unchanged 误报；前后身份 JSON 实际字节相同。保留失败收据和解释，不重试制造绿色。
- 公开投影不等于私有原件。

## 6. 下一阶段计划

### P0：N5-0 验证与证据闭环
1. 在原 Make-ASan 失败输入上观察四项 IPC poll：返回值、peer 生命周期、accept/read/close 顺序、PID/start_ticks、deadline、真实退出码。单因素诊断，不延长 5 秒断言。
2. 对 ASan 38 timeout/2 fail 分组，选择代表项取得阶段日志、stderr、LastTest/JUnit、源码/ELF identity。
3. hosted 私有原件托管先做威胁模型：限权目的地、ACL、保留期、授权读回和审计；条件不满足就保持 BLOCKED。
4. Windows image 两个来源分别保存，查字段生成路径；无法解释则 UNRESOLVED。
5. Compatibility02/03 分析 WAL/SHM 生命周期、checkpoint、文件身份和采集时点；不删除 sidecar、不放宽断言。
6. 最终对同一冻结 SHA 执行适用门禁，不拼接不同 SHA 结果。

### P1：N5-1 持久 inbox runtime 强化
补 T0/T1/T2/T3/T4 前后故障、commit outcome unknown、反馈/CLOSE 失败、Store 重开、旧 Actor、部分 ID、迟到响应、AMEND/cancel、close/join。证明 T4 action、feedback、per-ID proof、finish subset 同一事务且 rollback 无半写。进程内标记不能当持久恢复授权。保持同步 API，不接 live/file/process/daemon/第二 writer。

### P2：N5-3 Windows 只读身份目标验收
先决策 S-1-5-32-544 是否允许作为 trusted owner/writer。冻结 Win11 build、MSVC/SDK、NTFS、普通 token、可信 SID。实机验证祖先 HANDLE、ACL、reparse/hard-link、路径替换竞争、对象身份、EOF/size/hash、token 变化。没有 Win11 实机保持 NOT_RUN，不以 Server2022 elevated 替代。

### P3：N5-2 最小 DAG/审核票据（单独采纳后）
PR #21 仅设计。先审图版本持久化、artifact/submission 映射、producer/reviewer 双身份、父预算借记、迁移回滚。014/Store 412 未授权，不得实施。N5-1 稳定后先三节点 a→c←b 图事务，再独立审核票据。M1 ReviewRecord/WorkClass::Review 不是 S1 授权。

### P4：真实 S1 / 更广 S2
真实供应商调用必须另行明确端点、精确模型 ID、协议能力、可信计费来源、凭据引用、预算和调用授权。持续开发授权不包含付费授权。N5-1/N5-2 稳定前不扩大 S2。

## 7. 权限边界与状态
不合并 PR #25/#26/#21 或 main；不强推；不改历史 SQL、v0.4.1、原始失败日志；不放宽权限/CAS/UNKNOWN/预算/deadline/断言/sanitizer；不调用真实模型、不付费、不操作生产数据。

IMPLEMENTATION_COMPLETE=true（仅限 N5 声明范围）
LOCAL_VALIDATION_COMPLETE=false
PUSH_COMPLETE=true
REMOTE_CI_COMPLETE=true（PR #25 产品 c2 的 7/17 jobs；PR #26 文档 f5 的 7/17 jobs；严格聚合未完成）
PR_REVIEW_COMPLETE=false
MERGED=false

本报告不授予合并、014/412 实施、真实模型、付费或生产操作授权。
