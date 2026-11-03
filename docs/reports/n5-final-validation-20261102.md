# N5 冻结候选验证汇总草稿：2026-11-02

**DRAFT / VALIDATION_BLOCKED / NOT_FORMAL_ACCEPTANCE**。本报告汇总已经实际发生的
N5-0、N5-1、N5-3 执行和失败，登记 N5-2 尚待采纳的设计包。原冻结 79654ca 的完整
Release 在 umask 0022、0002 下均实际 **94/94 PASS、exit 0**。后继候选已汇集两项
Windows 修正，已完成局部校验复用、SAX、步骤内 config 复用三轮定向实验；
三轮均 **NOT_FIXED / VALIDATION_BLOCKED**。最新 targeted03 的 runtime_types 8/8，
Store 原 120 秒超时、runtime 9/4。最终产品已冻结为 **c2a5cea / cb9c1a**，
871 项源，已推送 Draft PR25；**本机 GCC Release 0022、0002均已完整94/94 PASS、exit0，
Clang18 strict Release与UBSan也已各完整94/94 PASS、exit0；共享GCC ASan／LSan已完整失败：54 PASS／38 TIMEOUT／2 FAIL，CTest exit8、outer exit1，原Make已完整失败（step2／outer1、Store21／4，后74配方未运行）**。最终实际工具也已完成六个unittest入口共123方法，
另有native observation三个场景；它们独立于94项产品注册。本报告不把组件绿色、
工具自检、源码快照或
先前 SHA 的结果补入冻结候选的未完成门禁。当前远端证据保留 PR22 Windows 原失败，
后继 PR23、PR24 的 hosted native push / PR 事件均已通过；Windows runner 工具修正也有实际红绿。
最终 PR25 head 的 Windows Server native 两事件也各 **5 suite／34 方法通过**；
**18:07:25 UTC 最终稳定远端收据已完成**：准确 c2a5cea 的七个 workflow、17 个 job
均 attempt1、completed／success。push／PR 的 strict 双 umask、Clang18、UBSan、
CMake ASan 各94/94，原 Make ASan 两事件各94项命令按序完成。
`REMOTE_CI_COMPLETE=true` 仅适用于该准确 head 的既有 CI 范围；本机两项ASan门禁已
失败收尾，原Make后74配方未运行，Win11 普通用户验收仍 NOT_RUN，正式批准／合并仍 false。
PR20、PR22、PR23 的原 runtime 5 秒 Make-ASan 失败继续 OPEN。PR20 与 PR22 的
GCC ASan push／PR 均已完整执行 94 项，实际各 **93 PASS / 1 FAIL、CTest exit 8**；
唯一失败 suite 为 persistent runtime，方法 **12 PASS / 1 FAIL**。本机原 ASan 单 runtime
诊断为 **9 PASS / 4 FAIL**；这些失败不被其他事件绿色覆盖。

本轮原冻结产品输入为 `79654ca9cca80d5ae8f5204df580797b04963460`，tree
`2243d75d78669a6a292a1059d50033b5cfa93d21`。该输入两种 umask 的完整 Release 已完成；
后继仅 Windows 测试／工具修正已在独立 `final-candidate` 工作树普通合并 PR23、PR24
和 0c338 执行报告，工作检查点为 `cb81f74c2b3d8edf455c859c4de92378f320c451`，tree
`b33edf84d206ef89fff09d002e67ccafb2bed3e2`，870 个 tracked 文件。该工作检查点当时尚未推送；最终产品现已在 PR25 推送，
原 PR22 ref 仍是 79654ca。cb81f74 只是 Windows／报告普通合并的工作检查点；
独立局部校验复用候选通过有限非作者源码复核后，实际 configure／build 成功，但两项
选定 suite 均未通过：Store 120 秒超时，runtime 9/4。随后 SAX 增量 targeted02
也实际收尾失败：runtime_types 8/8，但 Store 再次原 120 秒超时、runtime 再次 9/4。
步骤内 config targeted03 随后也实际失败，三轮均不作为性能修复或绿色交付。最终
单一产品现已冻结为 `c2a5cea64aeea261b214cdfd9a968b53f491110a`，tree
`cb9c1abce1ba2efa3f190933a7844555863d4fab`，871 项 source；Draft PR25 以原 PR22
79654ca 为 base。`/root` 已交接完整GCC Release双mask收据：同一runner的0022、
0002各94/94 PASS、CTest exit0，源／head／binary及构建身份保持。最终实际工具
已在该准确输入上通过；Clang18 strict Release也已完整94/94 PASS、exit0。后续
compatibility02、03均实际exit1／NOT_COMPLETE：02 takeover sidecar消失，03 readonly
新增WAL／SHM，严格文件集合边界与原因仍OPEN，不能据预期拒绝或主DB哈希相同宣布兼容通过。
每个 lane 独立登记实际 source / tree / ELF，不将不同输入的结果合写为一轮。报告在独立分支
`docs/n5-final-validation-20261102-csj777e9` 的原准备阶段普通合并 Store 执行报告
`0c33889ae75e5672f08984cefd90401fa5595e86`；合并提交为
`dc57295eeca8d8e42e7badb2466651b053445243`。这次合并只新增一份 138 行报告，
不改变当时冻结产品源。随后 `/root` 已把准确产品 c2a5cea 普通合并到报告分支，当前
报告 HEAD 为 `d3563bb8c805c615d616c0da7d965bbf7b0e8e8b`，父提交依次为
dc57295 与 c2a5cea，tree 与产品 `cb9c1abce1ba2efa3f190933a7844555863d4fab`
完全相同。dc57295 保留为历史准备截点；产品执行输入仍是 c2a5cea，不改写为报告
合并提交或后续文档提交。新总报告与[机器索引](n5-final-validation-20261102-index.json)
尚未提交、推送或取得正式批准。
**本文是提交前冻结快照**：两稿untracked／PUSH=false等字段描述该准备时点。实际最终
文档提交SHA、推送和CI由PR及发布后外部收据记录，不预写未知commit或通过自指反复
改稿。最终文本／隐私followup和完整checker结果同样保留外部，不回填自身输入的hash。

本报告作者 `/root/phase_observation_review` 只读取来源、原日志和收据并编写新文档，
没有启动本地编译或产品测试。`/root` 持有本机执行窗口与集成职责，
`/root/delivery_report` 提供有界独立复核和 CI 采集；这些身份不代替主协调者的正式
架构采纳、PR 批准或合并授权。本文保留 CI 初次快照
2026-11-02 14:31:35–14:32:20 UTC，并新增 15:13:55–15:15:17 UTC 的独立快照；
本次另补入已收尾的原／派生本机诊断、原 ELF GDB、PR20／PR22 GCC ASan 公共失败
收据、PR24 source snapshot 与 Windows 独立交付收据；全部旧原件保留。

## 冻结输入、数量与交付状态

本轮从 PR15 的 `aa2c592b8aed6509189710cc6f6d69e0959098a8` 接手。主协调者已采纳
PR16 方案 B 的完整 N5-1 契约，批准新增 **013 / Store 411**。该批准不包含
N5-2 的 **014 / Store 412**。原 SQL、v0.4.1 蓝图、已发布报告和原始失败日志保留原字节。
报告准备检查比较了冻结输入中的 **270 份历史报告文件**，全部 byte-identical；
记录在私有 `evidence/n5-final-report-preparation-01/setup-and-historical-preservation.json`。

| 项目 | 本报告的实际状态与范围 |
| --- | --- |
| 产品实现 | N5-0、N5-1 opt-in Store/runtime、N5-3 原生组件已纳入冻结候选；完整产品验收 OPEN |
| 原冻结输入本机 Release | 79654ca 在 0022、0002 下各 94/94 PASS，完整目录和身份收据已核验 |
| 原冻结输入本机 ASan 诊断 | 原单 runtime 完整结束，9 PASS / 4 FAIL，原四处 5 秒断言；属于诊断，不是完整 94 项 gate |
| 首次局部复用实验 | configure／build 0；CTest 8；Store 原 120 秒 timeout、runtime 9/4，NOT_FIXED；两个新守卫后来另作 pre-SAX 候选配对 2/2，完整 suite 仍失败 |
| SAX 后继 targeted02 | 增量 build 0；三 suite CTest 8；runtime_types 8/8，Store 原 120 秒 timeout，runtime 9/4；仍 NOT_FIXED |
| 步骤内 config 复用后继 targeted03 | 已完成 NOT_FIXED；build 0，CTest 8；Store 25 方法中前 8 PASS、第 9 原 120 秒 timeout、16 未进入；runtime 9/4 |
| 后继候选本机最终矩阵 | c2a5cea / cb9c1a；GCC Release双mask、Clang18 strict与UBSan各94/94 PASS、exit0，实际工具123方法＋3场景已过；共享ASan已完整54 PASS／38 TIMEOUT／2 FAIL、CTest8／outer1，原Make已完整失败（step2／outer1、Store21／4，后74配方未运行）；compatibility02／03及已完成控制实验仍NOT_COMPLETE／OPEN，整体LOCAL_VALIDATION_COMPLETE=false，不继承79654ca结果 |
| 冻结候选推送 | 原 PR22 保持 79654ca；最终 c2a5cea 已推送 Draft PR25，冻结 source 871；该准确 head 的既有 CI 已完成 |
| 各准确输入远端 CI | **c2a5cea 的 REMOTE_CI_COMPLETE=true**；七workflow／17job全success，push／PR六路均收尾；PR22 原 Windows、PR20/22/23 sanitizer 红项及本机失败仍按原来源保留 |
| 冻结源码／文档工程复核 | 有界非作者静态复核无阻碍；三轮失败与 VALIDATION_BLOCKED 保留，非正式批准 |
| 正式 PR 审核 | OPEN；工程复核不等于正式接受 |
| GitHub 合并 | FALSE；PR16–25 相关提案／修正均保留 Draft 状态，未据本报告合并；PR25 VALIDATION_BLOCKED |
| 新总报告/index 发布 | NOT_RUN；等待完整结果与 `/root` 复核后才提交或推送 |
| 真实 WLS 模型／付费请求／生产操作 | 0；证据来自本地、FakeModel、合成夹具及托管 CI |

Linux 产品目录为 **94 个 CTest 注册项**：原 92 个名称与期限保持，新增 Store 和
persistent runtime 各一项。方法数、崩溃场景数和工具自检数另计。Windows 原生组件的
5 套、34 方法也独立于该 Linux 目录。`historical_clang_asan_failures_closed=false`
保持；历史 sanitizer / live_workflow 根因不能由本轮其他配置绿色自动关闭。

六状态分别使用 `IMPLEMENTATION_COMPLETE`、`LOCAL_VALIDATION_COMPLETE`、
`PUSH_COMPLETE`、`REMOTE_CI_COMPLETE`、`PR_REVIEW_COMPLETE`、`MERGED`。下表 TRUE
仅覆盖注明的代码／组件范围；PENDING 表示尚未收齐最终输入结果，FALSE 不被有限
静态复核自动提升。`MERGED` 指正式 PR 合并，不以工作树内普通合并代替。

| N5 范围 | IMPLEMENTATION_COMPLETE | LOCAL_VALIDATION_COMPLETE | PUSH_COMPLETE | REMOTE_CI_COMPLETE | PR_REVIEW_COMPLETE | MERGED |
| --- | --- | --- | --- | --- | --- | --- |
| N5-0 规范 JSON／证据工具及局部源码修正 | **限定范围 TRUE**；sanitizer／性能根因未闭合 | **FALSE／BLOCKED**；共享ASan完整失败与历史问题保留 | **TRUE，代码**；私有原件托管仍 BLOCKED | **TRUE，仅c2a5cea既有CI**；旧 PR18 CI仍按原输入保留 | **FALSE**，无正式批准 | **FALSE** |
| N5-1 已批准 013／Store411 与异步 runtime | **TRUE，代码已实现** | **FALSE／BLOCKED**；三轮定向失败保留；最终共享ASan完整失败，原Make已完整失败（step2／outer1、Store21／4，后74配方未运行） | **TRUE，代码** | **TRUE，仅c2a5cea既有CI**；两事件六路均完成，历史红例不改写 | **FALSE**；有限源码工程复核不等于正式 PR 审批 | **FALSE** |
| N5-2 014／Store412 生产实现 | **FALSE／NOT_STARTED**；design-only 未采纳 | **FALSE／NOT_STARTED** | **FALSE，生产**；PR21 提案已推送 | **FALSE／NOT_STARTED，生产** | **FALSE**；提案文档复核非正式采纳 | **FALSE** |
| N5-3 只读公共资源身份原生组件 | **TRUE，限定组件**；完整原生 runtime 不在该实现范围 | **FALSE／PENDING**；Win11 普通用户 NTFS 为 NOT_RUN | **TRUE，组件代码** | **TRUE，仅c2a5cea既有CI／Server组件**；native两事件各5套34方法，非Win11目标验收 | **FALSE**；组件工程复核非正式批准 | **FALSE** |

N5-0 的局部工具／源码实现不关闭原 sanitizer、live_workflow 或 SIGTERM20s 问题；
私有原件目的地／ACL／保留／授权仍 BLOCKED。N5-1 不因实现、有限守卫或源码复核
通过而取得 LOCAL_VALIDATION_COMPLETE。N5-2 只记录提案推送与文档工程复核，
不授权 014／412。N5-3 的 Server 组件 CI 与 Win11 普通用户目标保持独立；
本轮 REMOTE_CI_COMPLETE 的 TRUE 绑定最终 c2a5cea／cb9c1a 的既有工作流，既不
补填本机矩阵，也不授权未采纳的 N5-2。当前没有一个范围取得正式 PR 批准或 GitHub 合并。

## PR 依赖与准确来源

下表是报告准备时读取的 PR 元数据。所有条目均为 OPEN Draft，未合并；base 是真实
分支依赖，不代表下游当前工作流已经在最新 base 重新执行。完整分支名在机器索引中。
报告分支当前已由 `/root` 普通合并准确 PR25 产品输入至 d3563bb；这是本地报告依赖
更新，不改变各 GitHub PR 的 Draft／未合并状态。PR25 的实际远端 checkout 另由下文
原 job log 绑定。

| PR | 范围 | head SHA | base 依赖 |
| --- | --- | --- | --- |
| [16](https://github.com/wmqfl861/wls861/pull/16) | N5-1 设计、013/411 契约 | `451b232a0dde08220a5e096d91209087b027b42e` | `work/coordinator-n0-n4-20261102-0k9` / PR15 |
| [17](https://github.com/wmqfl861/wls861/pull/17) | N5-3 只读公共资源身份 | `af3bd2b14ed497a57453c0261f26d0c6b52190be` | PR15 分支 |
| [18](https://github.com/wmqfl861/wls861/pull/18) | N5-0 规范 JSON 与私有证据工具 | `77cf51265211d9f766926dfb5e6aad59c1604d94` | PR15 分支 |
| [19](https://github.com/wmqfl861/wls861/pull/19) | N5-1 Store 411 持久关联 | `0c33889ae75e5672f08984cefd90401fa5595e86` | PR16 分支 |
| [20](https://github.com/wmqfl861/wls861/pull/20) | N5-1 异步运行时连接 | `97e3c3f75e35b674d2a254aa0a80bf89ccdf9a59` | PR19 分支 |
| [21](https://github.com/wmqfl861/wls861/pull/21) | N5-2 存储设计提案 | `4326ea5004889c4e14b31e26c6b86e6b0a8509b3` | PR16 分支 |
| [22](https://github.com/wmqfl861/wls861/pull/22) | 冻结统一候选 | `79654ca9cca80d5ae8f5204df580797b04963460` | PR20 分支 |
| [23](https://github.com/wmqfl861/wls861/pull/23) | Windows 精确资源常量修正 | `a99ad867a25ad0404bf4b4b7e6a5b76d446022d0` | PR22 分支 @ `79654ca` |
| [24](https://github.com/wmqfl861/wls861/pull/24) | Windows 失败路径源／ELF 身份收集 | `0c7a7c2478c532fc5114e178df8e63bbc93a8070` | PR23 分支 @ `a99ad867` |
| [25](https://github.com/wmqfl861/wls861/pull/25) | 单一最终产品输入、三轮候选验证后冻结 | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | PR22 分支 @ `79654ca`；含 PR23／24 普通合并；VALIDATION_BLOCKED |

PR19 的产品／测试执行输入为 `bef9cec095e972f7d5c7ff7e14ff917a9c43c4fe`，tree
`df06e15ecb5648ee75baa6b6fd64e37b9052a7aa`；其后 0c338 只新增执行报告。PR20 的
CI 快照实际读取到 base `bef9cec...`，PR job checkout `ef42f466...` 的两个父提交是
该 base 与 `97e3c3f...`。这只能证明当次 checkout 的关系，不能用它声称最新 PR19
文档 head 已被该 job 测过。CI 元数据的 merge SHA 本身不作为 checkout 见证。

## N5-0：原完整失败与同 ELF 布局对照

N5-0 将规范 JSON 连续普通字节按段追加，保留 Boost 转义字节和完整最终验证，并新增
私有 capture / pack / verify 工具。具体源码、确定性字节 oracle、非作者复核及原诊断
见[原执行报告](n5-0-execution-validation-v2-20261102.md)和其
[原机器索引](n5-0-execution-index-v2-20261102.json)。这些原报告保持原时域。

| 实际执行 | 输入和完整程度 | 原始结果 |
| --- | --- | --- |
| 原完整 Release，umask 0022 | 77cf512，原 92 个注册项 | 90 PASS / 2 FAIL，CTest exit 8，1948.131 秒 |
| 原完整 Release，umask 0002 | 同源同一构建，原 92 个注册项 | 90 PASS / 2 FAIL，CTest exit 8，751.213 秒 |
| 同 ELF 布局对照，0022 | 同字节 ELF 改为可发现源资源的位置；仅选两项，26 方法 | doctor 12/12、review 14/14，CTest exit 0，9.577 秒 |
| 同 ELF 布局对照，0002 | 同上，独立新夹具与隔离证据 | doctor 12/12、review 14/14，CTest exit 0，8.551 秒 |

原 configure / build 均 exit 0，build 2125.833 秒；完整矩阵 launcher exit 1，已回收，
没有信号或强制终止。原失败注册项是 `doctor_diagnostics_cli` 与 `review_cli_smoke`。
源码诊断定位到构建在 SOURCE 旁边，未提供可发现的 manifest：doctor 预期 SOURCE 根，
review 的未知命令先在资源 preflight 返回 Unsupported。该源码解释与同 ELF 布局对照
支持布局前置条件的诊断；**原两个 90/2 结果继续是失败，不能改写为 PASS**。

同 ELF 对照没有重编译，只改变两项原 argv 中的二进制路径，保留原断言、CTest
90/120 秒、Python 子进程 30/35 秒以及 Recorder 5 秒回收期限。源、两个 ELF、受保护
原件和 HEAD 均不变，结束后无遗留进程。它使用新夹具、不同调度时机，分类为
`SAME_ELF_LAYOUT_CONTROL_NOT_CLEAN_BUILD_NOT_FULL_GATE`，不复现原机器负载，
不替代最终候选的干净构建与完整 94 项矩阵。

PR18 精确输入 push run `41445539633` checkout 77cf512，PR run `41445621086`
checkout `a276ded542b6fc17a21b2f61b613735db19214c4`，后者 tree 与源 tree
`74452124aec039bd783cdaf3e7d04845fabb2e19` 相等；两个事件各六条 Linux lane 完成成功。
源码 artifact 的 828 个文件逐字节验证，tar SHA-256
`fc31ad70dd78b280d8a1302040ab545b160dbdbc241f2e2f54047cf62f5d0479`。
这些远端结果只属于 PR18 输入，不关闭原本机失败或历史 sanitizer 根因。

私有工具曾有 16 方法通过，后继实际为 15 PASS / 1 ERROR，SIGTERM 原 20 秒期限
超时；单方法 strace 后继通过不足以关闭原原因。早期 GDB launcher 的 TypeError
造成三个诊断重叠／缺失收尾，按 `HARNESS_ERROR` 保留；后继被动诊断的
structured / capacity / escaped 各方法在原期限单次通过，只记
`DIAGNOSTIC_NOT_GATE / OPEN_NOT_REPRODUCED`。hosted 完整私有证据持久化仍为
**BLOCKED**：获准目的地、ACL、保留／删除、最小身份与实际读取回验尚缺。

## N5-1：Store、原运行时事务与实际崩溃执行

冻结候选实现显式 opt-in 的持久 inbox/call 关联。原 claim、预约、唯一派发、原结算、
业务效果、feedback、逐 ID 完成凭据在原 T0–T4 边界连接，业务效果与 feedback / ID
证明在同一写事务提交。唯一 owned Store 仍只由 owner 线程访问，callback 不访问
Store；原同步默认路径保留。旧请求不能补开新能力，填充 410→411 仍需要原全请求
维护准入。详见[采纳与实现说明](../implementation/n5-persistent-events-20261102/adoption-and-implementation-20261102.md)。

本地集成人实际执行 `evidence/n5-integrated-targeted-build-08`：输入
`0458806227a7d76698428c197f2b9f74de57c954`，tree
`c3473482be2086955ac7dfd511afb67ac2f65f0f`，GCC13 Release，ASan/UBSan OFF。
configure / build / CTest 全 exit 0；94 个目录中仅选择两项，Store **22/22**，
29.0172 秒，runtime **13/13**，5.87515 秒。867 项源身份前后相等；Store 120 秒原期限
未改。此结果属于 045880 的两项定向执行，不能记作 79654ca 的完整 94 项门禁。

完整 `LastTest.log` 实际含 **21 个互异正 PID 的 SIGKILL / reaped=true 收据**：
W00–W10、W11_prepare、W11_start、W12、W13、W14_first_id、W14_close、W15、
W16_amend、W16_cancel、W19。W00–W06 的目标 callback 为 0，后续各为 1。收据
覆盖事务内回滚、已提交 T0/T1/T2/T3/T4、真实 AMEND/cancel、原 claim/link/member/
receipt 原字节、UNKNOWN 与预算保留、死 session 拒绝、CLOSED replay 零新增派发／
效果，以及两个 ID 不部分完成。完整逐相位表与 35 个方法核验在
[实际执行报告](n5-persistent-event-process-crash-execution-20261102.md)。

W19 使用**当前测试 ELF 的 Store 410 兼容模式与原同步接口**，实际产生 note 已提交／
feedback 未写的历史，再真实 kill；它没有执行真正旧版 410 ELF。
升级 411 后实际为 E=1/F=0、零 links，无伪造关联或完成证明。公开命名的
`UNLINKED_HISTORY/HOLD_MISSING_FEEDBACK` classifier API 不存在，本结果只证明
保存的原事实和没有发明关联。W17 实际通过 callback join；W18 与 Store T4 的实际
`SQLITE_IOERR` 注入验证保留／flush 不重做业务，它们不另算 SIGKILL。

JUnit 的每套 stdout 只保留 1024 字节，因此方法与 PID 根据完整 10688 字节
LastTest 核验。复制的 26 字节 `LastTestsFailed.log` 与运行 06/07 相同，是保留的
旧文件；运行 08 的当次 CTest exit 0、JUnit 零失败与完整 PASS 输出共同确定本次结果。
Store ELF SHA-256 `f17fc59b71f773b82da040c604eb8ad3d50a20e3c46af7bd3d5d90b6ba9b662c`；
runtime ELF `f506c3b3648f7d110dff2c8850eeab3b916496eefb5ffe86855d4c16cd1e9511`。

Store 源输入 bef9cec 的远端 strict **push** run `41459049325` / job `125323084664`
实际 checkout bef9cec，93/93，344.04 秒；Store 10.81 秒。strict **pull_request**
run `41459058440` / job `125323113864` 实际 checkout
`7db66d540b8e906f703c9904c6c254ea84f90229`，父为 451b232 + bef9cec，tree 与 Store
源 tree 相同，93/93，207.57 秒；Store 6.30 秒。93 是 Store 分支的注册数，不是集成
94。后继文档 head 0c338 的工作流另计，不能从 bef9cec 继承全绿。

## 必须保留的编译、夹具与兼容红绿

| 实际检查点 | 原失败与最小后继修正 | 结果边界 |
| --- | --- | --- |
| Store `88c8259` | 远端实际 38 个 string/Json 比较编译错误、1 个 shadow；`28cf4b5` 改正 | 原编译失败保留；不是产品反例通过 |
| Store `28cf4b5` | strict 实际 93 项 85 PASS / 8 FAIL，Store 14/17 | 后继分离兼容、Store 与 runtime 夹具，不删除原断言 |
| Store `9031772` / targeted04 | Storage 预期实际 Conflict，因为 RAISE(ABORT) 返回 SQLITE_CONSTRAINT；410 enable 测试实际 InvalidInput | `0860966` 只在测试注入真实 SQLITE_IOERR，并使用合法 ID prefix；原 Storage/NotReady 断言保留 |
| targeted05 | 测试新增命名空间使全局 test 歧义，实际编译失败 | `daa35d3` 明确 `::test::persistent_events`，保留 stderr |
| targeted06 | runtime 13 PASS；Store 16 PASS / 1 FAIL；误将 joined f.task() 与 raw Store::task 比较 | `c8de76a` 分别比较前后完整 raw task 和 joined task 两份视图并输出键差异，保留全部业务断言 |
| targeted07 | 6/6 被选 suite，Store 17/17，runtime 13/13 | 仅该次实际选择范围 |
| bef9cec / targeted08 | 新增五个方法执行 21 次真实 kill | Store 22/22、runtime 13/13，见上文 |
| 411 evaluator `cfd9074` | 真实 411 库旧 allowlist 在 reader 前拒绝；相同三个测试旧 exit 1，2.395 秒 | 唯一产品行把已知 411 加入 allowlist 后，同测试 exit 0，2.539 秒；未知 412 仍拒绝 |

411 修复保留 URI `mode=ro`、`query_only`、单读事务、quick_check、长度／执行期限与
原全部业务、来源和结算守卫。四处当前 CLI 版本断言由精确 410 更新为**精确 411**，
没有改成允许集合。三个新增方法仍在原 Evaluation suite 中，不增加注册项。测试
前后源码 SHA-256 `a525b9533fe7b0f650f93aff7b3d5632448df97ab0c6c3d48354b691a53a12ba`。
同一原始 411 夹具旧 evaluator 拒绝、新 evaluator 接受，DB bytes / rows 不变、模型调用 0。
旧／新 evaluator SHA 分别是 `89bc317e52f90471d07e2c6ec6f84bf3c55e11a9f87ec177812c7dd6efd76a47`
与 `770d2200a9086e53552c39458d0f57e880843b89ed45925a7994d9679d73348e`。

该兼容测试使用集成人已建的 f0208f9 真实 411 ELF，SHA
`2a673b74345e7ab82f69275050d8656d05d610c53a02b5bd37f4b743faacfc37`，
**不是 28cf4b5 精确重编译**；资源输入已逐字节相等。历史 408–410 的方法执行原发布
DDL 形成空 schema，证明 reader 空 schema 兼容，不声称历史已填充业务 E2E。
[兼容原报告](n5-1-store411-evaluator-compat-20261102.md)保留完整限制。

历史 **capability／compatibility01** 收据
`evidence/n5-1-capability-and-compatibility-01/summary.json` 对旧 410 probe 编译 exit 0、
运行 exit 1，新 411 probe 编译 exit 0、运行 exit 0；真实 411 init exit 0，旧 410 ELF
打开该库实际 exit 2，**仅该01原收据与其准确输入下数据库不变**。分类是
`CAPABILITY_DIFFERENCE_AND_ACTUAL_OLD_ELF_REFUSAL`，证明能力差异和真实旧 ELF
拒绝，不把 410 未提供 N5 能力标成旧生产缺陷；不覆盖后文最终compatibility02的
NOT_COMPLETE及sidecar集合差异。

## N5-3：托管原生结果与尚未完成的机器／策略验收

PR17 源 af3bd2，tree `2accae74438863c9280c70b95a555502018614df`，已经正常推送并
读回。Windows Server 2022 / elevated / NTFS 的实际 runs `41447873808` 与
`41447882220` 各通过 **5 native suite / 34 方法**，另各有原 runner 工具 16 方法和
provenance 工具 12 方法通过，后两者不增加产品套数。源码快照 836 文件核验。
见[原组件报告](n5-windows-readonly-validation-20261102.md)、
[stat 诊断](n5-windows-stat-diagnostic-20261102.md)与
[stat 修订](n5-windows-stat-correction-20261102.md)。

原 540fd7d / 8d4bf58 jobs 在 C++ 前的工具阶段失败。实际 Python 3.12.10 的 path
ctime 对应 CreationTime、descriptor ctime 对应 ChangeTime，稳定对象的两者不同；
修订仍保留原生 128-bit file ID、volume 及 before/after 身份字段，无弱化 fallback。
Linux 初版 rewrite fixture 因 tick 碰撞没有建立 ctime 变化而失败，原记录保留；
20ms fixture interleave 只建立真实变化，原断言保持，最终 af3 的托管结果如上。

历史 runner header 表示 Windows2022 `20261025.346.1`，继承的 ImageVersion 为
`20261026.347.1`，两来源仍冲突，实际比较 exit 1 / **UNRESOLVED**。没有选择有利
来源或发明原因。**Windows 11 x64 普通非提升用户 / NTFS = NOT_RUN**，受信 SID /
build 部署策略为 **POLICY_PENDING**。完整 native Store/VFS、任意文件／进程、强沙箱、
退出见证与 cleanup 为 NOT_IMPLEMENTED；托管组件成功不等于完整 Windows runtime。

## 两次远端快照的实际红项、后继结果与未完成项

CI 采集器从各完成 job 原日志读取 `git log -1 --format=%H`，再查询实际 commit 的
tree / parents；push 要求 checkout 精确等于 head，PR 则绑定当次记录的 base/head。
源码 artifact 只在内存核成员与 bytes，未解包执行。完整私有快照
`evidence/n5-pr19-pr20-pr22-remote-status-01/status-summary.json` 的 SHA-256 为
`143778af62eda5fb0eb47c6ecd3d1810ca66b43d1909bfeabec4c0c43fd7514c`，
冻结清单为 `7e879c55f669c4d867ff8bff6e88f1322d2654324c66212a9fdfbef3279b2946`。

下表保留**首次快照时域**，后继变化另列，不覆盖原证据。

| 准确执行 | 实际 checkout / tree | 首次快照结果 |
| --- | --- | --- |
| PR22 push Windows run 41461451274 / job 125331237861 | 79654ca / 2243d75 | **FAIL**；native_readonly 6 PASS / 1 FAIL，CTest 4/5，exit 1 |
| PR20 pull_request Make-ASan run 41459402615 / job 125324281802 | ef42f466 / 6615b9f | **FAIL**；runtime 12 PASS / 1 FAIL，Make exit 2 |
| PR22 push source run 41461451329 / job 125331237768 | 79654ca / 2243d75 | 867/867 源文件相等；只证明源码快照 |
| PR22 Linux push / PR 各六路 | 快照时未有完整 checkout 收据 | queued / in_progress；OPEN，不填 PASS |
| PR22 A4 push / PR、Windows PR | 快照时未完成 | OPEN |
| PR19 0c338 Linux push / PR | 快照时未完成 | OPEN；该 head Windows push、A4 push 与 source 已成功 |
| PR20 97e3 Linux push / PR | strict、strict-0002、Clang 已完成成功；其余如上 | 整体 INCOMPLETE；PR Make-ASan 红，不继承父 head 绿色 |

PR22 Windows 原日志的 `native_readonly` compiled public capture 第 57 行仍断言
`captured.count() == 35`，而 configure 的授权资源 `asset_count` 已是 36；该次实际
测试失败保留。另两个精确旧断言是 bytes 160549 与旧 b9f546 前缀的 manifest hash，
尚未随 013 更新为授权资源的 **36 / 166684 /
f03bce7bb55b02f7740c81473c12301ecc437b35d980d629fb2d896196d4dc30**。
后继独立测试修正已经正常发布为 [Draft PR23](https://github.com/wmqfl861/wls861/pull/23)，
head `a99ad867a25ad0404bf4b4b7e6a5b76d446022d0`，tree
`f5b49bb7cef4c9f38bd849caa6f439998520dddb`，parent 为冻结 79654ca，base 为 PR22 分支。
只改三个精确测试常量并新增一份报告；`/root` 完成非作者工程复核。没有本机编译／产品
测试；后继 hosted native 的两个事件通过见下文，完整远端 CI、正式审核／合并仍未完成；
冻结 79654ca 不改。不能将静态常量
一致、发布完成或 source artifact 当成产品通过。
原日志 SHA-256 `0f8f4a8697ef960c4fa43b91e76cfe00403307a3b6f841ef314fdc4de2489f7b`。

真实失败 artifact `13149561362` 的 36 个成员、API digest 和 job upload digest 已核验，
archive SHA-256 `ad482a97405e34dacc089fd4a6bab4f1aae5c9ad40ee7bea3225134201c80a49`。
原 35 个资源逐项不变，唯一新增 013 的 6135 bytes；全部 36 个实际资源 bytes 已验证。
静态来源／失败 artifact 核验冻结清单为
`d6721baec8aaea49c741a0022739b6c3ce6326cdf62cd85e77a50f932d39bc25`，
PR23 发布收据 SHA-256 `0fbb5ca0c5204fc9d5b3183dcceeb5c2ea689ae8ba76dcae0c8b2b9e22115b0e`。

本地集成人另从同一次实际失败 artifact 确认 runner 的失败路径缺少 source-after 与
binary-before / binary-after 身份收据；这是第二个独立工具问题。独立最小修复已发布
[Draft PR24](https://github.com/wmqfl861/wls861/pull/24)，head
`0c7a7c2478c532fc5114e178df8e63bbc93a8070`，tree
`1b17b29e18a4a0f4424e0c8a861d8a330aa0f67d`，parent 为 a99、base 依赖 PR23。
工具修正不改变产品权限、原期限或退出结果；原失败 artifact 的缺证据状态保留。

该工具的实际红绿使用**同一份新测试、同一 synthetic fixture source、同一执行条件**：
旧 a99 runner exit 1，unittest 报 6 failures + 4 errors；修订 runner 22/22 方法通过、
exit 0。总计是 **1 个工具 suite / 22 方法**，其中 mutation 方法含两个 subcase，
不能把 failures/errors 或 subcase 当成新的独立 suite。原 16 方法加强并新增 6 方法；
两次源均前后恒等，无 timeout 或遗留进程。本机未编译、未执行真实 native 产品，
使用 synthetic discovery/build/install，CTest 进程与日志是真的；timeout/interrupt
由明确 fixture 注入。工具通过不增加 5 个 native suite 或 94 个 Linux 注册项。

非作者 `/root/concurrency_fix` 提出的 F1 是输出收集异常掩盖真实非零 CTest exit 8。
修订先保存原退出状态，再独立收集输出；实际回归保存 exit 8 与次生 capture error，
没有标为 PASSED。F1 修正后复核范围内无剩余 blocker，正式 PR 批准仍为 false。
执行时的 868 个输入 bytes 与随后提交树全部相等，第 869 个新报告明确不是运行输入。
工具红绿 summary SHA-256 `a4eaec2d10be09dfbadbd9ace8bf4579a064a45efbe9badaf1712c30e582eec9`；
非作者复核 `cb343d2897ae142412f269e0f73b279256167fa535a178f948934cb0b47e9edd`；
发布收据 `2085e0140f93f1b400ca6a31187fb7ab84501cebcd4cd3a4bb21d52dced4eee7`。
该准确 head 的 CI 在 15:15 快照仍 pending；随后真实 Windows 组件结果在 15:22 收据中
单独核验通过，见下文。工具红绿与真实组件结果各自保留原输入和范围。

PR20 Make-ASan 的失败方法是 “two requests and two independent nodes advance while
one callback waits”；原第 283 行 5 秒 wait、原第 187 行 `checked < deadline` 断言
实际失败。这里只记录真实超时／断言，不归因为机器负载或宣称生产缺陷已定位。
初次快照没有 rerun；后继诊断与处置仍 **OPEN**。

后继快照 `evidence/n5-pr19-pr20-pr22-pr23-pr24-remote-status-02/status-summary.json`
于 15:13:55–15:15:17 UTC 独立采集，原 01 不变；summary SHA-256
`21065e62fe6622bbaf662982f5df51952763665627eaa53aa438e74b8693c21a`，
manifest `bee9f9734b2776489bab7859a72ac836f970bd8d1edf0a80b83a60d72d1537ef`。

| 后继准确执行 | 实际 checkout / tree | 实际结果与限制 |
| --- | --- | --- |
| PR23 Windows push run 41465501759 / job 125345166478 | a99ad867 / f5b49bb | SUCCESS，5/5 原 native CTest；source snapshot 868 文件逐字节等于 tree |
| PR23 Windows PR run 41465516463 / job 125345220172 | c2604be2 / f5b49bb；父为 79654ca + a99 | SUCCESS，5/5 原 native CTest；不关闭 Windows11 普通用户／SID／image 冲突 |
| PR22 Make-ASan push run 41461451642 / job 125331240229 | 79654ca / 2243d75 | SUCCESS，只属于这个事件与配置 |
| PR22 Make-ASan PR run 41461687728 / job 125332057073 | 3497b950 / 2243d75；父为 97e3 + 79654ca | FAIL：同一原 runtime 5 秒方法，12 PASS / 1 FAIL，Make exit 2 |
| PR23 Make-ASan PR run 41465516422 / job 125345218297 | c2604be2 / f5b49bb | FAIL：同一原 runtime 5 秒方法，12 PASS / 1 FAIL，Make exit 2 |
| PR20 GCC ASan push / PR jobs 125323755016 / 125324281724 | 97e3 / ef42；原 tree 6615b9f | 两个 job 均 FAIL；后继公共 artifacts 确认各 94 项全执行、93 PASS / 1 FAIL，runtime 12 PASS / 1 FAIL；原 runner 字段 NOT_COMPLETE 保留 |
| PR24 0c7a7c 的各工作流与源码快照 | 快照时 queued / in_progress，未有完成 checkout 见证 | OPEN，整体 REMOTE_CI_COMPLETE=false |

随后在 **15:21:58–15:22:44 UTC** 核验了实际 Windows artifacts：
`evidence/n5-pr23-pr24-native-artifacts-01/summary.json`，SHA-256
`ecf6d1e1f0c2a05aa530302ebc6122043188ba04655ca85e02d6b62ff76ac1cd`，
冻结清单 `22d8b53f45a8aafe7011db8241ad7a007ab438729a139da2da3a32c36c6faecf`。
四个 run 的 raw log、ZIP 的 API / upload digest、逐成员来源与完整 LastTest 均保存并核验，
没有执行 archive 中的代码。

| 实际原生组件结果 | 准确 checkout / parents | 完整 artifact 核验 |
| --- | --- | --- |
| PR23 push / PR，前述两个 job | a99 / c260，两者 tree f5b49bb | 各 **5 suite / 34 方法 PASS**；868 source before=after=commit。旧 runner 仅有 4 个原 executable hashes，仍缺 binary before/after pair，不补造该证据 |
| PR24 push run 41469098253 / job 125357641077 | `0c7a7c2478c532fc5114e178df8e63bbc93a8070`，parent a99 | **5/5 原 CTest、34/34 原方法 PASS**；869 source before=after=commit，4 个 executable 的新 before=after=原 hashes，identity complete、无 errors |
| PR24 PR run 41469114207 / job 125357695545 | `ab1c42e8fa387fe99ebe51a1d1b5edb00a79d703`，parents a99 + 0c7，tree 1b17b29 | 同样 **5/5、34/34 PASS**，869 source 与 4 个 executable 配对身份完整且恒等 |

PR24 source snapshot 已于 15:24:55–15:25:25 UTC 独立核验完成：run `41469098242`，
artifact `13154917062`，**869/869** 成员 mode／bytes 对应准确 0c7a7c tree；ZIP SHA-256
`ed07f09e6cde268799ee0731414ce8505fede2094cb157954fecd8392ded6e95`。收据
`evidence/n5-pr24-remote-status-03/status-summary.json` SHA-256
`cf0501cbe9cf8e35a94e726e2a8ffa0204752e8048e6a6fa5a16ba0d434df757`，manifest
`fe19d5355db3a29fc5ab5b6b040c1db84978fbf0cc3c9c14fe36cc6eaca39fc1`。
source snapshot 只证明来源；当时 Linux／整体 CI 仍未通过。

两项 Windows 修正的独立交付入口为
`evidence/n5-windows-corrections-delivery-01/delivery.json`，SHA-256
`a4e695aa63a7d743dfb563af8e9ebf5d0513deeeaacc28bc226a77b5cedae0a6`，manifest
`719411d761611411cc21ca98a7e84bec1be74de373ea294b7b2524bc37785dc0`。该收据固定
PR23／24 的 full SHA、tree、分支依赖、原红、工具 22 方法红绿、有限非作者复核和上述
四路真实 Windows 5 suite／34 方法结果，PR24 另有四组 executable 前后身份。其
`REMOTE_CI_COMPLETE`、`PR_REVIEW_COMPLETE`、`MERGED` 均为 false。

这些新结果属于 hosted native component；Linux／全部远端门禁仍未闭合，Windows11
普通用户验收仍 NOT_RUN，原失败路径 artifact 的缺失身份不被后来成功运行改写。

02 还独立读取 branch ref：PR20 真实 base ref 已是 0c338，PR API metadata 仍报 bef9；
旧 ef42 的父确实是 bef9 + 97e3，所以只能作为旧执行输入，不绑定当前 base 关系。
metadata 的稳定不等于准确，当前 ref 与日志见证分别保存。

79654ca 已含 JSON 优化，但上述原 5 秒 Make-ASan 方法仍实际为红；该优化没有消除
这次观察到的失败。根因、性能贡献与处置继续 **OPEN**，不归因为负载，也不放宽期限。

后继已收到完整的 GCC ASan 公共失败证据。下面四路都实际执行完 **94 项**，结果为
**93 PASS / 1 FAIL、0 skip、0 notrun、CTest exit 8**，唯一失败 suite 是
`persistent_event_runtime`，该 suite 都是 **12 PASS / 1 FAIL**，失败方法同为
“two requests and two independent nodes advance while one callback waits”。这是完成的
失败执行，未发生 CTest 总 timeout；runner 顶层 `status=NOT_COMPLETE` 是原失败状态字段，
不能据此描述为尚未收尾。原 Make-ASan push 绿／PR 红也仍各自保留，不与 GCC ASan 合并。

| 已收尾的完整 GCC ASan | 实际 checkout / tree | CTest 秒数／原 runtime 秒数 | 公共 artifact |
| --- | --- | --- | --- |
| PR20 push，job 125323755016 | 97e3c3f / 6615b9f | 2519.674662 / 18.9256 | 13151282260 |
| PR20 PR，job 125324281724 | ef42f466 / 6615b9f；原父 bef9 + 97e3 | 2545.046047 / 18.6517 | 13154335852 |
| PR22 push，job 125331240402 | 79654ca / 2243d75 | 2319.129699 / 17.3729 | 13155980027 |
| PR22 PR，job 125332056771 | 3497b950 / 2243d75；父 97e3 + 79654ca | 2396.503830 / 17.4957 | 13156381590 |

每路 26 个公开索引成员均已核验。PR20 两路各 847 个 source mode／hash 对应准确提交，
63 项 binary／build identity 前后相等；PR22 两路各 867 项 source 与 binary 身份也前后
相等。PR22 与本机分别创建的 checkout 内容 bytes 一致，普通 mode 为远端 0644、本机
0664（867 项），Git executable bit 一致；该差异原样登记，不声称跨环境 mode 相等。
公开 LastTest 只披露失败方法，异常正文仍隐藏 **97 bytes**。不能补造 private 全文或仅凭
这一投影断定准确断言行；同 tree 的 Make-ASan 原 raw log 另行见证第 283／187 行。
private hosted 原日志持久化／读回缺口继续 OPEN。

PR20 独立 assessment 为 `evidence/n5-pr20-gcc-asan-public-assessment-01/verification.json`，
SHA-256 `7861dd150bf28e4d45130d3cc886e1319addc645373602848290c6c4fafd367f`。
PR22 完成入口 `evidence/n5-pr22-796-asan-watch-01/complete-observation.json`，SHA-256
`0bae5d93b972d29689ae15bbefb051a44a23f7636586778055d0e0dca783e616`；四个 watched
job 均 terminal，observer exit 1 表示未全绿。采集只读，没有发起 CI rerun。

## 原 79654ca 的本机诊断与局部复用候选

本机原源诊断由 `/root` 持有单一执行窗口，GCC13 Debug `-g`、ASan 下只选择原
`persistent_event_runtime` suite，原 180 秒 suite 期限与各处 5 秒 `until` 均保留。
实际 CTest 已完整结束，**9 PASS / 4 FAIL，exit 8，79.303201 秒**；原 LastTest 记录
79.26 秒。失败分别发生于原第 **283、326、369、569** 行的 5 秒等待，统一在第 187 行
`checked < deadline` 断言失败；其余九个方法通过。该执行不是完整 94 项 ASan lane。
source 与 ELF／构建身份前后恒等。summary 原字段 `NOT_COMPLETE` 保留，收尾见证
`execution_completed=true` 与完整 LastTest／JUnit 已核实。

派生观测只重新编译添加观测的单个测试翻译单元，链接同一份原生产 archive；生产源、
原 ELF、archive、配置与资源不变，方法顺序、原断言及 5 秒期限也没有放宽。派生 suite
同样完整结束为 **9 PASS / 4 FAIL、CTest exit 8，78.176564 秒**；它属于
`DERIVED_TEST_TU_DIAGNOSTIC_NOT_FULL_GATE`，不能替代原产品 gate。

首失败发生于 `before_gate_release`：仅两次 host `poll` 合计 **5.675589164 秒**，
单次分别约 3.101375 和 2.574214 秒，`gate_entered=0`；当时只建立两个模型，两个
dispatcher 回调已完成，coordinator 有 **2 ready results、2 outstanding**。两个请求的
原 link 均为 DISPATCHED，各还有一个 pending ID；successful note 数为 0。该观测说明
方法尚未进入要等待的 worker gate 阶段，不足以证明 callback wave barrier 或调度死锁。

另直接对原 ELF 做 GDB 名栈诊断，实际 **89.507002 秒、exit 1**，原方法结果仍是
9/4。16 个样本从首个原 `until` 断点开始：**样本 0–7** 位于 owner 的同步 CPU 路径，
包括 Store request／exposed JSON、capture／submit 配置恢复、T1 的重复 FrozenConfig
恢复与 reservation canonicalization；**8–9** 已是首超时后的 evidence-only cleanup，
**10–15** 属于后续方法的 setup／capture，不能全计入首个 5 秒窗口。其他线程多处于
condition-variable 等待。stderr 另有 **LeakSanitizer ptrace fatal**，明确指出 LSan
不能在 ptrace 下工作。没有关闭 leak 检测；GDB／ptrace 开销是显式受控变化，所以这次
执行分类为 **ORIGINAL_ELF_GDB_DIAGNOSTIC_NOT_GATE**，不是 ASan／LSan 验收。
名栈支持重复配置／上下文验证成本的假设，但不是定量 profile，不能给出成本占比、候选
速度收益，或断定四个失败只有同一个原因。

| 原始诊断收据 | SHA-256 |
| --- | --- |
| `evidence/n5-runtime-asan-original-diagnostic-01/summary.json` | `b256da94a1a4b9144158814c3eec6e2557260e6c105d48c2cc446034aa90a516` |
| `evidence/n5-runtime-asan-derived-diagnostic-01/summary.json` | `4056f23abf8c4c4e1b9f202148336213a0ae8f165c958e62689991b0e4519eeb` |
| `evidence/n5-persistent-runtime-original-hotspot-01/summary.json` | `ecce338234935cda67f77233d8d290be8781cf0d049dfd0d5d5edb4e9714e8e8` |

`/root` 在独立工作树准备了最小局部复用候选：按本次 fresh-read 的完整 envelope 字节、
config 字节与 SHA 复用已完整恢复的 `ModelRequest`／`FrozenRequestConfig`，生命周期
限于当前函数／事务。所有原 receipt/item/event/source/open projection、Actor／当前
policy／CAS、预算／容量和写后完整验证仍逐次执行；T4 事务前、业务事务、catch 新事务
不共享恢复结果。没有 Store 成员或全局缓存。两项新增 Store 回归分别覆盖同事务配置
字节变化后的派发回滚，以及同 Store 跨调用重新验证；runtime 测试和原期限未修改。

非作者源码复核结果为 **NO_BLOCKING_FINDING_IN_BOUNDED_SOURCE_REVIEW**，收据
`evidence/n5-event-validation-fix-nonauthor-review-01/review.json` SHA-256
`d1cb1875defa13f64bbfabbf04b8b7875949cb9ed5d91b1e4bb642cabeb62f8e`。审查绑定 base
cb81 的未提交 diff SHA-256
`36c47229a30018486c638f4851d654ed1ad685a9d959f1bb5438ac79af3574e7`，覆盖引用寿命、
T4 三窗口、原守卫与 SQL 夹具；所有原 require 表达式保留。审查者未编译或执行产品。
首次实际候选执行已经收尾，结果为 **NOT_FIXED**，不是性能修复或绿色交付。
输入是 cb81 基础上的三文件实验树；执行前 manifest 中的三份源 SHA 与上述有限源码
复核逐项相同，870 项源和 binary／build 身份前后保持一致，完整 CTest 证据已收齐。
configure **exit 0、7.609102 秒**；build **exit 0、718.627982 秒**；随后两项选定
suite 的 CTest step **exit 8、201.820219 秒**。这不是完整 94 项矩阵。

| 首次局部复用实验的实际 suite | 结果与实际执行范围 |
| --- | --- |
| persistent_event_store | 原 **120 秒**期限发生 **timeout，JUnit 120.367 秒**。前八个方法 PASS，第九个 T4 故障方法已开始后被中断；候选共 24 个方法，余下 **15 个未进入**，包括两项新增守卫。不能写成 24 个方法已执行或已失败 |
| persistent_event_runtime | **81.4172 秒，9 PASS / 4 FAIL**。仍是同四个方法名失败，等待行是 **283、326、374、569**；partial AMEND 已由原 369 行等待 tasks=2／outstanding=0 推进到 374 行 worker gate。不能写成同四个 callsite |

该 runner 顶层 `selected_original_deadline=180` 是复制的泛化 metadata。准确
`test-plan.json` 对 Store 明确为 **120 秒**、runtime 为 **180 秒**；Store timeout
依照原 120 秒期限报告，没有改成 180 秒，也没有放宽各 5 秒等待。

另外只抽取两项新增 Store 守卫测试，链接原 79654ca 的未改生产 archives，实际
**2 PASS / 0 FAIL、CTest exit 0，12.630241 秒**，身份前后相同且无遗留子进程。
这是原版本的正确守卫覆盖，分类为
`TWO_METHODS_CORRECT_GUARD_BASELINE_COVERAGE_NOT_GATE_NOT_HISTORICAL_DEFECT`。
它说明新测试在原有效守卫上也通过，不构成旧版本失败的红绿反例；只执行两个方法，
没有执行完整原 22 方法或候选 24 方法。首次候选完整 suite timeout 尚未到这两个方法，
不能将原 archive 的 2/2 计入候选验收。

后继候选配对使用与上述原库子例**同一份已编译测试 object**，链接首次局部复用实验的
冻结 **pre-SAX archives**，也实际 **2 PASS / 0 FAIL、CTest exit 0，11.458921 秒**。
source 与 binary 身份前后相同，完整收尾且无遗留进程；分类为
`PASSED_TWO_METHOD_CORRECT_GUARD_COVERAGE_PRE_SAX_NOT_GATE`。原库与 pre-SAX 的
2/2 各自只证明这两个守卫子例，不能抵销首次候选完整 Store 的 120 秒 timeout 或
runtime 9/4，也不作为完整 suite 绿色。四个原实验 ELF／archive 已另存于
`evidence/n5-event-validation-presax-binaries-01/`；其 `origin.json` 将保存的四个哈希
绑定到首次实验的 source snapshot 和 diff，后续 SAX 结果不会覆盖这份身份。

## 原 archive 的 owner 函数计时诊断

新计时诊断仍使用原 79654ca 生产 archives，只编译派生单测试翻译单元，以 GNU ld
`--wrap` 记录四个函数的 owner-thread CPU 与 wall 时间。原 suite／5 秒期限不变。
实际 suite 完整失败为 **9 PASS / 4 FAIL、CTest exit 8，85.620087 秒**；原源、原
archive／ELF 与派生输入／binary 身份前后相同，无遗留诊断进程。这是
`LINK_WRAPPED_OWNER_FUNCTION_METRICS_DIAGNOSTIC_NOT_GATE`，不替代原产品 gate。
summary 顶层 `NOT_COMPLETE` 是未更新的结果字段；`execution_completed=true`、
`complete=true`、exit 8 与完整 LastTest／JUnit 证明已经收尾失败，不是运行中。

首失败仍在 `before_gate_release`，两次 poll 合计 **5.476423709 秒**，
`gate_entered=0`、两个 dispatcher 回调已完成、coordinator `ready_results=2`。
同次首等待输出记录如下，各行均为 **inclusive** 时间：

| GNU wrap 观测函数 | 次数 | Inclusive wall 秒 | Inclusive owner CPU 秒 |
| --- | ---: | ---: | ---: |
| FrozenRequestConfig::restore | 18 | 2.867055714 | 2.866553064 |
| canonical_json | 314 | 1.797893900 | 1.798346655 |
| parse_json_strict | 266 | 1.305366153 | 1.305300309 |
| model_request_from_json | 14 | 0.343544825 | 0.343307817 |

这些调用存在嵌套，**各行不能相加**，也不能把和当成首等待总成本或独占占比。
GNU wrap 不覆盖同一翻译单元内已直接绑定的调用，因此次数不是完整调用清单；包装器
和计时本身也改变派生可执行文件。wall 与 owner CPU 接近，支持首等待中存在实际
CPU 工作这一判断，但没有提供 exclusive 成本拆分，也没有量得局部复用候选的速度
收益。前述首次局部复用实验的失败继续保留为 NOT_FIXED。

SAX 后继修正的两个文件已冻结：`src/runtime_types.cpp` 与
`tests/runtime_types_tests.cpp`。作者 `/root/concurrency_fix` 的 preparation 收据只记录
实现与静态检查；`/root/serialization_review` 随后完成有限非作者源码复核，结论为
**NO_BLOCKING_FINDING_WITHIN_BOUNDED_SOURCE_REVIEW**。复核不包括执行或性能实验，
也不批准整个集成候选。准确 diff SHA-256 为
`068d14b3e12c15cd9ee033a8318f61ba1e34269d173216ce424db8b8342f17c2`。

`/root` 的唯一增量 ASan **targeted02 已真实收尾失败，仍为 NOT_FIXED**。
先前 BUILDING / EXECUTION_PENDING 状态由已完成原件替换；保留原三 suite 的
**90／120／180 秒**期限及 runtime 原 **5 秒**等待。该次重用 targeted01 构建，
没有新 configure；compiler-version 检查 exit 0，增量 **build exit 0、89.598032 秒**，
三项选定 suite 的 **CTest step exit 8、204.618516 秒**。目录仍注册 94 项，实际只
选中并执行 3 项；其余 91 项不记作已执行。

| SAX targeted02 的实际 suite | 结果与实际执行范围 |
| --- | --- |
| runtime_types | **8 PASS / 0 FAIL，JUnit 6.86414 秒**；原 strict-parser／canonical byte／UTF-8 守卫通过，方法数来自完整 LastTest。JUnit 自身 system-out 被 CTest 的 1024-byte 输出阈值裁切，不能单靠其可见 PASS 行计数 |
| persistent_event_store | 原 **120 秒**期限发生 **Timeout，JUnit 120.398 秒**；前 8 个方法 PASS，第 9 个 T4 方法 START 后中断，余下 **15 个未进入**，两项新增守卫仍未到达 |
| persistent_event_runtime | **9 PASS / 4 FAIL，JUnit 77.3141 秒**；等待行 **283、326、374、569**，各自仍是原 **5 秒**等待；与首次局部复用实验同四行，不能改写为与原 79654ca 同四个 callsite |

本次 base 仍是 cb81 的五文件未提交实验；870 项 source 前后相同，binary／构建身份
前后相同，`execution_completed=true`、`complete=true`，无遗留 test PID。执行前
source manifest 与 targeted01 逐项比较，仅 SAX 的两文件不同，其 SHA 正好匹配有限
源码复核；Store 三文件及 runtime 测试未变，全部 94 个注册名称／期限未变。完整五文件
diff SHA-256 为 `8748ab5b64e24a16030c2f99b3bbf06295d3690a2f81028d18f0040b469ed6ed`，
与前述只含 SAX 两文件的复核 diff 分别绑定。

有限源码复核及 canonical-byte 守卫绿色不能推出性能修复，也不关闭原本机或远端
失败。另作的候选 owner 计时诊断现已实际收尾，仍 **NOT_FIXED**，完整原件位于
`evidence/n5-runtime-owner-metrics-candidate-01/`。它保持原计时诊断同一已编译
object（SHA-256 `d1de7e6407d0b95d85b93a3e6414396bdb3e5842f20f561433105c235012d687`），
只替换所链接的候选两份 production archive；归一化输出／两库路径后，link argv 相同。
**link exit 0、5.702574 秒；CTest exit 8、91.674344 秒；runtime 9 PASS / 4 FAIL**，
JUnit suite 为 91.5952 秒。原 180 秒 suite 期限及 5 秒等待保留，source、object、
archive 和派生 ELF 身份前后相同；完整 LastTest／JUnit 与完成标记齐全，无遗留 PID。

候选首失败仍在 `before_gate_release`。**3 次 poll 合计 11.910229518 秒**，其中
第 3 次为 **7.798900148 秒**；gate 尚未进入，models_created=4、已完成回调 2、
coordinator outstanding=2／ready_results=0。同次首等待的四项 **inclusive** 计时为：

| 候选库 GNU wrap 观测函数 | 次数 | Inclusive wall 秒 | Inclusive owner CPU 秒 |
| --- | ---: | ---: | ---: |
| FrozenRequestConfig::restore | 28 | 3.982058436 | 3.979243395 |
| canonical_json | 636 | 2.350196338 | 2.350595939 |
| parse_json_strict | 758 | 3.567524821 | 3.699574799 |
| model_request_from_json | 26 | 0.717229725 | 0.716525013 |

原 archive 首等待为 **2 次 poll、5.476423709 秒**，当时 models_created=2、
ready_results=2；候选为 3 次 poll，恢复／解析次数和推进阶段也不同。两者虽使用同一
诊断 object，却不是等工作量成本比较。上述 inclusive 值存在嵌套，**不得相加**；
GNU wrap 仍不覆盖同一 TU 的直接绑定调用。因此不宣称 CPU 回归、百分比加速或
修复成立，也不把派生诊断作为完整 gate。

`/root` 最新另限定事务内 config 传参修正：T2／helper，以及 T4 note 的
prepare／start 在同一事务内共享 config 参数，并新增第 **25** 个 Store 正确守卫
负例。`/root/owner_hotpath` 已完成三文件增量非作者源码复核，结论为
**NO_BLOCKING_FINDING_WITHIN_BOUNDED_INCREMENTAL_SOURCE_REVIEW**；相对 targeted02
只改变 Store 三文件，SAX 两文件、公共 Store header、ready 完整 guard 和
EventLinkReadContext 本体保持。原 fresh read、binding、CAS、授权和状态守卫在
该有限源码范围内保留；review 未执行编译、产品测试或性能验证，也不是正式批准。
完整五文件 diff SHA-256 为
`3b199aaf6ca7617c3ccc1cf7404e69d8451acd5d9030270fb5e266210093ae0a`，三文件增量为
`032b81f4f95e35eabbff15498d9a435f73037782bb50ffe582379a23168ed1eb`。

新增测试比较 inbox status／claim_id 与原 link generation 等事实；它**没有直接
快照 mutable event_actor_claims 的 status／current generation 整行**。因此可以说明
已检查的 inbox／link claim 身份／generation 事实保持，但不声称执行了该 mutable
行的直接断言或穷尽 CAS 覆盖。Store 现为 **25 方法**，新增 CTest 注册为 **0**，
产品目录仍 **94**；前两次实际 Store 尝试仍各按当时 24 方法登记。

另将三项正确守卫方法链接原 **79654ca archives**，使用原 headers／资源与 ASan -g，
实际 **3 PASS / 0 FAIL、CTest exit 0，31.695076 秒**（JUnit 31.6006 秒）。
compile **exit 0、58.297744 秒**，link **exit 0、5.086903 秒**，原 Store **120 秒**
期限不变。新增方法为 T4 坏配置／note start 的 forbidden RUNNING IOERR 哨兵负例；
原源／库、派生源／ELF 身份前后相同，完整执行证据收齐，无遗留 PID。这仍是
**THREE_METHODS_CORRECT_GUARD_BASELINE_COVERAGE_NOT_GATE_NOT_HISTORICAL_DEFECT**：
只证明正确守卫覆盖，不构成历史漏洞红例、完整 Store gate 或新候选执行结果。

`/root` 的唯一增量 ASan **targeted03 已实际收尾，仍 NOT_FIXED**，此前
BUILDING / EXECUTION_PENDING 已由完整结果替换。增量 **build exit 0、236.003135 秒**，
三 suite **CTest exit 8、202.122094 秒**；未重跑 configure，目录仍注册 94 项，
只执行以下 3 项。

| targeted03 的实际 suite | 结果与实际执行范围 |
| --- | --- |
| runtime_types | **8 PASS / 0 FAIL，JUnit 6.91487 秒**；方法数来自完整 LastTest |
| persistent_event_store | **Timeout，JUnit 120.278 秒**；原 120 秒期限，25 方法中前 8 PASS、第 9 T4 方法 START 后中断，余 **16 未进入**；不能写成 25 个已执行失败 |
| persistent_event_runtime | **9 PASS / 4 FAIL，JUnit 74.8955 秒**；原 5 秒等待行 **283、326、374、569**；partial AMEND 相对原 79654ca 已由 369 推进到 374，但没有等工作量速度测量或可量化加速 |

870 项 source 及 binary／构建身份前后相同，完整 LastTest／JUnit 与执行完成标记
齐全，残留为空。执行输入逐项匹配步骤复用非作者复核，94 个注册名称／期限保持。
本次与前两次分别使用当时的 25／24／24 方法分母，三轮性能候选均保持
**NOT_FIXED / VALIDATION_BLOCKED**，不作为完整矩阵绿色。

三方法候选配对随后也真实完成：保持原三方法基线的**同一已编译 object**，只换
冻结的步骤复用候选 archives，**link exit 0、6.601192 秒；CTest exit 0、27.426061 秒，
3 PASS / 0 FAIL**（JUnit 27.4006 秒）。原 120 秒期限与源／binary 身份前后保持，
证据完整，残留为空。保存库的 origin 绑定 targeted03 source／diff；这是有限正确
守卫配对，不是历史漏洞红例，不能抵销 targeted03 完整 Store timeout 或 runtime 9/4。

产品代码随后分别提交 SAX `752e37e06460c3aa8500e76eacd723be02d7af37`、事务内
Store `2637d415246e2b1b93c3710ea3f3e3f9fb922bc8`；最终单一输入已冻结为
**c2a5cea64aeea261b214cdfd9a968b53f491110a / cb9c1abce1ba2efa3f190933a7844555863d4fab**，
source **871**。冻结 manifest 相对 targeted03 只新增实施候选说明、更新 STATUS，
其余原路径 bytes／mode 保持，产品／测试代码相同；这不把此前定向失败变为最终
完整 gate。398 个旧受保护路径均未改变；初次路径解码检查脚本错误及更正通过均
保留在 preflight，未计作产品失败。冻结收据分类为
**FINAL_SINGLE_PRODUCT_INPUT_NOT_VALIDATION_APPROVAL**。

`/root/serialization_review` 随后完成冻结 diff／文档的有界非作者复核，结论为
**NO_BLOCKING_FINDING_WITHIN_FROZEN_DIFF_AND_DOCUMENT_SCOPE_VALIDATION_REMAINS_BLOCKED**。
五文件 exact patch 仍是 `3b199aaf...`，8／25／13 方法 registry、原 92 个名称／期限
及新增两 suite 保持；复核只读源码、文档与已完成证据，没有启动编译或新测试。

该复核相对 aa2c592 验证 root 原保护范围 **398 个 mode／type／blob 对象**，
另验证 `db/README.md`、`db/contract_store.py`、`db/schema.sql` 三项，故验证
**union 为 401**，覆盖 10 个原 SQL；新增项为 013 migration 与 17 份报告。
398 的范围包括 aa2 基线的 256 份历史报告；本草稿另核对的 270 份报告以原
79654ca 为基线，不能混计。初次不同范围的 390 项清点包含 full db、缺少 v0.5.0，
并错误假设 full db 有 20 项而非实测 12 项；生成断言错误按原字节保留，不当作
产品或工具自检红例。398 与 401 不混称一个分母，也不将这次有界工程复核写成
正式 PR 批准、完整 Store 复核或矩阵验收。

`/root` 已推送并建立 **Draft PR25**，base 是 PR22@79654ca，正文保持
VALIDATION_BLOCKED。此前GCC首轮前66项仅是owner进度，随后先交接0022 phase；
现在同一runner双mask均已完整94/94 PASS、exit0，父目录已收尾稳定。最终实际工具
也已通过123方法＋3场景；Clang18于19:07:52–20:14:21 UTC完成strict Release，
94/94 PASS、CTest与outer exit0。UBSan随后也于21:07:23 UTC完整94/94 PASS、
CTest／outer exit0。共享GCC ASan／LSan随后已完整收尾失败：54 PASS、38 TIMEOUT、
2 FAIL，CTest8／outer1，94项均已尝试，原始证据与前后身份完整；原Make已完整失败（step2／outer1、Store21／4，后74配方未运行）。compatibility02／03
均exit1／NOT_COMPLETE；后续A／B／C控制实验虽已完成且支持有界机制，仍按原收据
NOT_COMPLETE／acceptance OPEN，不能提升为兼容通过或关闭历史失败。
准确 head 的既有远端 CI 已由最终稳定收据核为七workflow／17job全部success，
不补填任何未收齐的本机 lane。报告作者未访问
活跃矩阵目录或产品 session。N5-2 的 014／412 仍未批准、design-only；Windows11
普通用户验收仍 NOT_RUN。原 79654ca 结果、三轮定向失败、各守卫子例和派生诊断
各自保留来源，不相互继承。

准备中的最终私有 harness 另有非作者**静态**审查，收据
`evidence/n5-final-harness-nonauthor-review-01/review.json`。审查者未执行工具自检、
编译或产品测试；以下 H-01–H-04 只适用于归档的原脚本字节，**不是已复现的产品
误判**，也不能倒推此前实际执行结论无效。CMake matrix v3 在该静态范围内无阻塞
发现；其余三入口在正式使用前需要纠正这些证据缺口，并取得独立后续审查：

| 静态发现 | 归档原 harness 的证据缺口与要求 |
| --- | --- |
| H-01 | integrated tools 仅声明 expected_methods，未强制校验顶层实际 unittest 方法数、skip／notrun；需要按最终顶层 summary 核验实际数量，区分外层方法与刻意跳过的内部夹具 |
| H-02 | Make-ASan、tools、compatibility 三入口仅执行前校验 HEAD；需要清理／结束时记录并核对 head-after，避免同 tree 的不同 commit 留下旧 SHA；CMake v3 已有结束检查 |
| H-03 | compatibility 接受的 --build 没有绑定到所声称的最终 source／HEAD；需要校验实际 producing Release 收据、source／config／build 及 archive／CLI 身份，兼容子例与其他 Release 产品失败分开登记 |
| H-04 | 新编译的 standalone new411 ELF 未纳入执行前后身份记录；成功编译后需在运行与清理两侧核对该 ELF，并单独保留 oldELF 身份 |

原静态收据的四项状态均为 **OPEN_AT_REVIEWED_INPUT**。归档 original 脚本与 review
保持独立身份；该时点尚无修正／后续复核收据。随后 `/root/concurrency_fix` 准备
修正，`/root/serialization_review` 完成有限静态后续复核，结论为
**NO_BLOCKING_FINDING_IN_STATIC_FIX_REVIEW_H01_TO_H04_ADDRESSED_EXECUTION_PENDING**。
三份修正入口分别补强实际顶层方法数与无修饰 OK、末尾 HEAD、生产 Release 构建
来源、派生 new411 ELF 前后身份。静态 preparation／review 当时没有执行自检，
其原状态保留；后来的真实执行另列如下。

| 私有 harness 的受控红绿 | 真实结果 |
| --- | --- |
| 原 tools-loop baseline | **8 方法，5 FAIL / 0 ERROR / 0 SKIP，exit 1，2.084242 秒**；五个失败是原工具对受控 skip／缺方法／缺 summary／带修饰 OK／缺入口的回归契约 |
| 修正 candidate | **26 方法，0 FAIL / 0 ERROR / 0 SKIP，exit 0，3.511112 秒**；ToolsLoop 8、Head 4、Provenance 10、DerivedIdentity 4 |

该次实际执行直接使用原／修正 loop AST 和 guard 定义，输入是无操作 unittest 输出
及数据夹具；不是另一份镜像判定实现。基线红例只属于私有证据工具回归，H-02–H-04
另由 candidate guard 契约覆盖，不能写成生产漏洞红绿或此前真实产品误判。总收据
确认 product／compiler／actual tool suite executions 均 **0**，source 与入口身份
不变；递归清单 **257 个文件 SHA 全匹配**，完成交接无残留。静态 review、准备
和真实执行各自绑定哈希；26 方法不计入 94 产品注册，也不填作最终实际工具 suite
结果或正式 PR 批准。

| 本次新增的实际原件 | SHA-256 |
| --- | --- |
| `evidence/n5-event-validation-asan-targeted-01/summary.json` | `b39e17297ec0c2c7db07421cfc02abe617f7fd0350a7fdf547a7e438bc44c7a6` |
| `evidence/n5-event-validation-asan-targeted-01/ctest-logs/LastTest.log` | `7ed81bfadb2868a4c152052200c6a3bdc20ceafced0654e45a3a7ad53760c097` |
| `evidence/n5-event-validation-report-preparation-01/store-guard-baseline-outcome.json` | `ade6af49e6d1daee09581e8645e56017e82d2fd48c3f86a492ffb6d71b2e2ca0` |
| `evidence/n5-store-guard-candidate-01/summary.json` | `868a593fd6d1047741ca835a83f11730aa9a3a942814518d58ace97461a59c0c` |
| `evidence/n5-event-validation-presax-binaries-01/origin.json` | `a0157e075719314120a4369943eb31653a8c52ecf1c322bccecad55763a5e901` |
| `evidence/n5-runtime-owner-metrics-diagnostic-01/summary.json` | `df5c02228adfd2fdc09ef89e3aecb2f6e4427729389b705e4e7885bfc82a003c` |
| `evidence/n5-runtime-owner-metrics-diagnostic-01/ctest-logs/LastTest.log` | `9e68ab3fbbff8384318a704237999e19256b77f8943b1657c475061d07267f62` |
| `evidence/n5-canonical-sax-nonauthor-review-01/review.json` | `416962bb42a61260db12b1dc0497371d68839daee7a000877fa6009f01761e0c` |
| `evidence/n5-canonical-sax-implementation-01/preparation.json` | `8d1db0c09ddb02bb28a338cc36939948676165a8b46c487342f6225a06779952` |
| `evidence/n5-event-validation-asan-targeted-02/summary.json` | `7120eb1ed0709301552b51564b8de4c8960e885459092c7d39e7dc770292ab2d` |
| `evidence/n5-event-validation-asan-targeted-02/ctest-logs/LastTest.log` | `deaf98502e02964f6b763ab7b96980e6a9c89a3a089def54b6cb57c91f750af4` |
| `evidence/n5-event-validation-asan-targeted-02/ctest.xml` | `0c3fe46c7b4d92994e2e14617d288c49867a016cdfa90324a2e2df0fb6fa4b36` |
| `evidence/n5-runtime-owner-metrics-candidate-01/summary.json` | `5af43f8f67fd7374e1f76db9eb237702c810516b6024d22fe91a145de383e127` |
| `evidence/n5-runtime-owner-metrics-candidate-01/ctest-logs/LastTest.log` | `d08fa197e08aca519ea5e9e6426ee716a6181575c2b02e960bd688899f32b466` |
| `evidence/n5-final-harness-nonauthor-review-01/review.json` | `0b5a52b1040e84c9307fc154d48ac53a5d91d366f4ac1d47b804217d3af9ebd2` |
| `evidence/n5-store-guard-baseline-02/summary.json` | `198d01ede8668caab41dd6ce7f7fb6d20de45e03006809d0c7167a25be73ad18` |
| `evidence/n5-store-guard-baseline-02/ctest-logs/LastTest.log` | `dc0ec8dd92a4384a9c06491183d964883b4a53e296d8e91a6eec960ffa4455f8` |
| `evidence/n5-event-validation-step-reuse-review-01/review.json` | `30d4fd2e250ef2257f7a501ebe5120a89b82705c800a1fa98da9f5afd001a702` |
| `evidence/n5-event-validation-asan-targeted-03/summary.json` | `69407b2e22f25bc367ad73b313db5ce0b02c7c3192a13426df84958318a0beba` |
| `evidence/n5-event-validation-asan-targeted-03/ctest-logs/LastTest.log` | `10c5092ade65fe4af7c007d76e5ef71136f1a2ef2fff8f1d1177e318ee6aed19` |
| `evidence/n5-final-harness-fix-review-01/review.json` | `89df2307bd11a530d0794f2777f11a861606407bb0d0a0e17b75b18dce402050` |
| `evidence/n5-final-harness-fix-01/preparation.json` | `62afdebb22dbd624393ef3b03c24a2550d7a2ee28f582890203f8fe445c7f870` |
| `evidence/n5-final-harness-fix-01/execution-01/summary.json` | `db20ee3dd739ee6e87801aa842313af5a1f70f0bf9a8bba3922e73a358fa1ff8` |
| `evidence/n5-store-guard-candidate-02/summary.json` | `c7f6ffece061135ef85fa4ea27bb718ca79545f031c04384ef15a8b8a8407090` |
| `evidence/n5-final-candidate-freeze-01/freeze.json` | `42e8ca4e0d0c5e37b12384975800d975dd5c4450ac3b66b50bc4395808f3dbfb` |
| `evidence/n5-final-source-review-01/review.json` | `d68de35a8787159f69745bec496d83ede127ca1a62ead7af37316096360ee6df` |
| `evidence/n5-final-source-review-01/immutable-object-verification.json` | `b61fc6e0578c3230e79d4618e84e5de0dfd23db7d28512df4ed63700adb7dcd2` |
| `evidence/n5-final-product-remote-01/initial-verification-02/summary.json` | `47c87138e10a9551b9ccd9331e234c47423ce98a0f704cc2490575aab9524838` |
| `evidence/n5-final-product-remote-01/initial-verification-02/SHA256SUMS.json` | `e18d5be8dc25f9aebdb3943e6b90a502005086e60b0a64c58adf3954ac29680c` |
| `evidence/n5-final-product-remote-01/intermediate-delivery-02/summary.json` | `d048a99730a3f2c4e8982c9445ee703bea54ab600df41567219a564a2e386409` |
| `evidence/n5-final-product-remote-01/intermediate-delivery-02/SHA256SUMS.json` | `789c4418ea6360466982bbdb78368c9b47a837b26f8639f59eaf66e7d8f58558` |
| `evidence/n5-final-product-remote-01/intermediate-delivery-02/make-plan.json` | `a89404d95c8bbb346891d4d6f139b94a84e52100cf68ba10de09e202a8a8334e` |
| `evidence/n5-final-product-remote-01/complete-observation.json` | `c6a58e59b5614e734b9c9a8b82ab282b1b238dd428df23405f647f12f5772fec` |
| `evidence/n5-final-product-remote-01/FROZEN-SHA256SUMS.json` | `c441a67c45eef02cee8af37686661cd9024ef5a9e47a5699ccb997974bc228f0` |
| `evidence/n5-final-product-remote-01/final-asan-verification-02/summary.json` | `0c0e059cd1448f8d930cdd73dda8aed965167f9e4ad18c105298bf7627e9a6b9` |
| `evidence/n5-final-product-remote-01/final-delivery-verification-02/summary.json` | `76bc1c0f5557da3230900cdc47b5a47a3ec4e09e8c3834f6b2b27e849b4b3d17` |
| `evidence/n5-final-product-remote-01/final-verifier-execution-02/summary.json` | `91e2824738b1b19ae856ec7dac36af76b815a79c6ed336e343b546bfd7b2098d` |
| `evidence/n5-final-product-remote-01/method-visibility-01/summary.json` | `20a5bbeafd4f484fa79467ea54cac6798a99c08168d30c115805c1c9443c2dc8` |
| `evidence/gcc-release-20261102T171141Z/umask-0022/summary.json` | `9acdd6e838e4ceef9497de07ea3c52431539b3c345941131709445f768cb8c9d` |
| `evidence/gcc-release-20261102T171141Z/umask-0022/ctest-logs/LastTest.log` | `d8f576e59c8f8a7046318e33a4d59ed2187d13642141d81cd0fdf86c8f02c645` |
| `evidence/n5-final-host-observations-01/after-release-0022-before-0002.json` | `88094616b85c6313ceb73846813984d862c733d6f8767bc536a26c67f5e1e04c` |
| `evidence/gcc-release-20261102T171141Z/summary.json` | `a7a2ba404a1458fa1792776336e1362289f1ad0546274d16768cb68b4b6b9766` |
| `evidence/gcc-release-20261102T171141Z/SHA256SUMS.json` | `ff7c823c994daf41449146ebb74d0f28eeefd191ba3a27aa5c872af233e9fe16` |
| `evidence/gcc-release-20261102T171141Z/umask-0002/summary.json` | `cb9d56a74fe61d28efab35fcffbd59842c9e87b1adf40688480ab812796d4024` |
| `evidence/gcc-release-20261102T171141Z/umask-0002/ctest-logs/LastTest.log` | `50eba5cd511dcfcb46d781df2eaf1d0311f6219252297a744072c3f946dfa86e` |
| `evidence/n5-integrated-tools-02/summary.json` | `7aa004756eef6bc91e46e77d5c81e09bc1e7f98e8c55c3ea6aeba5837b80b215` |
| `evidence/n5-integrated-tools-02/SHA256SUMS.json` | `f9cb0af8da7631f8f7326590589f9f9e1829256370b08498f5c5117cffb40375` |
| `evidence/n5-final-capability-and-compatibility-02/summary.json` | `eb0e94f332a0db025b4df4e82d9a5a0e8cea6bfc85d44e4a77792bcb85130075` |
| `evidence/n5-final-capability-and-compatibility-02/SHA256SUMS.json` | `8832b360b994a2c78e1998a318b680afe349a82aaa831884b23dc6c6f1070342` |
| `evidence/clang-release-20261102T190751Z/summary.json` | `120c9998ce73ddf1d0a121250b95d4a8a2dfd84736c9ab966ab39b65d3ff0f03` |
| `evidence/clang-release-20261102T190751Z/SHA256SUMS.json` | `ce48b700239cab37875b428fd51d4c25e298004deccce1af33717c31ff2ca189` |
| `evidence/n5-final-capability-and-compatibility-03/summary.json` | `31f395deaa4f23b3b01452a7e1aeb6a8a707976eea1054ff0a9c728efd23565d` |
| `evidence/n5-final-capability-and-compatibility-03/SHA256SUMS.json` | `eb84e9948414fd5581ad931a94a2908851b547b7a657f0efee754e45463f6965` |
| `evidence/n5-final-compatibility-observer-fix-03/execution-02/receipt.json` | `13a53c39bb7335d2c8662fdbe263a7808ffdc4444b2f9e7ab027a13320b9084d` |
| `evidence/n5-compatibility-sidecar-review-01/review.json` | `afe91b6ef9dd83877b5a53df5ca03795598b94877d95abaa290a733ce2c9a1dd` |
| `evidence/n5-compatibility03-actual-review-01/facts.json` | `35a544a334c15a408d0b79ef34b008a104572830ebf8b93a910e1ddff9e3ca40` |
| `evidence/gcc-ubsan-20261102T201745Z/summary.json` | `f0bc12cc60a07ee59d09003c1a81f1d6f828503a44f83fba213a741283f58b75` |
| `evidence/gcc-ubsan-20261102T201745Z/SHA256SUMS.json` | `3a24790a9f3486d0bb6af93f0e8b667c4d4e6420e8162436f706f00bd94667d7` |
| `evidence/n5-compatibility-sidecar-control-mock-01/summary.json` | `422e6fa8b84eb77d00a8b275c20d9431344985a20b2586ad99dd44049b05ff5f` |
| `evidence/n5-compatibility-sidecar-control-review-01/initial-review.json` | `f10e36b687ff7b1407e6a0226f37ff87075d11c5fff7a71e0ea2ce0f1e9660fe` |
| `evidence/n5-compatibility-sidecar-control-review-01/followup-review.json` | `f76f2fb7968f3f6203a05c14a4e148bd9d75ff6c85c9f5f771a65a2ecf056b93` |
| `evidence/n5-compatibility-sidecar-control-execution-01/summary.json` | `011f3065f4aa7ec5fef55d96d076a7233c6e8a75c864a94019ec3908f3a9df80` |
| `evidence/n5-compatibility-sidecar-control-execution-01/SHA256SUMS.json` | `9a3aaa888ded754f07351f21cd82457f1fec0b658b78796256010eacd2185d1d` |
| `evidence/n5-compatibility-sidecar-control-execution-review-01/review.json` | `e741f145598651c24c024e7488f73f34701a9089c6b34563a4133128486a6446` |

最终 head 的远端有界快照另于 **17:24:37 UTC** 完成核验：push 原 source artifact
的 **871 文件内容及 Git regular/executable mode** 对应 c2a5cea／cb9c1a，tar 实际
权限 `0o664 × 871` 单独保留。PR25 的 merge checkout 为
`7c0cb09bc5ccc3e672a40ecbe16f2bd1c305b5b9`，父为原 79654ca 与 c2a5cea，tree
与 head 相同；不能仅凭 PR merge metadata 推断 checkout，此处有原 job log 绑定。

| 最终输入 Windows Server native 事件 | 实际有界结果 |
| --- | --- |
| push run41489174066／job125427341732／attempt1 | **5 suite、34 方法 PASS**；完整 LastTest、JUnit 5 suite PASS，871 source／4 binary 前后相同 |
| PR run41489188259／job125427389094／attempt1 | 同样 **5 suite、34 方法 PASS**，输入绑定上述 merge checkout／同 tree；完整 LastTest 与身份收据 |

两份 `native_cli` 的 JUnit system-out 原本即按 1024 字节裁切，只可见 **7/14 方法**；
34 方法的完整证据来自原 LastTest，不能宣称 JUnit 方法输出完整，也没有重建日志。
初版核验器错误要求 JUnit 全文而记录两项 incomplete；原初版收据保留，v2 如实
记录裁切并核对 LastTest，未更改产品阈值或原 artifact，不计作产品／workflow 红例。

真实主机为 Server2022 build20348、elevated token、NTFS，MSVC19.44.35229.0。
同一 job 的镜像头 `20261025.346.1` 与进程环境 `20261026.347.1` 均已绑定来源，
但比较各 **exit1／UNRESOLVED**，没有选择一个覆盖另一个。Win11 与普通用户验收
仍 NOT_RUN。六路／两事件完整 CI 在该收据交接时仍 in_progress；这份 Windows
组件／源码快照不能填成整体 REMOTE_CI_COMPLETE，也不替代本机最终矩阵。

后续稳定远端快照于 **17:37:53 UTC** 记录原 head c2a5cea、原 base 79654ca 与
PR25，补齐以下八个已完成 Linux job。push runtime run 为 **41489173778**，PR
runtime run 为 **41489187735**，均 attempt1；每项均有完整94行 CTest结果、准确
注册名称及 `100% tests passed, 0 tests failed out of 94` 结束摘要，job conclusion
为 success。strict 的原日志分别记录 `umask 0022` 与 `umask 0002`。

| 最终 head Linux 配置 | push job／实际结果 | PR job／实际结果 |
| --- | --- | --- |
| strict，umask0022 | 125427339994，**94/94 PASS** | 125427389740，**94/94 PASS** |
| strict，umask0002 | 125427340022，**94/94 PASS** | 125427389474，**94/94 PASS** |
| Clang18 | 125427339568，**94/94 PASS** | 125427389580，**94/94 PASS** |
| UBSan | 125427340134，**94/94 PASS** | 125427389638，**94/94 PASS** |

八项 checkout 均由完成 job 的原日志核验：push 为 c2a5cea，PR 为上述 7c0cb09
merge checkout、父79654ca／c2a5cea，tree 均为 cb9c1a；没有借用父 SHA 或先前
run 的成功。原 source／Windows 有界收据继续保留。该17:37快照当时的 ASan push
job125427340103、PR job125427389747，以及 Make-ASan push job125427340052、PR
job125427388723 为 **in_progress／conclusion null**，当时尚无完成 checkout／结果核验。
两个 runtime workflow 当时仍 in_progress，该快照的 REMOTE_CI_COMPLETE=false 保留为历史。
`make-plan.json` 只登记原94项 Make 执行计划，不是 Make 已完成收据。该阶段远端绿色
不关闭历史 sanitizer／live_workflow 根因，不填入本机最终矩阵，也不改变三轮定向
实验 NOT_FIXED、Win11 NOT_RUN、镜像来源 UNRESOLVED 或正式审批／合并状态。

**最终18:07:25 UTC稳定交接已收齐上述四个 job**，此处才把准确最终 head 的
`REMOTE_CI_COMPLETE` 更新为 **true**。七个既有 workflow run、17 个 job 全部
attempt1、completed／success：push／PR 各有一个六配置 runtime workflow，另有
push source snapshot，以及两事件各自的 Windows native、A4 fairness workflow。
完整17项 checkout 均绑定 c2a5cea 或上述7c0cb09 merge checkout；PR merge父仍为
79654ca／c2a5cea，同 tree cb9c1a。八个普通 CTest job 各94/94的先前原件保留，
最后收尾的两种 ASan 结果如下。

| 最终输入 sanitizer 配置 | push job／完成结果 | PR job／完成结果 |
| --- | --- | --- |
| CMake ASan／LSan | 125427340103，**94/94 CTest PASS、原步骤exit0** | 125427389747，**94/94 CTest PASS、原步骤exit0** |
| 原 Make ASan | 125427340052，**原94项命令按序全部完成，job success** | 125427388723，**原94项命令按序全部完成，job success** |

两个 CMake ASan 公共包各核 **871项源输入**与提交字节相同、前后相同；
**63项 binary及构建元数据**的前后身份也相同，不能把63统称为ELF数量。
受影响 suite 的 `runtime_types` **8**、Store **25**、runtime **13** 方法名称与
PASS 均可见且齐全。身份哈希是远端运行的观测，未下载或在本机执行这些可执行程序。
公共包中 LastTest 与诊断文本是既有发布器生成的 **public projection**；
**不称为 private raw／原始私有日志**，也没有取得或复核私有 ASan 诊断原字节。

全部94项的 suite 结果与现有可用方法证据均通过，方法名称的实际可见性单独计数：

| 每一事件的日志类型 | 可见名称 | 仅计数的通过方法 | 没有unittest方法注册的直接脚本 |
| --- | --- | --- | --- |
| CMake ASan public projection | 80 suite／**1174 named methods** | 11 suite／**72**；其中9 suite／59因固定发布器省略名称，2 suite／13因原quiet unittest | 3 |
| 原 Make job log | 89 suite／**1233 named methods** | 2 suite／**13**，原quiet unittest | 3 |

`complete_individual_method_name_observation=false` 保持；count-only 与直接脚本
不被伪造为逐方法名称，也不据旧核验器的 generic incomplete 标志倒推产品失败。
完整性结论限于实际可用的名称、计数、suite结束与原命令顺序；不能宣称所有方法
都有逐名日志。初次核验器的 JUnit 全文假设，以及初版 ASan 核验器对公开归档的
嵌套路径假设失败，均保留原收据：后者在 final-verifier-execution-01 的 asan 步骤
实际exit1、delivery步骤exit0。后续只修正只读解释，两个最终核验辅助步骤均exit0；
未改产品、workflow、阈值或原artifact。这些是**工具准备／格式假设错误**，不是CI失败。

冻结 manifest 的 **1068文件**已重新核对字节数／SHA-256，收据记录190次GET均成功，
无API采集错误、rerun或远端写操作；观察进程已exit0且无其残留辅助进程。旧PR22、
PR23、PR24与main等已观测ref保持，PR25 head／base未变，仍 **Draft／open／未merged**；
mergeable clean不等于正式批准。Windows结论与初始有界原件相同：Server2022／
elevated／NTFS，两事件各5套34方法由完整LastTest支持；JUnit native_cli原截断7/14、
镜像双来源UNRESOLVED、Win11／普通用户NOT_RUN均保留。当前远端绿色不关闭本机
与历史失败，不改变N5-2未采纳、正式审核／合并false或整份报告VALIDATION_BLOCKED。

## 原冻结输入的完整 Release 与后继矩阵

本轮正式产品编译和测试由 `/root` 单独安排，未并行启动第二组编译或 sanitizer；
主机其他负载及已登记的晚段索引仍存在，不声称全 CPU／IO 隔离。下表只接受
完整原始日志、实际终止、准确 source / ELF、目录／期限、前后身份和结果收据。
在收据交接前，空白结果按 **OPEN** 保留，不使用旧 92/93 项或定向 2 项填充。
原冻结 79654ca 的 Release 已完整结束；它与后继候选的适用矩阵分开登记。

| 最终本机 lane | 需要固定的输入和完整目录 | 当前记录 |
| --- | --- | --- |
| GCC Release umask 0022 | 79654ca / 2243d75；94 项；完整 CTest/JUnit/LastTest 与源／ELF身份 | **94 PASS / 0 FAIL / 0 timeout / 0 skip / 0 notrun；CTest exit 0，860.002071 秒** |
| GCC Release umask 0002 | 79654ca / 2243d75；同一构建；94 项；独立原证据 | **94 PASS / 0 FAIL / 0 timeout / 0 skip / 0 notrun；CTest exit 0，860.601480 秒** |

正式配置／构建开始于 2026-11-02 14:21:57 UTC、完整尝试结束于 15:01:20 UTC。
configure exit 0、3.170451 秒；build exit 0、629.568616 秒；普通用户 UID/GID 1000，
GCC13 Release，source-relative-build。0022 的实际时域为 14:32:32–14:46:54 UTC，
0002 为 14:46:54–15:01:18 UTC。两轮各 867 项 source 与 63 项 binary／构建身份前后
byte-identical，两种 umask 之间也相同；源清单还逐项核对冻结提交的原 blob bytes。
两份完整 JUnit 各有 94 个唯一 case，均无 failure/error/skipped。实际注册名称和期限
与 94 项审核目录精确相等，原 92 项名称／期限不变，只新增 Store/runtime 两项。
CLI ELF SHA-256 `2a673b74345e7ab82f69275050d8656d05d610c53a02b5bd37f4b743faacfc37`。
原件目录 `evidence/gcc-release-20261102T142157Z/umask-0022/`，summary SHA-256
`43d5a4d382b277a7f40669ed8c8dc5989c6873ce7af0f2159b85f69ecf13ea70`，
LastTest 262823 bytes、SHA-256 `24df34302bdddf22f54d3b81855b723aba500e77902666229fb848a5617f734f`，
JUnit 103447 bytes、SHA-256 `8524cb87f6990c4cac3ac1c1eb185d3a477355e656f3ec66d339e5cf71b2a481`。
0002 summary SHA-256 `1c917e01abbd5ff3b6d53738df518b2b65947fbcdde67033e64fc2c3c68514ce`，
LastTest 262823 bytes、SHA-256 `3e3b4ccf7a00d17fdf9e543466d90cfad6ea155a102b77d93c20b5aad931d002`，
JUnit 103448 bytes、SHA-256 `76900917e084c2f175fddfe60460abe27acf8bdcf9329da9632e1dc527b24bcd`。
完整 root summary SHA-256 `70d10ffae51a6df6f585e5df266b6e5f5bde3b9c31a0afd6f6b2a164f80f7035`。
原失败、旧单因素诊断、这次完整 Release 与后继不同 SHA 均保留各自输入和结果；
两种 umask 完成不关闭其他编译配置、历史失败或后继新 SHA 的验收。

| 后继统一候选的适用 lane | 输入与执行前置 | 当前记录 |
| --- | --- | --- |
| GCC Release，umask0022 | 冻结 c2a5cea / cb9c1a；完整94项，871源／63 binary及构建元数据前后同 | **94 PASS / 0 FAIL / 0 timeout / 0 skip / 0 notrun；CTest exit0，2050.80688748206秒**，首个正式完成phase |
| GCC Release，umask0002 | 同一runner／冻结输入；完整94项，871源／63 binary及构建元数据前后及跨phase相同 | **94 PASS / 0 FAIL / 0 timeout / 0 skip / 0 notrun；CTest exit0，2058.519531132886秒**；双mask Release完成 |
| GCC ASan/LSan | 同一冻结 c2a5cea；原共享runner、原94名称／期限，871源／63 binary及构建元数据前后同 | **54 PASS／2 FAIL／38 TIMEOUT／0 SKIP／0 NOTRUN；CTest exit8、outer exit1**，完整失败收尾，非进行中 |
| UBSan | 同一冻结 c2a5cea；umask0002，871源／63 binary及构建元数据前后同，完整LastTest／JUnit | **94 PASS / 0 FAIL / 0 timeout / 0 skip / 0 notrun；CTest／outer exit0**；CTest打印2158.63秒 |
| Clang18 strict | 同一冻结 c2a5cea；Release、umask0002，871源／63 binary及构建元数据前后同，完整LastTest／JUnit | **94 PASS / 0 FAIL / 0 timeout / 0 skip / 0 notrun；CTest／outer exit0**；CTest步骤1997.0118731760886秒 |
| 原 Make ASan | 同一冻结 c2a5cea；原未修改 Make 可执行程序序列，独立于 CTest／JUnit | **FAILED；原step exit2／outer exit1**，20配方已进入（前19成功、当前1失败），后74未进入；末suite25方法21PASS／4FAIL，非CTest |
| 最终实际工具 suite | 同一冻结 c2a5cea；七个入口步骤均exit0，源／head前后同；独立于产品注册 | **PASSED**；六unittest入口共123方法、另native三个场景；此前harness26方法受控红绿独立保留 |
| 最终standalone capability／compatibility02 | 准确c2a5cea Release产物与原old410；独立于94项产品注册及工具123方法 | **exit1／NOT_COMPLETE／OPEN**；readonly身份同，takeover主DB／lock同但WAL／SHM集合不同，原因尚未闭合 |
| 最终standalone capability／compatibility03 | 相同冻结产品；显式关闭observer，严格全raw集合，另有独立41方法纯mock准备 | **exit1／NOT_COMPLETE／OPEN**；readonly预期exit2后新WAL／SHM触发raw拒绝，after observer未connect，无after semantic，takeover未执行 |
| 后续A／B／C sidecar控制 | DIAGNOSTIC_NOT_GATE；另有独立9方法纯mock，三个独立fresh fixture，A内先RO后RW | **experiment_completed=true，但exit1／NOT_COMPLETE／compatibility OPEN**；有界schema／typed rows同，A／B物理sidecar集合变化保留，非全兼容通过 |

最终输入首个0022 phase的原CTest步骤为17:50:08–18:24:19 UTC，实际exit0、
2050.80688748206秒；CTest自身打印总时长 **2050.77秒**，phase于18:24:23 UTC收尾。
原LastTest **264782 bytes**，SHA-256
`d8f576e59c8f8a7046318e33a4d59ed2187d13642141d81cd0fdf86c8f02c645`；完整JUnit
**103564 bytes**，SHA-256 `d753ca58e83e9fe5ca75a1288e4ef29a14bacd10bd11d9c883d36a1fb899bee6`，
有94个唯一case、无failure／error／skipped。LastTest有完整94段并全部Test Passed；
受影响方法的逐名证据来自这份原LastTest：types **8/0**（0.38秒）、Store
**25/0**（67.10秒）、runtime **13/0**（14.23秒），START／PASS名称逐项齐全。
源before／after清单均为871项，且逐项与冻结c2a5cea源清单相同；63项binary及构建
元数据before／after相同，63不是ELF数量。首个phase交接当时仅索引稳定
`umask-0022/` 的11份文件，未读取当时仍在写的父summary和0002；这些11份原件
全部保留。现在父目录已完整收尾，新增其余稳定父级与0002文件，完整目录共55份。

完整正式GCC Release尝试始于17:11:42 UTC，至18:58:58 UTC结束，普通用户
UID／GID1000，GCC13、source-relative-build。configure实际exit0、
**8.101216566981748秒**；build实际exit0、**2288.200055039022秒**。94项注册名称
及期限逐项等于expected-tests审核目录，原92项不变，仅保留已批准的Store／runtime
两项。父summary head为准确c2a5cea、head_unchanged=true；父级及两个phase的
source before／after六份清单均等于冻结871项，binary及构建元数据六份清单也全部相同。
`resources-before/after`是运行前后主机观测，允许随时间变化，不冒充不可变身份清单。

0002的CTest步骤为18:24:27–18:58:46 UTC，实际exit0、**2058.519531132886秒**，
CTest打印总时长 **2058.49秒**；phase于18:58:51 UTC收尾。原LastTest **264784 bytes**、
SHA-256 `50eba5cd511dcfcb46d781df2eaf1d0311f6219252297a744072c3f946dfa86e`，完整JUnit
**103559 bytes**、SHA-256 `006b7b1dc119ef2fd8fb065c57a632ad682fa66eebd3856c16e2aa4b5dfe9456`，
同样94个唯一case、无failure／error／skipped，LastTest完整94段全部通过。
其中types **8/0**（0.41秒）、Store **25/0**（64.28秒）、runtime **13/0**（15.65秒），
逐名START／PASS与0022相同。两phase现在均PASS；其完成只关闭本次正式GCC Release
双mask范围，不代替兼容性、历史根因或正式批准。后续Clang18 strict Release已独立
完成；UBSan随后也已完整通过。共享GCC ASan／LSan已完整失败，详见下节；原Make
ASan也已实际失败收尾，step2／outer1、Store21／4及后74配方未运行单列，不以其他lane补填。

最终Clang18 strict Release在19:07:52–20:14:21 UTC完成，umask0002、UID／GID1000、
source-relative-build。configure exit0、12.59073847788386秒；build exit0、
1957.995310293045秒；CTest步骤19:40:52–20:14:09 UTC，exit0、
**1997.0118731760886秒**，CTest打印总时长1996.98秒，outer exit0由root交接。
原LastTest为**265152 bytes**、SHA-256
`2cc435f32464171eaab0a1865b2b9c04b55bd5be072ea850f258acb8205e21fd`，
94个唯一完整段全部Test Passed；JUnit为**103550 bytes**、SHA-256
`1e830be91e077cf279a84adc04c2260cba2ac8b4b5d0bde8cc5dbe9478970728`，94个唯一case
无failure／error／skipped。types **8/0**（0.41秒）、Store **25/0**（64.12秒）、
runtime **13/0**（12.88秒）；billing_baseline_e2e suite也PASS（261.41秒）。
父与phase的四份源map均等于准确冻结871项，四份binary／构建元数据map均同为63项；
63不是ELF数量。94名称／期限与原审查catalogue相符，原92不变。44份稳定文件及43成员
manifest全部重哈希；Clang Release通过不关闭历史Clang ASan／live_workflow原因。

最终GCC UBSan在20:17:46–21:07:23 UTC完成，umask0002、UID／GID1000，
configure exit0、5.310614172834903秒；build exit0、747.6238551069982秒。
CTest步骤20:30:43–21:06:42 UTC，exit0、**2158.6595719149336秒**，打印总时长
**2158.63秒**，outer exit0由root交接。LastTest为**264417 bytes**、SHA-256
`06ca4f5c4e0b04890d1202daa18a9d17f37ba7817976780517e1e06ac514150b`；
JUnit为**103562 bytes**、SHA-256
`355f9accfec2c97b6918a2b4d62e32ea7a5ebd852cd6309389b963a2f082f84c`。
两者分别完整94个唯一段／case，全部通过、无failure／error／skip。父与phase四份
871源map同冻结输入，四份63项binary／构建元数据map前后同；94名称／期限与原catalogue
相符。Store **25/0**（58.18秒）、runtime **13/0**（14.32秒）；TLS12.74秒、
live_workflow130.53秒、full billing321.67秒、structured CLI27.42秒均PASS。
44份稳定文件／43成员manifest全部核验；这些UBSan结果不关闭旧sanitizer、Clang或
live_workflow原因，也不补成剩余ASan／Make的实际结果。

最终共享GCC13.3 Debug `-g` ASan／LSan在**21:09:20–23:45:44 UTC**完整结束，
普通用户UID／GID1000，build umask0022、test umask0002，
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`。configure exit0、2.723778秒，
build exit0、1334.695594秒；CTest于21:32:04–23:45:14 UTC执行，
**7990.191026秒、exit8**，outer为**exit1**（实际session29061／chunkcca9d4）。
原runner `status=NOT_COMPLETE` 保留，但执行与证据均完整收尾，**不是尚未运行完**。

94个唯一JUnit case和原LastTest共同登记为**54 PASS、38 TIMEOUT、2 FAIL、
0 SKIP、0 NOTRUN**。JUnit顶层failures40包含38个超时与2个完整失败suite，
不能写成40个普通断言失败或94项通过。所有原名称／期限与审核catalogue相等；
871项source before＝after＝冻结manifest，63项binary／构建元数据before＝after，
63不是ELF个数。outer after收据确认准确head c2a5cea、tracked工作区干净，
原共享ASan本轮所属测试残留进程为空；该收尾截点原Make尚未执行。

| 本轮共享ASan关键suite | 完整原件中的实际证据与边界 |
| --- | --- |
| persistent_event_store | 原120秒期限超时，JUnit120.315秒；25方法中前8 PASS、第9 START后中断，余16未进入 |
| persistent_event_runtime | 完整13方法，**10 PASS／3 FAIL**，JUnit72.4987秒；失败为two requests/two nodes、feedback after capture、empty S，仍是原5秒等待。不能沿用历史9/4或称四个同callsite |
| live_model_store_tls | 完整10方法，**9 PASS／1 FAIL**，JUnit93.1275秒；large escaped output原16秒断言实际17.14478492899798秒。该失败不是子进程或suite超时；容量方法本轮未报告失败，不能据此关闭旧容量原因 |
| live_workflow | 原300秒suite超时，JUnit300.069秒；可见6个ok与两份即时flush的完整失败trace，无完整方法总结 |
| billing_baseline_e2e | 原900秒suite超时，JUnit900.188秒；四个即时FAIL marker来自test01 multi/single、test02 single、test03 single。不能机械当作四个独立方法终态或补齐十样本；未执行项仍保留原分母 |

本轮live_workflow的首份trace位于原test_cross_task_host_submission第262行：
实际stop_reason为`needs_accounting`，预期`quiescent`；记录中一个INTERRUPTED attempt
仍保留五维unknown，未补零。另一份trace位于model_exhaustion第324行，
预期blocked包含`manager_limit`的any为false。billing四个marker都经过原
finish_case第356行，观察到`deadline`断言。即时输出在suite后被中止时实际保存了
原因，属于**新准确输入的可观测性证据**；不回填历史缺失trace，不据现象宣布根因、
关闭历史问题或削弱UNKNOWN／预算／唯一发送守卫。完整38个timeout和两个失败名
及各自实测时间写入机器索引，不把suite结果与内部方法数相加。

非作者`/root/validation_receipt_audit`随后只读核对**94项检查／34份既有原件**，
确认该完整失败终态、源／binary身份、原期限及方法范围；不是94个新产品测试。
HTTPS本轮42方法完整通过，TLS容量方法本轮ok，但两者都不关闭旧输入根因。
该审阅不执行产品、不重跑门禁，也不将完整失败改绿。其收据为
`evidence/n5-final-gcc-asan-nonauthor-audit-01/review.json`，SHA-256
`77e161ded044cd0409dbc6c8b30c2345962f2fbb04b42ad4c02b136b145cbeb6`。

原LastTest为273112 bytes，SHA-256
`0071e96f3baab3974e846ca9d9dffa96981c380ac98f238d314e8dc0ba5f1699`；
JUnit为160228 bytes，SHA-256
`69b6e66442417fe4dbbff0beeb18359faf6c333d5a188a45dbf269c94491d48f`；
原summary的SHA-256为
`9ed54d7ff583c42ffa35b32faa9659c051d930c0d193ff9bdcb0baca3c51220e`。
私有原件位于final-candidate的build-gcc-asan-20261102T210920Z-778d61d7／
private-evidence，命令、ctest-evidence、全部原日志及源／二进制前后身份均保留；
外层启动／终态／资源观察在evidence/n5-final-gcc-asan-launch-01。

正式本轮没有并行启动另一组sanitizer或编译，但**不声称完全独占CPU／IO**：
23:37的一次codebase-memory fast源码索引与尾部89→90项时域重叠，首次工具yield
约31.1秒，缺乏当时精确CPU／IO分解。索引之前已有35个timeout与两个失败，
不能归因这个后期索引；也不能推断尾部项完全未受影响。原始条件、前后资源观察
和未变期限全部保留，资源／编译器／性能根因继续OPEN。

根据用户本轮明确选择：保留本机私有原件、hosted托管继续BLOCKED；先整改N5-1，
之后再由主协调者审阅PR21，未采纳D1–D6或014／412；无获准Win11机器，
Windows11普通用户NTFS目标继续NOT_RUN。选择原记录在
`evidence/n5-user-decisions-20261102-01.json`，不授予正式批准、合并或付费调用。
`/root`随后仅执行一次已冻结的派生诊断02，结果现已完整交接，详见下节。
它只检验恢复中的重复规范编码成本，**DIAGNOSTIC_NOT_GATE**，不能覆盖本轮失败
或填作原Make-ASan通过。原Make在该诊断结束后由owner启动session15445，现已
完整失败收尾，详见下节。原“未运行／已启动待终态”消息保留在机器索引历史时域，
不再作当前状态；报告作者在Make活动期间没有读取其活动目录，也没有启动第二轮产品测试。

### 重复恢复／编码的有界派生诊断02：已结束，非修复门禁

诊断02准确plan为
`a7d8b74341ca6982d324dd8b1ed80005fccf8d8d35769257c2b24419544785aa`，
仍以原c2a5cea／cb9c1a、GCC13.3 Debug `-g` ASan／LSan的生产archives、
头文件和资源为输入。只重编一个派生测试TU，在main的原`test::run`外加入两个
链接wrapper和owner计量；反向去除include与main wrapper后恢复原TU字节。
原13方法、顺序、业务断言、5秒等待、umask0002及180秒suite期限均保留，
生产94注册没有增加或更改。派生目录的一个DIAGNOSTIC注册单列，不能算第95项。

**00:10:31–00:13:45 UTC（2026-11-03）**，编译exit0、73.098467秒，
链接exit0、7.790763秒；实际CTest于00:11:53–00:13:12执行，
**79.433330秒、exit8；外层exit1**（session75451／chunk86ab09）。
原summary `status=NOT_COMPLETE` 和 `test_status=FAILED_OR_INTERRUPTED`
保持原字面值；实际外层终止、完整LastTest／JUnit及13方法终态说明本次为
**完整失败且未中断**，不把字面OR推断成发生了中断。

原13方法实际**10 PASS／3 FAIL／0 NOTRUN**。失败仍是two requests/two nodes、
feedback after capture、empty S三方法，与本轮原c2正式runtime同名；表达式仍为
原5秒等待内`checked < deadline`，不是编译失败、外层超时或改成预期异常。
原套件失败继续保留，新的派生计时不能回填历史缺失trace或证明旧Clang原因。

source871、原binary／构建元数据63、prepared code7及派生ELF＋object两身份
前后保持；Git head/tree保持，identity_errors为空，无派生残留进程。
完整LastTest为606024 bytes、SHA-256
`0abe024828b850306ba3146c8329b99f1f99e112f9bac95a29bb31668ce83dfe`；
JUnit307806 bytes、SHA-256
`ce8f437048cdff487f96bf4a220a703bcbfb013eccc07706008c9ee90df9b429`；
summary16233 bytes、SHA-256
`736c3197e9e0985a740ee992529f71cf1180a939d90d01404653ab1decc0c900`。
完整原件、实际argv、源／库及派生身份收据均位于
`evidence/n5-restore-canonical-sites-diagnostic-02/`；外层终态位于
`evidence/n5-restore-sites-execution-launch-01/after.json`，SHA-256
`a4f152598690ca1af2979b2feeda3eb271308354cc6117b73cf8928d0e3d1b30`。

动态解析在声明范围内为`COMPLETE_FOR_DECLARED_SITES`，13方法共有167个
restore内完整成对site。全部167个restore parent为0，观察到的wrapper最大深度2。
这里只计各restore内constructor paired site：例如失败method4为32对，seq0的独立
freeze／constructor即使也映射到同源901行也不加入这32对，不能把聚合33次当作32对。

| 失败方法（原顺序） | 整方法constructor成对site exclusive wall总量 | 边界 |
| --- | --- | --- |
| 4 two requests/two nodes | 0.416700538秒，32对 | 包含整方法fixture／收尾，非原5秒等待分段 |
| 5 feedback after capture | 0.360529193秒，16对 | 同上；owner CPU与wall分别保留 |
| 13 empty S | 0.271093733秒，16对 | 同上；未证明删去工作即可满足原predicate |

失败method4整方法wall为**12.689821179秒**、owner CPU为
**12.616421923秒**，支持该次整方法累计了较多owner CPU工作；不能据整方法推导
5秒wait内成本占比、零锁等待、CPU／存储的唯一根因或历史Clang原因。
inclusive与exclusive不能相加；exclusive只剔除嵌套的被instrument wrapper成本，
仍含观察扰动。共享helper的direct caller也不是完整T0／T2／T4栈归属。
这次证据尚未满足生产优化的因果条件，**没有新增生产修复，没有新的正式绿色门禁**。

准备版01从未编译／执行，不能当生产红例。非作者原静态review登记D01（分析结果
部分JSON可使收尾漏写）和D02（未解析restore caller可误称覆盖完整），原件及其
初次followup的路径身份假设错误均保留。02另冻结新plan，最终静态followup在
限定范围确认两项工具问题已处理；该review不是运行通过或正式批准。收据位于
`evidence/n5-restore-canonical-sites-nonauthor-review-01/`，final followup的SHA-256为
`2943ea6256012d88d50d6e24aa640e0a6e0dd4849db7ab6f96c700814a01ca07`。
终态另有非作者限定证据复核，4648项断言／36份小原件，收据
`evidence/n5-restore-sites-terminal-nonauthor-01/review.json`的SHA-256为
`12b0d863f8c9f2358eacb0f65410dc1249863ad4224ced6132aceebb071aa4be`；
该复核未运行产品／工具、大ELF hash或检查活动Make，不将失败诊断提升为绿色门禁。

外层before／after复用了Make的被动资源capture函数，所以其`runner_inputs`
原字段列出Make receipt.py和run_n5_make_asan_02.py，**不是诊断工具身份清单**。
字段原样保留，不重跑、不改写；诊断实际身份以a7 plan的input_files、frozen-code、
实际执行argv及summary prepared_code_unchanged等前后事实绑定。

只读最终checker初版也有非作者静态工具发现，未冒充产品失败：C01要求JUnit的
notrun／disabled／error不能计PASS，C02要求绑定准确诊断身份而非仅非空JSON。
初版与review保留，新v2源码已收紧这些检查；当前没有运行checker或其合成测试。
Make终态和owner明确释放窗口前，不重hash大ELF／归档，也不宣称最终文档核验通过。

新版checker随后取得非作者**限定静态**followup：28／28小文本／JSON及控制流检查，
C01／C02在原发现范围内标为`SOURCE_ADDRESSED`，没有新增阻断。原收据位于
`evidence/n5-final-report-checker-nonauthor-01/review-followup-v2.json`，9375 bytes，
SHA-256为`3e23d63b9ba118de87fa3245525c70c1106b3cbf394fe42bc833caeaec0a79ce`。
它只复核当时稿件快照和v2源码，未运行checker、合成fixture、产品或编译器，
未hash大ELF／archives，也未读取活动Make；该followup不是新总稿最终哈希核验、
执行窗口释放或主协调者正式批准。

N5产品整体从aa2c592至c2a5cea跨模块PR累计**96个文件**，准确status／path及
目标source身份写入机器索引`final_n5_product_changed_files.files`。差异收据位于
`evidence/n5-final-product-diff-01/summary.json`，23926 bytes，SHA-256为
`d3e0da23da8290a527c1102498662461e46b70ca05835ca6b92ec0d058fe840c`。
该收据复用已有冻结871项源身份，没有执行产品或hash运行ELF。third_party、
docs/handoff、v0.4.1、原SQL和原reports的修改列表都为空；SQL仅新增已批准的
`db/migrations/013_event_call_association.sql`。**96是N5产品跨PR总体差异**；
当前报告分支最终文档提交预期只新增本总稿和机器索引两文件，不把96描述为报告补丁。

### 原Make-ASan正式尝试：完整失败收尾，原配方在Store停止

同一准确c2a5cea于**2026-11-03 00:15:01–01:06:53 UTC**执行原Make入口，
不是新CTest运行。普通用户UID／GID1000，GCC13.3，测试umask0002，
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`保留。新目录prebuild使用原目标展开
参数，原始CXXFLAGS完整记录为`-O2 -g -O0 -g -fsanitize=address -fno-omit-frame-pointer`，
prebuild **exit0、1668.794966秒**。随后未修改的原`make -j2 asan`步骤于
00:43:16–01:06:22执行，**1385.278294秒、exit2；外层exit1**。
实际终态session15445／chunkbf7a1a来自执行owner的终态handoff；被动after独立记录
numeric outer1，本身没有session／chunk字段，不能把缺失字段伪称来自after。

原runner `status=NOT_COMPLETE`保持。其含义是本次门禁未通过，执行本身已经完整
失败收尾、未中断，原stdout／stderr、命令和前后身份完整保留；无所属测试残留进程。
**原Make是94条配方命令目录，实际进入前20条：前19成功，第20失败，后74未进入。**
这组命令计数独立于94个CTest注册和各suite的方法数，不写94 PASS，也不将74计为跳过
或已测试。第19相关async_coordinator套件在原日志中为31 PASS／0 FAIL；第20个
persistent_event_store套件完整25方法，**21 PASS／4 FAIL／0未进入方法**。

四个失败是下面四个**方法组**，不是每个组内W阶段都已执行，也不是四个已证明的
真实业务效果缺陷：

| 原方法组 | 实际原断言与限制 |
| --- | --- |
| process crashes W06 through W10 | `tests/persistent_event_store_crash.inc:125: polled == 1` |
| process crashes W11 through W15 | 同一原断言；保留原日志中实际进入的阶段，未补造后续阶段 |
| process crashes W16 | 同一原断言；原业务校验未放宽 |
| legacy process crash W19 | 同一原断言；原兼容场景保留 |

这是原**5秒IPC读取屏障**内的`polled == 1`等待未满足。原件没有记录actual polled
数值，不能写成已证明返回0；精确阻塞阶段和根因继续**OPEN**，不能据此武断归因为
生产死锁、CPU、存储或历史Clang。原Make在该suite失败后按原规则停止，
**从persistent_event_runtime开始的剩余74条配方未执行**，不使用其他配置结果填补。
原stdout／stderr没有观察到ASan／LSan ERROR标记，也不能据此宣称sanitizer清洁通过。

source **871** before＝after＝freeze，HEAD保持准确c2；本次Make binary／build
身份map为**60项**、前后相同，包含archives等，**不是60个ELF，也不沿用CMake的63项**。
原Make不产生CTest JUnit／LastTest；本轮证据是原Make stdout／stderr及原runner收据，
不制造JUnit、不挪用其他配置LastTest。原summary3562 bytes、SHA-256为
`eb32ddf5ee90630499d24813e7def060a9799f5ae7a1b1b0707226cf32735034`；
stdout41369 bytes、SHA-256为
`bb5dd3f3e9e8ccbfd243d446b1873c504425b2702ae46f423646353176f93130`；
stderr6784 bytes、SHA-256为
`598a4bf78aaeb8d48707c1bb1f08a98843975bf4588d5aada0c8271de3628277`。
原件目录为`evidence/make-asan-20261103T001500Z/`。

被动外层before／after与精确runner来源在`evidence/n5-final-make-launch-01/`，
after15100 bytes、SHA-256为
`cc73c45ccaf36da9fc72e90288af46e0fa58157fde752fb6b8d7d13240a23076`。
owner随后于01:08:12 UTC明确释放本任务测量窗口，终态handoff2439 bytes位于
`evidence/n5-final-owner-handoff-01/final-handoff.json`，SHA-256为
`391ea19af68a2a4a6a0658278ecdd117b66c31c5bfca24fa7e3d8076f36bdfc1`。
窗口释放允许后续只读核验，不授予产品接受、合并或付费调用。

Make终态另经非作者限定复核：**69／69检查、22份稳定小原件**，确认实际失败和
身份边界，无该证据范围的新阻断；未执行产品、checker或大ELF hash。收据为
`evidence/n5-final-report-publication-review-01/make-terminal-review-01.json`，
17318 bytes、SHA-256为
`432a71fb273a4e8b2d6f4813e3b24a45ab02b1780485ff81c78267b6e7222270`。
包含当前稿件身份的最终文本／隐私followup和一次完整checker终态将保留为**外部收据**，
避免把报告自己的hash递归写回报告。**本稿冻结时checker尚未运行**；执行者是`/root`，
不是报告作者。完整证据和正式接受是不同状态，整体仍`VALIDATION_BLOCKED`。

另保留18:25:55 UTC、UID1000的主机观察，分类为
**READ_ONLY_HOST_OBSERVATION_NOT_CAUSAL_OR_TEST_GATE**（READ_ONLY_NOT_CAUSAL）。
它记录该时点的load／pressure、内存与cgroup CPU／IO等观测，只作上下文，不是
性能原因或测试门禁。原79654ca两个mask约860秒与本次c2a5cea 0022约2050秒来自
不同输入、运行时域与条件；不能据这份单时点观察直接归因新代码或环境，也不据此
给出速度百分比。其他正式本机配置与兼容性仍需独立完整收据，
**LOCAL_VALIDATION_COMPLETE=false**，历史NOT_FIXED和既有失败原件不改写。

最终实际工具在19:00:18–19:01:20 UTC于同一准确c2a5cea输入执行，收据为
`TOOL_SELFCHECKS_NOT_94_PRODUCT_TESTS`、PASSED、errors=[]，七个入口步骤均exit0，
head before／after均为c2a5cea，871项source before／after等于冻结输入。
六个unittest入口的expected、observed及完整原stderr逐项一致，均terminal OK、skipped0：

| 实际工具入口 | expected／observed方法数 |
| --- | --- |
| gcc-runner | 57／57 |
| immediate-unittest | 5／5 |
| transport-observation | 11／11 |
| private-evidence | 16／16 |
| windows-ctest-evidence | 22／22 |
| windows-image-provenance | 12／12 |

上述合计 **123个unittest方法**，原verbose日志也各有相同数量的逐名ok行。
`native-observation`是另一个standalone入口，核对**三个场景**，不另算unittest方法：
killed blocker为预期SIGKILL／-9，released failure为预期exit1及3pass／1fail，
released all-pass为exit0及3pass／0fail，三个child均reaped，入口自身exit0。
受控场景中的FAIL／SIGKILL不计作产品失败。该入口在工具内部编译自己的观察辅助程序
exit0，不是重跑94产品注册；本报告作者没有编译或执行它。原harness基线8方法／5fail、
候选26方法／0fail保持原件及原范围，这次实际工具执行也不把历史SIGTERM20s、
托管私有原件或Windows镜像来源问题自动关闭。

最终capability／compatibility02在19:02:15–19:02:42 UTC实际执行，分类为
`STANDALONE_CAPABILITY_NOT_PRODUCT_REGISTRATIONS`。final411 probe编译／执行均exit0，
原日志仅证明该guard **1 passed／0 failed**；随后final411 init exit0，old410的
pending／init各 **exit2**，原stderr均为
`{"error":"unsupported","message":"unknown database version; explicit migration required"}`。
readonly前后四项文件身份全部相同；takeover前后主DB哈希仍为
`c97c2b25b3e54bec48fdfe4949d95f55b61881422c4e212e2ccd34b5e18f4f22`，writer-lock
也保持空字节哈希。然而takeover before记录WAL与SHM，after两项均消失，
full-file集合断言实际失败，summary为 **NOT_COMPLETE／AssertionError**，整个入口
exit1按owner交接记录。WAL before的SHA为`e3b0…`（空字节hash）；SHM的SHA为
`fd4c9fda9cd3f9ae7c962b0ddf37232294d55580e1aa165aa06129b8549389eb`，**不是空字节hash**。
02未保存SHM body或size，不能称SHM为空，也不重建其内容或长度。

源／head／输入与派生binary身份保持，并不抵消这次文件集合差异。02原日志、
四份readonly／takeover身份快照及失败收据全部保留；cause／兼容验收保持OPEN。
02交接时作者曾拟议03严格工具修正：显式关闭readonly observer、保持全文件集合及
DB／schema／rows检查，并拒绝非空WAL／未知sidecar。该待执行状态由下文实际03
结果接续；02原失败与原因OPEN保留，不把差异预先归类为纯工具问题或产品通过。
非作者有界源码复核另确认旧Store对版本>410先拒绝、随后才close，WAL pragma位于
拒绝路径之后；这些源码事实不替代02的sidecar观测，也不自动闭合证据／harness验收。
该非作者只读源码与既有证据复核收据为
`evidence/n5-compatibility-sidecar-review-01/source-facts.json`，SHA-256
`082829d5ac6ff618308bab010b2ddac55c1f0a79015700fa2af7ef6956825000`；复核没有编译、执行产品、
连接数据库或运行工具；该早期复核时点尚无03执行结果，后续实际结果单独登记。

03工具准备包由concurrency_fix编写，32份冻结输入逐项重哈希，原02 runner及31份
原件保持。纯mock入口首轮实际41方法、0fail／22errors、exit1，原因是fake connect
参数名与uri=True冲突；原错误、输入和日志保留。只修fake签名后的execution02实际
**41方法、0fail／0error／0skip、exit0**，real SQLite／flock／subprocess／shell／fork
五类禁止调用计数全部0。该41方法属于另一个私有observer harness，不增加产品94，
也不增加先前实际集成工具123方法＋3场景。19:30的非作者静态审查结论为有界范围
无阻碍，保留完整raw集合、身份、schema／typed rows和显式close守卫；当时尚未实际03，
静态审查与纯mock通过均不代替兼容验收或正式批准。

实际compatibility03在**20:15:36–20:16:05 UTC**执行，summary为
**NOT_COMPLETE**，outer **exit1**按root交接；原错误是
`database bytes, identity or sidecar inventory changed before observation`。
final411 compile／guard probe／init均exit0，guard原日志仍仅1 passed／0 failed。
initial与readonly-before的observer均显式关闭SQLite、descriptor和writer-lock；
随后old410 pending仍按预期exit2／unsupported，但raw集合从主DB＋writer-lock两项
变为四项：新增**size0 WAL**与**size32768 SHM**，SHM的SHA仍为
`fd4c9fda9cd3f9ae7c962b0ddf37232294d55580e1aa165aa06129b8549389eb`。
主DB（`8b3f3f3085b3c6f99f2f1f92b02d2991fee47f4e1a1d22b177c4c03629434f55`）与
writer-lock的完整raw记录保持；全文件集合严格断言失败。after observer重新取得并
释放原writer-lock，在sqlite3.connect之前拒绝；没有成功的after semantic快照，
不声称post-command schema／rows等价。**takeover未执行**，不能沿用02的exit2填入。

37份稳定03文件／36成员manifest全部核验，inputs16／source871／derived binary1
前后保持。非作者只读实际证据复核也确认上述边界；03观察到的新32768字节值仅属于
03，不能回填02缺失的SHM size／body。03结果说明“仅未关闭observer足以解释02”
不足以解释全部观测；原因与兼容验收仍OPEN，不能宣布无sidecar副作用、差异无害或
生产缺陷。03交接时拟议的控制实验已按下文独立完成；它支持有界机制，不改变02／03
原严格断言、失败状态或缺失证据。

控制工具初版control01在未执行时由非作者静态发现 **C01：输入身份基线被重置**。
原问题、初版七份冻结输入及审查收据保留；另建revision02保留最早hash、逐项验证
既有输入后才追加新库输入，静态followup确认C01在有界范围已处理。旧八方法仅准备
而未执行，不把该静态问题写成生产反例或旧工具红绿。两版各七份冻结输入与原71份
02／03相关文件均逐项核hash／size保持。root随后实际执行修订版纯mock一次：
**9方法／0fail／0error／0skip、exit0**，原verbose逐名9条ok、terminal OK，
real SQLite与Popen调用均0；这9方法与94产品注册、工具123＋3、observer mock41分别计数。

一次真实A／B／C控制实验在**21:08:09–21:08:13 UTC**完成，分类
**DIAGNOSTIC_NOT_GATE**。原summary同时记录 **experiment_completed=true、
status NOT_COMPLETE、compatibility_acceptance OPEN**，outer exit1由root交接。
A使用一份新建411 fixture先做Python SQLite RO读取user_version再显式close，新增
WAL size0与SHM size32768（SHA `fd4c9fda…`）；随后在同一fixture上RW读取并close，
该两sidecar消失。A的RW承接RO，不声称是另一份fresh RW控制。B使用另一份新fixture，
仅执行old pending，预期exit2／unsupported并新增同类sidecar。C使用第三份新fixture，
仅执行old init，预期exit2／unsupported，记录的raw全集与semantic均同。

三个case内主DB、writer-lock、directory记录及各自全部**83表**的schema／typed rows
保持；这些是新建fixture的默认／metadata数据，不声称已填充业务E2E兼容。七次observer
均在自身观察前后保持完整raw不变并显式关闭；跨操作的A／B sidecar变化单独保留，
不归一化或隐藏。initial八键原值一直保留在before＝expected＝after十九键中，H03
原摘要与871源保持。85份稳定原件／84成员manifest及非作者有界实际证据复核已纳入。

Python SQLite **3.45.1**由实际process maps记录载入库；两CLI由其环境下ldd解析，
均指向库SHA-256
`85265a9d4afca6f4b325ceb078b669c754fb881abed4cafe91ccebe9d625d975`。
这支持同库sidecar生命周期机制；CLI证据是ldd解析，不能写成其实际运行进程mapping。
Python URI／单个PRAGMA与旧C++ open_v2的FULLMUTEX／NOFOLLOW及connection
foreign_keys／synchronous设置不同，不能等同完整旧Store行为或因机制支持而放弃全raw
兼容断言。02／03及该诊断仍NOT_COMPLETE／OPEN，不称无物理副作用、全兼容通过、
已关闭根因或正式接受，也不回填02缺失的SHM size／body。

在固定 79654ca 源 bytes 上的 AST / CI 入口清点为：GCC runner selfcheck **57**，
private evidence **16**，immediate diagnostics **5**，transport observation **11**；
native observation 是 **1 个 standalone runner + 3 个 scenario**，不混称四个 unittest
方法。清点者没有执行工具或产品，结果是 `STATIC_ACCOUNTING_VERIFIED`，不是工具
PASS；也不计入 94。私有复核 `evidence/n5-79654-tool-entrypoint-accounting-01/review.json`
SHA-256 `c4928fff27e82cdca7aadfeed054f3ffb20a008883ae128c9b94d15b4794b787`。

## 收尾阶段的只读证据交叉核对

`/root/report_completion` 接续本草稿，仅读取已完成收据并维护当前汇总；没有编译、
执行产品／实际工具测试或读取活动 ASan 文件。原作者、原状态窗口、2345 项证据
前缀与历史失败全部保留。当前摘要中滞后的 Clang BUILDING／UBSan NOT_RUN
已经依照两份完整通过收据修正，旧值另存机器索引的 report_update_history，
不将旧窗口记录改写为后来的结果。

`/root/validation_receipt_audit` 于22:03:59 UTC对已完成来源完成有界只读复核：
**1223份既有文件、3760项检查**，结论为
`NO_BLOCKER_FOUND_WITHIN_EVIDENCE_SCOPE`；严格告警、准确head／tree及三种构建
配置另有补充收据。这些是证据／静态检查数，**不是产品测试数**。该审阅初版
audit01对清单排序的格式假设错误原件保留，不归为产品失败，也没有重跑工作流。
`/root/historical_boundaries_audit` 的另一份只读审阅绑定56项输入，确认原
HTTPS、容量、大转义输出、structured CLI、runtime、SIGTERM、私有托管、
Windows镜像／Win11与历史Clang问题的开放边界；其P2文档发现是上述当前摘要
滞后；作者修订后，原审阅者的独立跟进仅将该P2文档项标为
`ADDRESSED_IN_BOUNDED_DOCUMENT_REVIEW`。原review／manifest未改，其限定关闭不
涵盖后来的ASan终态。两份审阅均无产品／工具运行或活动ASan读取，不构成主协调者批准。原收据及SHA见机器索引新增审阅项。

## N5-2：已准备、非作者复核完成，采纳仍 OPEN

独立 PR21 提案 `4326ea5004889c4e14b31e26c6b86e6b0a8509b3`，tree
`99e136daffd4efa87a036bb98d88c0da73845bd1`，基线为 PR16 的 451b232。
它不在冻结产品输入中。四份设计合同、原 21 项 source manifest 及两项补充输入已
准备，建议最小三节点 `a → c ← b` 的版本化图事实、T2/T4 依赖凭据、独立候选映射，
以及后继新的有界 ReviewPair host scope 与单层父预算逐次借记。

非作者 `/root/delivery_report` 对初版 edf08f0 的有界复核提出实际能力描述修正：
区分原 Fake 三维 unknown 与 live 五维完整性，新 review cache-only-null HOLD 是
拟议新增守卫；补齐矛盾 pass、重复 findings、精确 host-check 集合、双边恢复原子性、
action 专用 T1/T4 容量尾部与 D2 恢复连续性见证。对 4326ea5 的复核记录为
`DOCUMENTATION_FINDINGS_ADDRESSED_NO_NEW_BLOCKER_FOUND`，输入全部相等、原 manifest
不变；这只关闭该次文档复核项，不是生产实现或正式架构批准。

D1–D6 的协调者采纳仍 **OPEN**；`architecture_adoption=false`、
`implementation_authorization=false`、`formal_pr_approval=false`。014/412 未获授权，
票据／预算后继迁移编号未分配。DAG、独立业务审核票据、ReviewPair 生产接口均未实施；
原 `independent_nodes` 与 required-review Unsupported 守卫保留。所有 G/R/B/S/C oracles
均 PLANNED / NOT_RUN。没有编译、产品测试或旧 411 ELF 拒绝 412 的实际执行证据。

## 原始证据索引与后续完成条件

机器索引记录完整 source / tree、PR 关系、CI event/job/checkout/parents、原执行与
SHA-256。私有原件保留在任务数据盘，仅将安全路径、结果与 hash 写入报告；原日志、
数据库、ELF 和原 archive 不进入此次 Git 提交。

| 私有原件或复核 | SHA-256 |
| --- | --- |
| `evidence/n5-0-layout-control-01/summary.json` | `b8f821e36e53a2606e4dd2a0763dd63377c2de9ddd9ab007a410aaf85fa3b231` |
| `evidence/n5-1-store-process-crash-execution-01/execution-review.json` | `70ddba66f391c16feabec5e8ecfbae4104b352f7e04e6f0920e8ddc5a69c3a7a` |
| `evidence/n5-integrated-targeted-build-08/ctest-logs/LastTest.log` | `b41d0d4d95b61df10e030a14928a9fc902e99c6bdf8a2b159d65e1604ad299cd` |
| `evidence/n5-1-store411-compat-01/before.log` | `80c6cb3bc7b37d3c7915b86f5f6b1789702af1c5e5321124d7939f062101faed` |
| `evidence/n5-1-store411-compat-01/after.log` | `9411ec36cbdaf670dddf47e2e25fdae1f8a3c557f5c387c402e360d87cb9c13d` |
| `evidence/n5-1-capability-and-compatibility-01/summary.json` | `20fee34d168baf649a7c3a43a6df633fba15cdb87b243ea4dc38eb24ee35fa50` |
| `evidence/windows-af3-final-delivery.json` | `71838d911525be4c7f4065ee2d3d9fb6727b03ff62532fda55ebd2edd34e6a4f` |
| `evidence/n5-2-nonauthor-design-review-01/review.json` | `665604e5a69559c54c7843c43e61fa8f4efeeae872b852c8bb2f357b8d54edad` |
| `evidence/n5-2-nonauthor-design-review-02/review.json` | `753aea34d899711da46e6a081dc207a4de3019c8ef1c541b2550dbea37a21200` |
| `evidence/n5-final-report-preparation-01/setup-and-historical-preservation.json` | `c444cb048366957909819eda5f443147f1bfceeb109cbcb2cca530e1f0ebfb01` |

当前待交接事项是最终本机剩余矩阵、compatibility02／03处置，以及历史失败与平台／治理
边界；准确c2a5cea的既有远端CI、本机GCC Release双mask、Clang18 strict与UBSan
已收齐；共享ASan／LSan已完整失败，原Make已完整失败（step2／outer1、Store21／4，后74配方未运行）。原PR23／PR24
其他CI的历史时域、Make-ASan／GCC ASan原失败、三轮NOT_FIXED的处置、hosted私有
证据持久化、Windows 11／SID／build策略与历史image冲突、N5-2正式采纳和准确提交的
正式审核继续分别登记。

原截点的文档核验入口
`evidence/n5-final-report-preparation-01/verify_final_draft_v4.py`及早期v2／v3、
原verify_draft.py和失败收据全部保留；v4绑定当时共享ASan进行中的快照，
**不作为本次终态补充的最终checker**。接续作者先前只读核验原2345项证据、
270份历史报告及871项冻结源通过，收据保存在preparation-02；新的GCC终态补充
仅小范围读取已完成原件，未在root定向诊断期间启动大型重hash核验。
原Make收据现已稳定、owner已明确释放窗口；接下来由owner一次执行v2新快照入口复核全部inventory、
历史报告、冻结源、相对链接／空白和当前状态，仍要求准确产品输入与报告分支之间
只有获准的文档差异，不宽泛忽略生产变化；当前该最终文档核验为PENDING。

所有仍OPEN的项在机器索引中单独登记。报告准备完成与正式产品验收分开；本草稿
等待最终文档核验和本地集成人复核，再由获准集成人正常提交／推送；不据本文合并或扩大生产范围。
