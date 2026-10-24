# 进程结果结算与宿主核对（ADR-065）

本契约替代 S1-worker-tools 中“所有派生过进程的非零结果都是 UNKNOWN”的旧段落。
仅修改结果语义和控制入口；没有弱模式、网络放行、自动付费重试或新模型权限。
数据库仍为407；原 SQL、原结果、原始输出和记录哈希不改。

## 1. 失败与未知

WorkerTools 使用统一的 `process_operation_outcome` 分类：

| 执行事实 | 已结算状态 |
| --- | --- |
| 尚未启动、无残留不确定性的拒绝 | FAILED；原 ProcessResult.status 仍为 REFUSED |
| 启动前取消 | CANCELLED |
| 以下全部证明完整，正常退出码0 | SUCCEEDED |
| 以下全部证明完整，非零退出或信号终止 | FAILED |
| 以下全部证明完整，超时 | TIMED_OUT |
| 以下全部证明完整，输出超限终止 | FAILED，原状态 OUTPUT_LIMIT 与截断信息保留 |
| 以下全部证明完整，取消并已退出 | CANCELLED；绝不解释为撤销文件效果 |
| 已启动但证明不全、执行器异常、清单不完整或超限 | UNKNOWN |

所需事实同时包含：强隔离、真实 launcher 执行、已验证挂载与命名空间、命令启动屏障已释放、
network=none、cgroup-v2 的全部后代退出证明，以及完整前后工作区清单和一致的差异哈希。
单 PID 退出、任意 exit_code、模型声明或内部弱监督夹具都不够。
普通结果还须通过 Store 原始版本/身份绑定才能结算；取消、AMEND或恢复后的迟到记录仍不自动核清 UNKNOWN。
失败结算不重复原 op；当前批次仍可停止，新一次获准推进可以读诊断、修改或创建新操作。

## 2. 工作区证据

ProcessRunner 在同一根目录的排他合作锁内，执行前和后各做两次一致的完整扫描。
后扫描只在强隔离和全部后代退出均已确认后执行；锁覆盖前扫描、命令和后扫描。
/work 是唯一可写的持久挂载；临时 /tmp 不作为持久成果。其余挂载与网络条件沿用原强模式。

清单记录根的 generation/元数据、每条相对路径/类型、完整 statx generation、模式、属主、大小、
mtime/ctime 和普通文件完整 SHA-256。diff记录新增、移除、变化；根元数据另有 changed 标记。
结果包含两个原始清单及哈希，不仅是文字“没有变化”。effects_undone 恒为 false。

当前证据边界：最多256条目、总文件16 MiB、单清单48 KiB编码、32层、路径4096字节。
超限拒绝完整性声明，不截断清单。执行前超限且未spawn是明确拒绝；执行后超限为 UNKNOWN。
这组证据容量不是对全部项目规模的支持声明，统一冻结容量仍属 ADR-066。
拒绝链接、特殊文件、跨挂载、非本用户或组/其他用户可写对象；读取前后身份/元数据变化也拒绝。
同UID恶意宿主并发修改不在合作锁威胁模型内；不把两次扫描说成对恶意同UID代码的隔离。

结果/证据各受Store的256KiB上限，原输出继续独立BLOB保存。结算写入失败沿用原内存阻断和
持久UNKNOWN兜底，不允许随后取得新业务派发权。原有相关故障注入回归继续执行。

## 3. 受信本地主机 Root 核对入口

```sh
wls reconcile-op --db FILE --request REQUEST_ID --op OP_ID \
  --expected-version OP_STATE_VERSION --outcome FAILED \
  --evidence-file /private/host-observation.txt --key stable-reconciliation-key
```

允许结论为 SUCCEEDED、FAILED、TIMED_OUT、CANCELLED，仅处理UNKNOWN；不能用它执行命令。
证据文件为1–4096字节非空UTF-8文本，说明宿主观察/核对方法和外部证据引用，不能只含空白。
真实性由受信宿主操作员负责；这是持有本地控制数据库权限的CLI，不是新增网络身份认证。
模型工具表不注册reconcile-op。显式Root Store重载检查角色、root_id、请求归属及原操作CAS。
旧可信C++宿主重载保持兼容，模型不能直接调用。

CLI在读写者接管前校验选项、文件、角色/请求及CAS；取得写者后事务内再次校验。
事件包含完整证据，operation_intents只记录证据SHA；原执行结果/输出/退出事实不覆盖。
幂等意图使用稳定Root身份和显式op CAS，不包含易变的dispatcher epoch。精确重放跨重开有效；
同键异参冲突，陈旧CAS不修改状态。重放不重复reconcile_op事件（合法写者重开仍有自己的recover事件）。
事件插入失败，操作状态和证据引用整体回滚。人工核对不自动ack、不清等待、不批准AMEND，
也不能替代清理所需的原始进程退出证明；核对成功不等于cleanup完成。

## 4. 验证层次

实际测试分别覆盖：真实临时文件扫描/哈希/不安全条目、纯状态分类、真实SQLite事务和故障注入、
真实跨进程CLI、既有执行结算失败阻断。分类测试中的强隔离字段是明确标注的合成输入。
本批宿主无完整bwrap/cgroup委派，未执行强沙箱正向部署验收；不使用弱回退冒充成功。
P1-2容量、长历史压缩、否定验收后的返工仍见STATUS，不由本契约推定完成。
