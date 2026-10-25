# R1.3：冻结 Worker 历史窗口与原始结果回读

实现 0.7.5-r1.3；蓝图 v0.4.1；依据 [ADR-069](../adr/069-worker-history-window.md)。
本实现是确定性的上下文视图与证据读取，不是LLM语义摘要，也不是实际供应商传输。

## 冻结与兼容

新请求可在 system.context 中显式配置 request_format=structured-v2，以及
worker_history={mode:window-v1,recent_attempts:4,inline_bytes:32768}。
近期数1..8，单轮编码后字节32768..65536，乘积不得超过196608；组合/类型/未知字段在配置校验时拒绝。
不提供自动迁移：缺失worker_history保持原完整历史编译；缺失request_format继续framed-v1。
更新磁盘YAML不改变已有请求。策略进入冻结快照、当前任务契约及前缀依赖哈希；Store预约再次核对。
config check/explain 对实际窗口选择返回history_window_integrated，并继续披露未执行的语义压缩/其他配置。

## 有界构建与事实保留

Store::attempt_history_tail 只查询当前Worker的request/role/principal/task/team，获取最后N项（至多8项）
及总量、高水位和省略数量。最近记录查询以捕获的高水位作上界，防止只读连接与写者交错时混入
后来新增记录；下一次捕获可观察新记录。成员范围固定不等于结算状态冻结，未决事实仍另行完整读取。
省略数量必须非负。近期原反馈重新验证完整哈希、原绑定与原响应；不载入旧预约上下文，避免递归。
每条原记录仍按现有8MiB上限解析；这是有界读入，不是SQLite大对象底层流式读取。

每个近期轮次经R1.2原往返编译并按编码后的完整消息大小决定：可内联则整个保留；否则整个变成user级
history_reference，带原attempt/call ID、原绑定、接受标记、反馈哈希、section长度/SHA和回读参数。
不会留下半个tool-call配对，也不把接受响应等同业务成功。更早轮次只从视图省略，不删除数据库记录；
history_window 明示总量、省略量和分页高水位。历史文本与引用均不是新的授权。

当前request_snapshot原文/全部AMEND/约束、task、instruction、binding、budget、questions、submission、
pending_events和wait保持完整。PREPARED/RUNNING/CANCEL_REQUESTED/UNKNOWN操作，以及PENDING/UNKNOWN、
用量未知或缺失原反馈的调用，独立于近期窗口固定保留；各集合超过64项会明确quota而不是悄悄丢失。
完整当前契约自身超出原编译上限时仍在预约前拒绝，需要宿主显式解决未决状态或调整任务；本批不自动摘要约束。

## 两项模型控制

history_page 参数恰为 after_sequence、through_sequence、limit；limit为1..4。
history_read 参数恰为 attempt_id、record_sha256、section、offset、max_bytes；section为response/result，
max_bytes为1..4096。身份、task、team、epoch不能由模型参数提供。使用R1.1相同原哈希及作用域校验，
结果是原section规范化JSON UTF-8字节的base64分片；需拼接后验证完整SHA再解码，片边界可以位于UTF-8字符中间。
只读宿主history-page/history-read保留原64项/16KiB边界，不改变其无写者、无预算副作用的诊断语义。

这两项模型控制只由冻结窗口显式授权，独立于业务文件工具allowlist/工作目录；当前runtime_policy可撤销。
模型预约仍要求正常业务就绪与预算，不能因回读绕过WAITING、UNKNOWN、AMEND、终态或未确认说明。
两项必要回读能力有任一被撤销，窗口预约会拒绝，避免继续生成无法取回的引用。
执行前后复核当前身份、语义/代次与政策；迟到调用依旧记账但不能成为当前动作。

每次模型查询仍是普通持久调用：先保存输入/预约，再保存原响应和真实或未知用量，最后保存不可变反馈。
同响应重放返回原反馈，不第二次查询或收费。最近60秒的两类settled查询（包括失败）按request/task/team/principal
从持久账本计数，应用冻结system.control.max_queries_per_minute，重开不清零。
频率窗口依赖可信宿主UTC时钟；不是多节点时钟协议。参数校验发生在授权后的执行前，拒绝也保存错误反馈。
4项索引/4096字节回读的正常往返可在最低32768字节阈值下完整展示，避免每次读取又被自身引用隐藏。

## 验证与剩余项

实际CLI验证连续16次读取61100字节，重开后再16次；无新TeamLead调用，全部原反馈留存。
窗口外原结果由模型回读，工作文件被改写后仍取得原字节；查询错误、预算、等待、当前政策及AMEND围栏覆盖。
近期读取还验证超过256项历史以及缺失调用在窗口外固定保留。额外的确定性交错回归在
COUNT/MAX与尾部查询之间提交新调用，验证当前窗口不混入、下一窗口能看到；失败回读也计入跨重启查询额度。
独立用例和全部构建结果见
[验证报告](../reports/r1-worker-history-window-validation.md)。原测试的无窗口触顶行为保留为兼容检查。

不改SQL迁移、vendor、交接/审核原件，不发起真实模型请求。管理角色窗口、旧请求迁移、LLM摘要、实际provider
wire收据、模型决策质量、强环境部署验收、否定验收后继仍独立推进。
