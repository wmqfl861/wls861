# N0–N4 定向整改与验证输入说明（2026-11-02）

本报告记录完整矩阵开始前的开发、定向诊断和交叉审阅范围。整体仍为
**VALIDATION_BLOCKED**。完整矩阵、最终提交和该提交 CI 必须另按实际执行输入
登记，不能把这里的诊断、兄弟分支 CI 或工具自检拼接为正式验收。
本地工程审阅不代签主协调者批准；本轮不合并任何 PR。

## N0：工作区与父关系

- 当前普通用户 `ubuntu`，UID/GID 1000；核验的数据盘为
  `/mnt/d`，挂载来源 `/dev/vdb2`，ext4。
- 本轮根目录为 `/mnt/d/codex-workspace/wls861-n0-n4-20261102-0k9_2wlf`；
  `candidate`、各模块 worktree、构建、私有临时目录及证据均在该数据盘。
  未重置、清理、stash 或覆盖已有工作区，未修改主机账户与安全策略。
- 交接资料固定于 `c33978ca62fe3fc63b4cf3440afdc4f5b647f13b`。
  它是资料提交，不是产品修复基线。
- 从 PR #14 的 `4289589384d75f29d42233cbaad256ceb711e9bf` 新建候选，
  正常本地 merge PR #13 的 `ceaa9b3e18618c592345001dfc597bdaf579cf81`。
- 集成提交为 `f3fc9bfaf345030ee994097d2a2443e3c290e9fc`，两父按序就是上述
  #14 / #13。两侧均为祖先；相对 #14 只增加 #13 的四个计费日志/自检/报告文件。
  两支的共同代码祖先是 `08dad7e815b9c3c84d3bd2393818b2030a6e0cde`。
- 候选分支 `work/coordinator-n0-n4-20261102-0k9` 已正常发布为
  [Draft PR #15](https://github.com/wmqfl861/wls861/pull/15)，base 是 #14 的
  `docs/zcode-final-validation-20261101`，明确同时纳入 #13。
  原 PR #1–#14 的 head/base 和 main 均未由本轮移动。

接手保存了完整命令、退出和 SHA 收据于私有 `evidence/n0-intake/`；首次发布收据
在 `evidence/n0-publication/`。main 接手时仍为
`a6b8559018a599d7a806b05942857a56d69a00ce`；后续发布前须重新读取远端。

## N1：诊断与限定生产修改

主协调者对 R-A3-01 / R-A4-01 的结论保持 **VERIFIED_FIXED_SCOPED**。
本轮没有重写 AMEND、资源头资格、Store、Actor、CAS、预算、claim 或 SQL。

本轮唯一生产改动在 `src/sse_replay.cpp`：私有 `within` 接受
`std::string_view`，只在原失败分支建立 owning 错误消息。原字节限额、取消检查、
错误代码/原文及 14 个调用点的条件不变。这消除成功的逐字节限额检查所产生的
临时堆字符串。原 ASan 容量超时、函数栈、受控红绿及其限制见
[N1 聚焦诊断报告](n1-focused-diagnostics-20261102.md)。

受控容量绿例使用只替换一个原 archive 成员的派生 ELF，不是新的完整 CMake gate。
新增长行/CRLF/chunk/原字节/hash/恰限额与超一字节拒绝方法单独通过；完整 SSE
套件仍在原第一方法达到 90 秒期限，未到新增方法，不能说 32 方法全部通过。
首次新增测试的类型比较编译错误也保留为构建失败，不算产品红例。

HTTPS 原 `prefixes=0`、large escaped output、历史 Clang、原整机矩阵失败原因仍
分别 OPEN。structured CLI 原 50 秒场景本次单项未复现，不能改写旧失败。
runtime 的派生单方法及原 ELF 单套件诊断也不替代原完整矩阵。

本机 ZCode 依用户原授权用于编码。第一会话 240 秒超时、无修改且无最终 usage；
第二会话成功，报告 4 次编码模型请求、92728 total tokens，费用没有可核查总额。
这些编码工具使用与 **WLS 产品真实供应商调用 0 次** 分开登记。

## N2：常驻回归与日志治理

新增方法进入原有 suite，没有新建产品 CTest：`async_coordinator` 29→31，
`store` 32→34，SSE 31→32。原 Linux 注册集合仍为 **92**，名称和期限未改。

- 实际 AsyncCoordinator/M1/Store 的 adapter exception、跨池同资源后继、
  retained/UNKNOWN、唯一 claim、控制、close 与仅证据回收覆盖，见
  [常驻回归报告](n2-permanent-regressions-20261102.md)。
- Store event 原 command/payload/hash、Actor/session/CAS、预算/epoch、精确重放、
  拒绝与事务回滚，包含完整 command 恰 1MiB、超界与深度 65 的对照。
- 独立 `ce7798d7979801f5319ae7a79636dfdd002af2d2` Release 输入的两目标
  CTest **2/2** 通过，65 个内部方法；不是 65 个 suite，也不是最终整合门禁。
  [原收据](n2-release-targeted-validation-20261102.md)保留其精确输入和日志 hash。
- 集成后的即时日志工具必须执行全部 5 方法，包括真实 billing `__main__` 入口。
  工具方法不计入 92 个产品注册项。
- 共享 GCC gate 保持原工具链、Debug、sanitizer、环境及期限，保存完整原 LastTest、
  JUnit、终态、前后源码和二进制身份。成功输出的 JUnit 截断不再代替 LastTest。
  原始日志在私有目录；公开 artifact 只包含经范围/hash/字段检查的投影。
  见[runner 契约](../implementation/gcc-asan-validation.md)及本批各工具验证报告。

非作者审阅实际发现过 runner 收尾和公开投影检查缺口；旧失败自检及对应小修正
分别保留，不能把工具缺陷红例叫作 WLS 生产权限漏洞。可捕获中断、末尾身份失败、
重复 JSON 键及被 XML 解析器忽略的原字节都需要有专门拒绝/收尾证据。
通过候选的投影不证明进程真实退出 0；必须另核原进程和 Actions 结论。

本机私有原件持久保存在数据盘。hosted runner 的私有目录只存在于临时 job 文件系统，
不上传公开仓库；job 销毁后的原件可取得性为 **NOT_PRESERVED_DURABLY**。
公开投影和原件 hash 不等于完整原件，未擅自创建其他存储。

## N3：Windows 来源与实机边界

旧 PR #14 的两组原件可按同 run/job/attempt/artifact/checkout 绑定：

| 事件 | run / job / attempt | 原 job `Runner Image.Version` | artifact 继承 `ImageVersion` |
|---|---|---|---|
| Push | 41301131767 / 124832085071 / 1 | 20261025.346.1 | 20261026.347.1 |
| PR | 41301137417 / 124832102399 / 1 | 20261025.346.1 | 20261026.347.1 |

Push checkout 为 `4289589384d75f29d42233cbaad256ceb711e9bf`，PR checkout 为
`6ed7684631bc27401a4e6c89228d86ac086641da` 的 merge 输入。原下载 artifact ID 分别为
13081940335、13081137127，
API digest、上传 ID/name 与 ZIP 字节一致。原 runner 从继承环境直接读取该字段；
没有旧采集时点或改写来源证据。这个差异仍为 **UNRESOLVED**，不是篡改结论，
也没有选择其中一个值覆盖原记录。

新入口在 checkout 后和工具链发现前分别记录 allowlist 环境、UTC、run/attempt/job key、
实际 checkout。job key 不冒充 API numeric job ID。两个环境快照一致仍不证明它们与
Runner Image 日志头一致；后者必须从同一次原 job 日志独立核对。

新增 `run_windows11.py` 使用原生只读 API 检查 workstation build 22000+、原生 x64、
普通非提升非管理员 token，以及 source/work 固定本机 NTFS；拒绝 Server、提升或
split administrator token、架构仿真和缺失观察。它复用原 4 个组件 suite、27 内部方法
及完整日志机制，没有实现完整 Windows runtime。

16 项合成 runner 自检已通过；本轮本机实际 Windows 11 产品执行仍为 **NOT_RUN**。
Windows Server hosted CI、Windows 11 普通用户组件、完整 Store/VFS/文件/进程/
沙箱/cleanup 是三个不同范围。没有购买机器、创建账户或改变 UAC/ACL 策略。

## N4：后续设计与采纳边界

[三卡索引](../implementation/next-cycle-20261102/README.md)包含异步 inbox 驱动、
最小 DAG/版本审核票据和 Windows 只读身份切片。现有接口、拟议接口/迁移、崩溃窗口、
权限、兼容、正负例和退出条件分开描述；40 个本地相对链接已独立核对。
设计文档的完成不表示方案已被采纳，也不授权开始未来 schema、DAG 或 Windows 强沙箱。

## 审阅和证据索引

作者/审阅者按模块登记：本机 ZCode/主执行 Agent、`/root/concurrency_fix`、
`/root/delivery_report` 和 `/root/phase_observation_review` 各自承担实现。
`/root/phase_observation_review` 对 N1/N2/N4 的冻结输入做非作者交叉复核，
也是 N3 代码作者，不能同时作为 N3 的独立代码审阅者。主执行 Agent 审阅了 N3
入口、原生主机守卫、来源绑定与 CI 接线，并执行 16 项合成自检；这不是 Windows
实机或完整权限验收。各份收据说明实际工具/执行范围，未称为第二模型独立评测或
正式主协调批准。

完整私有收据位于本轮根目录 `evidence/` 的 `review-n2-ce7798d`、
`review-n4-78faa75`、`review-n1-sse-input-01`、`review-n1-controlled-756946d`、
`review-n2-runner-97cf68a` 及后续修正版收据目录。旧失败记录和审核附件未改写。
最终全量结果、公开投影身份、Push/PR checkout、源码快照和发布状态需以随后实际
执行报告与远端读取为准。
