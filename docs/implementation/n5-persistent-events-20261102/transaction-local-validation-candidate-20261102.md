# N5 事务内验证复用候选（2026-11-02）

本记录补充已采纳的 [013／411 契约](adoption-and-implementation-20261102.md)。
它是有界实现与失败处置记录，不是主协调者的正式审核或合并批准。
生产比较输入为 `79654ca9cca80d5ae8f5204df580797b04963460`；后继候选正常汇集
Windows 资源夹具、失败身份收尾和真实进程崩溃执行报告，再加入下列两个提交。
原分支、SQL、历史蓝图、报告和失败日志均保留。

- `752e37e06460c3aa8500e76eacd723be02d7af37`：规范 JSON 输出继续使用原编码器，
  改由同一 Boost.JSON 的完整 SAX 解析器验证 UTF-8、结构、深度和原 DOM 容量上限，
  避免分配第二份 DOM。严格输入解析、唯一键、数值有限性和错误类型继续执行。
  独立 oracle 使用原 Boost serializer 与原 DOM parser，比对完整字节和接受／拒绝结果，
  包括深度 126–129、空容器、完整 UTF-8、NUL 后字节、大键／字符串和数值边界。
- `2637d415246e2b1b93c3710ea3f3e3f9fb922bc8`：私有栈上 `EventLinkReadContext`
  在一个调用和一个事务内复用纯解码结果。每次仍重新读取 SQLite 中完整 envelope，
  只有完整字节相同才复用 ModelRequest；配置必须同时匹配完整 bytes 与 SHA。
  T2 唯一派发及 T4 note 的 prepare／start 使用本事务的已验证配置，仍核对各自新读的
  授权行、Actor、绑定、CAS、状态、工具 grant、UNKNOWN、预算与容量。

没有跨事务或跨公开调用的缓存。T4 preflight、业务事务与异常处理各自创建 context；
receipt、来源、投影、hash、授权与提交后校验没有缓存或删除。
公共 Store 接口、原同步默认路径、唯一写者、执行槽、线程数、期限、权限与冻结配置不变。
未启用 persistent events 的旧请求继续原路径；旧精确重放不取得新的业务权。

新增三项 Store 方法专门检查复用边界：T2 中 receipt 后篡改配置必须全事务回滚；
T4 note 在 prepare 原事实后篡改配置必须在 start 前拒绝，保留结算、账目、原 inbox／link、
任务、事件及兄弟请求；另一次公开调用必须拒绝后来改变的完整配置字节。
T4 用测试专属 SQLite 故障哨兵区分 start 前拒绝和仅在后置校验发现错误。
它们属于正确旧守卫覆盖：同一个已编译测试 object 链接原生产库和候选库均为 **3/3 PASS**。
检查包含 inbox／link 的 claim／generation 事实，未直接快照 mutable `event_actor_claims`
整行，不能称为穷尽 CAS 组合。新增 CTest 注册项为零，目录仍为 94 项；
`runtime_types` 为 8 方法，`persistent_event_store` 为 25 方法，runtime 为 13 方法。

三轮局部候选实验使用原 GCC 13.3 Debug `-g`、ASan／LSan、业务参数及期限。
最近一轮 build exit 0；types 8/8；runtime 9/4，失败位置为原 5 秒等待的
283、326、374、569 行；Store 前 8 方法通过、第 9 方法运行中在原 120 秒期限超时，
后 16 方法未进入。这不是完整 sanitizer 门禁通过。
原输入的 AMEND 场景在 369 行失败，候选推进到 374 行，但两轮 poll 工作量不同，
不可从嵌套耗时相加或直接比较墙钟推导百分比改进。阶段栈和限定 wrapper 显示 owner 的
同步 Store／配置／JSON 工作消耗等待窗口；这仍不足以证明所有历史超时的根因。

原始数据盘证据位于 `wls861-n5-20261102-csj777e9/evidence/` 下：

- `n5-runtime-asan-original-diagnostic-01`、`n5-runtime-asan-derived-diagnostic-01`；
- `n5-persistent-runtime-original-hotspot-01`、两个 `n5-runtime-owner-metrics-*`；
- `n5-event-validation-asan-targeted-01`、`-02`、`-03`；
- `n5-store-guard-baseline-02` 与 `n5-store-guard-candidate-02`；
- `n5-canonical-sax-nonauthor-review-01` 与 `n5-event-validation-step-reuse-review-01`。

每轮保留原 stdout／stderr、LastTest／JUnit、源码／ELF 前后身份、实际退出码和残留进程检查。
插桩、GDB 和只跑子集分别属于诊断，不能代替正式门禁；GDB 下原 LSan ptrace fatal 也保留。
有限非作者源码复核未发现范围内阻碍，不等于独立产品验收或第二模型评测。
最终统一提交的 Release 双 umask、Clang、UBSan、共享 GCC ASan／LSan、原 Make-ASan、
兼容性及远端结果由后续执行报告绑定准确输入，不能继承旧 SHA 的绿色。

当前结论为 **NOT_FIXED / VALIDATION_BLOCKED**：不再无假设重复整个 sanitizer，
不将局部复用称为已修复门禁。历史 Clang、GCC、live_workflow 问题继续 OPEN；
hosted 私有原件持久化的 ACL／保留／受权读取方案仍 BLOCKED，
Windows 11 普通用户 NTFS 仍 NOT_RUN，历史 image 双来源仍 UNRESOLVED。
没有真实模型、付费调用、main 写入或 PR 合并。
