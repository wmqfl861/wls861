# ADR-090 — 全代码审核中的状态、文件与证据边界修复

日期：2026-10-27（America/Los_Angeles）。基线：`d3da700c9c47db86abe941374bcbdac5e31462d4`。
用户要求审核整个代码并直接修复优化；沿用此前开发、提交及推送授权。

## 决定

按内核/持久化、文件/进程、协议/计费、CLI/编排四条调用链检查当前实现，
以可重复的基线失败确定修复范围。版本为 `0.11.1-r3.3c-audit`，蓝图 v0.4.1、Store410不变。

1. Store 锁随最后一个共享文件描述符关闭释放。子进程析构继承对象不能主动解锁父进程的
   open-file description；不由此承诺跨 fork 继续使用 SQLite 连接。
2. 账本重建的预算、原 attempt、用量投影及未结算判断必须属于同一个 SQLite 只读快照。
   复用已有快照守卫，允许嵌入已有事务，不新增写者接管或持久状态。
3. live settlement 控制容量由当前五维投影是否完整、是否存在争议决定。原结算保持不可变；
   部分核对、PENDING/UNKNOWN 和争议继续占用控制容量。Fake/旧库保留原三维规则。
   完整但超预留的账目仍阻止新付费调用，不能把完成核账与可继续消费混为一谈。
4. 进程工作区观察先以 O_PATH 检查对象，再重开已固定的普通文件。实际读取不超过剩余预算
   加一个 EOF 探针；owner marker 与库存所引用的普通文件也必须通过实际 EOF，不能仅信任声明长度。
   保留原代次、权限、挂载、硬链接、完整清单及删除授权。
5. 启动前取消的 run 仍由原 ProcessRunner 返回、WorkerTools 保存 `spawned=false` 与完整退出事实。
   无效果的文件/note 取消记为 CANCELLED；原 Store 清理门禁和精确重放不变。
   旧的缺证据记录不能由补丁猜测或改写为有证据。
6. 旧 Schema 目录构造器复用已有固定 inode、有界 EOF 的运行文件读取器；按剩余 registry
   预算读取并在收集时检查数量。原捕获字节构造器、Draft-07、闭合引用与字段投影语义不变。
7. 配置准入与 HTTPS 传输共用目标地址解析器。非法/歧义 IPv4 及数字主机映射冲突在预约前
   拒绝；运行时 TLS、DNS 后端、取消与 deadline 检查保持。
8. 评测调用来源交叉核对原请求冻结配置、选中 profile、原输入及执行设置/hash。
   `execution_kind` 单列不能把 local_tls 提升为 provider；同步改写 settings/hash 仍须匹配
   原冻结 profile。原完整 logical request 也必须与 live admission 的原哈希相同，
   不能只验证修改后输入自身重算的哈希。保留 v1 结构性含义、v2 模型来源和 v3 逻辑 Agent 身份规则。
9. 重建原 Store 事实时保留 C++ 已保存的数字 token，避免 Python 将 `1E-1` 改写成 `0.1`
   造成合法输入、反馈和完整历史链误拒。原事实专用序列化贯通哈希与消息内容；
   计划和报告仍使用既有 Python canonical/digest，不能因兼容修复改变历史计划身份。
10. 同一个 `model_request_from_json` 调用内只完整恢复一次刚解析的 context，随后保留全部
    envelope字段检查和完整回序列化比对。序列化 helper保持私有；公开 `to_json` 与
    `validate_response` 继续完整验证可变请求。编译/捕获仅复用本次调用未变化的prompt字节或长度。
11. 一次 transcript 内复用已验证记录的canonical数组字节，避免反复构造/编码/解析每个prefix。
    每条记录仍以 `canonical_json(Array{record})` 保留外层数组深度、UTF-8及有限数字检查；
    两端括号和逗号计入原预算。每步原marker/chain及最终完整消息、Store捕获和预约边界重验保持。
    SHA仍读取完整prefix，不将其描述为全链路线性算法；不新增跨调用或跨事务缓存。

## 验证与范围

反例使用本轮基线 `git archive` 构建的原始库/CLI，在 UID/GID65534、空附加组运行；
不以既往 CI 或修改后的库充当旧实现失败证据。冻结后的完整源码另做严格普通用户回归，
发布后核对最终 SHA 的六路 CI 及 source snapshot。保持已有超时、断言、sanitizer、
Make CPPFLAGS 与旧套件。实际结果见[验证报告](../reports/code-audit-20261027-validation.md)。

不修改已发布 SQL、vendor、历史 handoff 或旧验证报告。没有真实供应商、付费调用或
生产数据操作；同一模型体系的多任务工程复核不能充当独立第二模型的业务验收。
未确认的问题、检查范围与工程限制见[审核报告](../reviews/code-audit-20261027.md)。
