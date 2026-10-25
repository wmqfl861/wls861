# R1.2：冻结选择的结构化模型请求

实现：0.7.4-r1.2；目标蓝图v0.4.1；设计见[ADR-068](../adr/068-structured-model-input.md)。

## 表示与持久化

`CompiledContext.logical_input` 在 v2 是可直接检查字段的 JSON 对象：selection、prefix、
完整 task_contract 与有序 messages。prefix 独立保存工具参数 Schema、具名 system 段和
依赖；messages 的 role/content/tool_calls/tool_call_id/source 独立，不把工具调用编码成
一条纯文本用户 prompt。`compile_structured_context` 的旧 messages 入参必须为空，拒绝
静默丢弃未转换历史。工具定义/依赖按名称规范排序，消息顺序保持。

消息只允许 user、assistant、tool。assistant 可提出1..32个工具调用；同一轮所有结果
必须完成后才能加入下一条普通消息，每个 ID 在整个上下文中唯一，不能有孤立结果、重复
结果或未配对调用。空的工具结果内容可表达真实空输出。历史中旧工具名是观察，不加入
当前 tool_names；权限仍在响应采用和 Store 的当前 grant/版本/CAS 处检查。

`wls-compiled-context-v2` 的 prefix_sha256、cache_identity、logical_input_sha256 分别
绑定规范前缀、完整模型/协议/思考缓存选择及结构化输入。`wls-model-request-v2` 再增加
logical_request_sha256，覆盖其完整捕获封装（含调用ID、绑定、冻结配置、输出限额、超时
和预留），计算时排除该哈希及 wire_request_sha256 自身。输入/请求两级 wire 字段均为
null：没有网络发送。哈希不是认证，改变输入并重算哈希不能绕过 Store 的授权。

恢复不仅核哈希，还重新编译验证字段、模型元数据、Canonical排序及工具配对。额外字段、
伪造 wire 指纹、格式/上下文不一致都拒绝。旧 v1 序列化、帧和恢复代码保持兼容。
`compiled_task_contract` 提供统一宿主读取入口，管理工作流和经验目标不再各自解析旧帧。

## 冻结选择与运行器

在新请求所用的 system YAML 中显式设置：

```yaml
context:
  request_format: structured-v2
  # 其他既有 context 字段继续保留，不能仅用此片段替换完整配置。
```

支持 framed-v1、structured-v2；缺字段按 framed-v1，未知值在准入前拒绝。兼容样例保留
framed-v1，`config check/explain` 返回 context_request_format 与 history_window_integrated=false。
已有请求只恢复其原冻结配置，不读取后来 YAML。Store 在新预约时同时检查格式选择，错配
不消耗预算；已有精确预约重放保留旧封装。数据库格式和已发布迁移不改变。

run-fake/pump-fake 的 Dispatcher、TeamLead、Worker 均可使用v2；管理角色仍读取当前权威
快照并捕获单一问题/提交/验收/经验目标。Worker 历史来源是自己作用域的已保存尝试。
单一、完整且已采用的响应与存在的原反馈核对后生成 assistant/tool 消息；result 保留完整
JSON、错误/失败状态与原文件字节，不把失败结算等同业务成功。

逻辑 tool ID=`history_`加（域标记、attempt_id、原call_id）规范JSON的SHA-256；原ID、
原版本绑定及记录指纹保留在source。跨重开或重复供应商ID不串结果。反馈缺失、迟到/未采用、
多个调用、无完整响应或运行时拒绝记录为明确的user观察；不补造结果、不执行旧工具。
402/403兼容入口的宿主反馈同样仅表示观察，不伪造原生工具对。当前404+路径忽略外来反馈，
以持久记录为准。

## 容量与下一切片

本批仍保留完整历史的256条/8MiB读取边界，结构化编译仍受1MiB总逻辑字节、256条消息等
限制；原生对占两条消息。超限在预约前明确拒绝，不裁剪约束或伪造旧文件结果。
这不是窗口或压缩实现。R1.3 必须把冻结窗口、遗漏索引、R1.1原始证据回读及未决状态保护
接入Worker并验证连续61,100字节结果和跨重开可继续；当前该问题仍开放。

没有新增真实模型调用、网络适配器、付费重试、弱沙箱、SQL迁移、模型新权限或自动压缩。
实际测试与发布状态见[报告](../reports/r1-structured-context-validation.md)。
