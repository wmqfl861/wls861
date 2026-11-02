# wls861 本地 Agent 下一阶段执行提示词（N5-0 至 N5-4）

仓库：https://github.com/wmqfl861/wls861

## 1. 角色与权限
主协调 Agent 负责架构决策、任务排序、独立审核、缺陷裁决和最终验收。你负责数据盘上的源码实现、构建、测试、诊断、正常提交/推送和证据归档。

不授权：写入或合并 main/PR、强推、覆盖其他 worktree、真实供应商模型或付费请求、生产数据操作、修改历史 SQL/v0.4.1/原始失败日志、放宽权限/CAS/UNKNOWN/预算/deadline/断言/sanitizer。

## 2. 接手基线与必读资料
最新审核对象：
- PR #15：https://github.com/wmqfl861/wls861/pull/15
- 最终 head：aa2c592b8aed6509189710cc6f6d69e0959098a8
- PR base：docs/zcode-final-validation-20261101 @ 4289589384d75f29d42233cbaad256ceb711e9bf
- 完整本机矩阵输入：7bf3a16d9683ed6a8de65d88023bae9687abd20d
- N0 整合输入：f3fc9bfaf345030ee994097d2a2443e3c290e9fc
- PR #13：ceaa9b3e18618c592345001dfc597bdaf579cf81
- PR #14：4289589384d75f29d42233cbaad256ceb711e9bf

先阅读本交接目录中的 REVIEW_AND_HANDOFF.md 与 NEXT_AGENT_PROMPT.md，以及：
docs/reports/n0-n4-final-validation-20261102.md
docs/reports/n0-n4-final-validation-20261102-index.json
docs/implementation/next-cycle-20261102/README.md
docs/implementation/next-cycle-20261102/01-async-inbox-driver.md
docs/implementation/next-cycle-20261102/02-dag-review-tickets.md
docs/implementation/next-cycle-20261102/03-windows-readonly-identity.md
AGENTS.md、STATUS.md、ROADMAP.md、相关 ADR、A3/A4/S1 EventRuntime/Store/Windows 契约。

若远端已有更新，先检查依赖和差异，不覆盖其他成果，不只 pull 旧 main。

## 3. N5-0：本机失败和证据闭环，最高优先级
冻结唯一集成候选 SHA。完整矩阵真实输入仍是 7bf3，不可把 aa2 的文档发布当成本机重新执行。

分别定位：
1. UBSan HTTPS partial upload：peer.prefixes=0，预期 1；保留原 3500ms 与 900000 padding。
2. ASan live_model_store_tls 容量子进程原 18 秒期限超时。
3. ASan 大 escaped output 原 16 秒断言失败。
4. UBSan structured CLI 原 50 秒子进程超时。
5. 代表性 runtime timeout，取得阶段级阻塞/CPU/IO/锁证据。

每项先写可证伪假设，使用最小单因素实验。区分夹具、传输、性能回归、死锁与环境资源。不得延长原 deadline、减少输入、放宽断言、关闭 sanitizer 或反复重跑直到绿色。无法解释就保留 OPEN。

补齐 hosted 完整私有 LastTest/JUnit 持久化：
- 私有原件与公开投影分开。
- 私有 artifact 限权、短期保留，仅授权审阅者可读。
- 公开 artifact 只上传严格投影。
- success/failure/timeout/interrupted 四种终态均保存原件、SHA、命令退出和前后身份。
- 不得公开私有原件，也不得称投影等于原件。
- 若权限/保留策略不满足，报告阻塞，不得假称完成。

处理 Windows image header 与环境 ImageVersion 冲突：保留两个原始来源、run/job/attempt/checkout、采集时间和字段出处。不能解释则 UNRESOLVED，不得择一覆盖。

## 4. N5-1：持久 inbox 异步纵向闭环
依赖主协调者先采纳事件关联持久化方案。不要直接臆造接口或迁移。

目标最小场景：两个独立请求、每个两个独立节点，FakeModel 等待可重叠；plan/instruction/ack/note 经过真实 Store/EventRuntime/AsyncCoordinator 路径。

持久关联必须绑定 request/role/actor_task、原 claim/generation、捕获 ID 集合、目标/hash、原 attempt/envelope/binding、settlement/feedback 与逐 ID 完成凭据。

必须保证：
- 每个事件 ID 只由正确的原 claim/attempt 完成。
- 部分完成不吞掉未完成 ID；后到 ID 不被旧 claim 确认。
- 崩溃后旧 Actor/claim 不重派、不重新执行已发生的业务效果。
- 缺失结算/反馈/UNKNOWN 保留并阻断，不猜测成功。
- AMEND/cancel 精确重放无新 stop 副作用；真实转换只废止旧绑定。
- callback 不访问 Store；只有 owner 线程访问唯一 Store。
- 关闭先停止准入、回收/保留结果并 join callbacks，之后才释放 Store。
- 原同步 API、CLI 默认行为保持兼容。

首版不接文件/进程工具、live 网络、后台 daemon 或第二 Store writer。

## 5. N5-2：最小 DAG 与独立审核票据
仅在 N5-1 持久关联方案稳定、主协调者批准存储方案后开始生产实现。

第一阶段只实现三节点图 a→c←b：
- 原子图发布、节点/边唯一性、无环、容量、版本和权限检查。
- c 只有在 a、b 的当前 ACCEPTED 事实都有效时才可就绪。
- FAILED/CANCELLED/SUPERSEDED 与 UNKNOWN 分开处理。
- 每次首次派发和业务提交重新核图版本与依赖证明。
- 错误图提交零半写；历史图不可被新图覆盖。

第二阶段才做固定候选独立审核：
- producer 与 reviewer 必须是不同的真实授权请求/团队。
- 双方完整 binding、版本、epoch、CAS、attempt、预算和票据均冻结。
- candidate 与 artifact 映射明确且不可变。
- verdict 精确重放不增加轮次；changes_requested 使用新 task/instruction/ack。
- inconclusive、轮次耗尽、UNKNOWN 均升级，不自动批准。
- 模型 verdict 与独立确定性检查分开；二者均满足才可进入原 Dispatcher 验收。
- M1 WorkClass::Review 或 Journal ReviewRecord 不是 S1 审核授权。

保留旧 Schema/SQL 原字节。任何迁移先提交设计、旧 Store 兼容/回滚方案并等待批准。

## 6. N5-3：Windows 只读对象身份切片
冻结目标：Windows 11 x64、本机 NTFS、普通非提升用户、明确 build/MSVC/SDK/受信管理主体。

在 native-foundation 实现最小只读根身份与公共资源捕获：
- 根与每层祖先使用受控 Handle 固定。
- 验证卷/文件 ID、对象类型、reparse tag、owner/ACL policy。
- 拒绝路径替换、reparse、不支持对象和权限不明。
- 通过同一受控 Handle 有界读取到 EOF，核对前后对象身份、大小、时间与 hash。
- 仅读取编译认可 manifest 的公共资源；不开放任意路径、私有配置、凭据、Workspace 写入、SQLite、进程或 cleanup。
- HANDLE 不得伪装为 Linux fd/device/inode。
- 不支持能力必须拒绝，不得 stub 成功。

没有 Windows 11 普通用户实机时保持 NOT_RUN。Windows Server elevated 组件成功不能代替目标验收。

## 7. 执行顺序和并行规则
顺序：N5-0 先做失败定位与证据治理；N5-1 先提交事件关联方案供协调者采纳，再做最小纵向闭环；N5-2 依赖 N5-1 稳定后分两阶段；N5-3 可与 N5-0/N5-1 独立文件工作并行，但不能伪造 Windows 11 实机结果。

不同任务使用独立 worktree。src/store.cpp、公共头、CMake、CI、STATUS 由单一集成人维护，避免多个 Agent 同时改同一文件。失败链路不得阻塞无依赖的独立工作，但不得绕过门禁。

## 8. 测试与交付
每个真实缺陷先在精确旧基线上建立确定性失败，再在候选上同条件通过。新增守卫覆盖若旧实现本来正确，不得伪造生产缺陷。

每个最终输入执行适用的 GCC Release 两种 umask、Clang strict、UBSan、共享 GCC 13.3 Debug ASan/LSan、原 Make-ASan、Windows 目标环境测试。

测试期限、断言、权限与 sanitizer 设置不得放宽。区分 CTest 注册项、内部方法和工具自检。

推送独立 Draft PR，禁止合并。最终报告必须列出：
- 精确 base/head SHA、PR base/head、父关系；
- 修改文件与最小源码差异；
- 每项红绿复现的命令、退出码、日志和输入身份；
- 本机与远端各自完整矩阵；
- 私有/公开证据 artifact 的权限、保留和 hash；
- Windows 实际 OS/build/token/filesystem/toolchain；
- OPEN、NOT_RUN、失败、超时和下一步；
- IMPLEMENTATION_COMPLETE、LOCAL_VALIDATION_COMPLETE、PUSH_COMPLETE、REMOTE_CI_COMPLETE、PR_REVIEW_COMPLETE、MERGED 六状态。

不合并 main/PR，不真实调用模型、不付费、不操作生产数据。现在从 N5-0 开始；存储方案或安全策略需要协调者选择时，先提交明确选项、代价和验收，再等待决定。
