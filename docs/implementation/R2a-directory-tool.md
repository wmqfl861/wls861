# R2a.1：有界 Worker 目录创建

实现目标 0.8.3-r2a.1；蓝图 v0.4.1；设计见 [ADR-074](../adr/074-bounded-directory-tool.md)。
验证结果单独记录于 [R2a.1 报告](../reports/r2a-directory-validation.md)。

## 模型入口

`fs_mkdir` 是 `workspace` 副作用的业务工具，schema ID 为 `wls-fs-mkdir-v1`。
模型参数只有必填的相对路径：

```json
{"path":"out"}
```

父目录必须存在，只新建一个空叶目录；目标存在即拒绝，包括已有空目录。
不接受 `parents`、`exist_ok`、`mode`、身份、代次、版本或模型自报副作用。
成功后可用既有 `fs_write` 创建 `out/result.txt`，也可用原有有界列表/读取/搜索工具访问。

成功结果形如 `{"path":"out","created":true,"type":"directory","mode":448}`，
其中 448 是 Unix 权限 0700 的十进制值。失败结果为 null，原失败原因进入操作证据。
目录创建成功不等于任务验收或交付；它也不登记 runtime-owned envelope 或授予删除权限。

## 授权、兼容与操作事实

新 general 模板显式包含 fs_mkdir。已有请求继续使用原冻结模板、request allowlist 和执行策略，
不会补写新 grant；当前撤销政策仍可拒绝工具。共享 `wls-worker-tools-v1` 和 WorkspaceLimits
保持不变，避免已有绑定因全局策略哈希变化而失效。

工具仅在 Worker 的原业务路径执行。每个首次派发仍检查当前身份、完整 binding、
AMEND 逐任务处置、说明应用、暂停、UNKNOWN、权限和额度；先持久准备意图，取得唯一
`dispatch_claimed=true` 后才调用 Workspace。只读 `tool-query` 不能创建目录。

同 business key、同工具和参数返回原操作及原结果，不再次 mkdir，也不重扣业务额度。
同 key 改参数/工具冲突。UNKNOWN 重试同样只读回原事实；看到目录存在或不存在不能自动核清。
迟到结果保留原 binding；AMEND、取消和恢复后的旧操作不能被当成当前授权。
结果持久化失败继续触发原团队/代次的首次派发围栏。

## 文件系统边界

Workspace 沿用固定根目录 fd、mutex、合作 flock、当前 UID/权限检查和 openat2 的
BENEATH/NO_SYMLINKS/NO_MAGICLINKS/NO_XDEV 限制。无可用内核保障时拒绝，不采用字符串路径回退。
目标、祖先和现有树中的不安全链接、特殊对象、非归属或 group/other 可写对象都不能绕过旧检查。

创建前校验投影容量：新增目录占一个条目；目录组件数加一必须不超过 max_depth。
这是为了保持原 `collect(".", 1)` 遍历规则，防止新建一个后续所有完整树检查都会拒绝的目录。
例如 max_depth=2 可创建一级目录并在其中写叶文件，但不能再创建第二层目录。

在固定父目录 fd 上执行一次 mkdirat(0700)，打开并固定新目录身份，fchmod 到准确 0700，
同步新目录和父目录，然后核对根、父路径、目标目录身份、权限和空目录状态。
不临时修改进程全局 umask；常规 0002/0022/0077 下均得到 0700。

## 失败分界与证据

| 分界 | 返回语义 | 后续要求 |
| --- | --- | --- |
| mkdir 尝试前失败 | EffectNotStarted | 记录明确错误；没有执行本次 mkdir |
| mkdir 尝试后的失败 | OutcomeUnknown | 保留可能已创建的目录及原证据；不能自动重试或删除 |
| 创建、同步、最终核验全部成功 | Succeeded | Store 持久结算原操作结果 |

即使 mkdirat 由于最后时刻的竞争返回 EEXIST，也保守进入 UNKNOWN。异常路径不会盲目 rmdir。
每次证据保留 phase、mkdir_attempted、created、child_synced、parent_synced、durable，
以及当时可观察到的根/父/新目录 device/inode；失败另保留 cause_code 和原错误。
同步标记表示实际完成的同步步骤，不能替代最终状态、当前身份或独立副作用核对。

文件代理和合作锁仍属于原单用户宿主信任边界，不能宣称防御任意同 UID 宿主在校验后移动目录。
故障测试使用临时夹具与测试二进制私有 syscall 注入，不向生产 API 增加故障回调。
这些测试不代替物理掉电证明或强 bwrap/cgroup 正向部署验收。

## 后续

R2a.2 单独解决 PREPARED 分配的完整 staging、持久创建收据、原子无覆盖发布、精确代次恢复和显式放弃。
弃置必须保留原记录和残留，不能伪称 REMOVED；同时处理同团队新分配、路径保护、清理目标集合和容量结算。
doctor 的部署路径解释、R3 单协议离线回放及真实付费验收继续保持各自完成边界。
