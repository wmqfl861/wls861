# R2a.2：运行时工作区分配恢复与显式放弃

实现版本 **0.8.4-r2a.2**；蓝图 **v0.4.1**；依据 [ADR-075](../adr/075-owned-allocation-recovery.md)。
本契约描述实现边界；实际执行证据见[验证报告](../reports/r2a-allocation-validation.md)。
它承接 [S1.6](S1-owned-cleanup.md) 的受控所有权与清理，不改变 Worker `fs_mkdir` 的业务授权。

后续 [owned 贯通容量](R3-owned-workspace-capacity.md) 允许新请求显式冻结容量对象，
保存到原 intent 并以 marker hash 绑定；v2 原创建收据同时绑定该原始内容。
旧 intent/receipt 不回填，分配与恢复的代次、空树和无覆盖条件保持，本节点的 010 SQL 不变。

## 意图、创建收据与发布

新 CLI 使用结构版本 408。新分配先保存 `PREPARED` 记录及原命令键；v2 意图包含资源、请求、团队、
随机最终目录名、随机同父暂存名、nonce 和完整父目录身份，然后才允许文件效果。
父目录须已存在；规范路径、设备、inode、mount ID 和 birth time 在创建前捕获。
任何模板或模型参数都不能构造新的分配/弃置/清理权限。

创建分为四个阶段，文件系统发布与最终 Store 登记是两个独立的持久化边界：

1. 保存意图，检查 64 个有效资源、256 条终生历史及事件结算预留。
2. 确认 stage/final 均不存在；创建 0700 stage、0600 `owner.json`、0700 空 `work`，
   同步 marker、work、stage、父目录并复核固定对象。
3. Store 以原资源 CAS 保存不可变 `allocation_receipt`。它绑定意图哈希、父身份、
   stage/work 的完整代次、marker 哈希和同步完成事实；保存成功后才允许发布。
4. 再次核对当前 Root、请求/团队、资源 CAS 和未绑定状态；以 `renameat2(RENAME_NOREPLACE)`
   无覆盖发布，重新同步和核验，最后登记原对象的 OWNED 身份。

stage 的名字留在原意图和创建收据中；最终身份仅将这两个目录的路径平移到 final。
不修改原收据来伪装它是在最终名字下创建的。任一步失败均保留现场，不做猜测性回滚删除。
本次未新增持久失败现场日志：异常返回即时错误；持久证据是原意图、已经成功保存的创建收据、
最终身份及明确弃置记录，不能把事后查询或错误文字当作原始创建证据。

## 中断后的判定

| 已保存收据 | stage/final | 行为 |
| --- | --- | --- |
| 无 | 两者均缺失，父代次匹配 | 可以开始原意图的创建 |
| 无 | 任一存在 | 保持 PREPARED 并拒绝采用；可明确弃置 |
| 有 | 只有 stage，完整代次、marker、空 work 匹配 | 无覆盖发布原对象 |
| 有 | 只有 final，完整代次、marker、空 work 匹配 | 同步后补记同一 OWNED |
| 有 | 两者同时存在、应有对象缺失、代次/内容不符 | 保持 PREPARED 并拒绝采用 |

父目录、stage/work 以代次验证，不接受相同名字或复制 marker 代替原对象。
work 非空也拒绝分配恢复，避免把已有业务效果冒充空的初始化目录。
原 v1 意图保留单独兼容路径：完整 marker 和空 work 可精确重试，部分 marker 仍不能采用。
旧意图不回填 stage、不伪造创建收据，也不改写历史冻结配置。

## 明确弃置和命令重放

```text
wls owned-workspaces --db DB --request REQUEST
wls abandon-workspace --db DB --request REQUEST --resource RESOURCE \
    --expected-state-version VERSION --key KEY --reason REASON
```

第二个命令只转换该请求下未绑定的 PREPARED 记录，需要当前 Root、独占写者、原资源 CAS、
非空理由与精确命令键。CLI 先只读预检，无效参数不会接管写者；取得写者后重复授权检查。
弃置不访问文件系统，所以父目录消失、请求终态、团队退役或业务容量暂停都不会堵住这个出口。

`ABANDONED` 不可逆；保存原意图、已有收据、原 Root 身份和理由，`proof` 保持为空。
该动作不删除文件、不写 REMOVED、不生成不存在证明、不清 UNKNOWN、不恢复已暂停业务。
同一弃置键和完整参数重放返回原结果；改变理由、资源或 CAS 会冲突。
旧分配键始终指向原历史资源。ABANDONED 的原键重放只按原规范父路径核对参数，
不访问父目录；父目录缺失或替换也能返回旧元数据，错误父路径或任务仍拒绝。
新键和新资源 ID 才能在相同团队继续新分配。
已 OWNED、DELETING、REMOVED 的资源仍占该团队槽位，本增量没有已绑定工作区替换协议。

## 容量、历史和清理联动

| 项目 | 限制/规则 |
| --- | --- |
| 有效资源 | 每请求最多 64；非 ABANDONED 均计数 |
| 全部历史 | 每请求最多 256；弃置仍计入，窗口检查点不能重置 |
| v2 PREPARED，无收据 | 预留 2 个控制事件槽 |
| v2 PREPARED，已有收据 | 预留 1 个登记或弃置槽 |
| 迁移后的 v1 PREPARED | 预留 1 个登记或弃置槽 |

新分配在 mkdir 前完成普通准入和预留检查。收据/登记/弃置是已有分配的有限续进，沿现有
控制结算规则完成；业务 hold 或旧窗口到限不能在效果发生之后代替真实结算。
物理磁盘和 SQLite 写入失败仍原样失败，不能声称预留保证磁盘可用。
新增 `allocation_receipt`、`allocation_registered`、`allocation_abandoned` 事件归入控制池，
历史 prepare/register 的原事件分类及字节不变。容量输出用独立 `owned_allocation` 和
`reserved_owned_allocation` 表示终生计数与待结算尾部，不给旧 checkpoint totals 填造新字段。

`owned-workspaces` 展示全部历史；清理与业务绑定只选择有效资源。
弃置历史的 stage/final 两条路径仍参与工作目录绑定、交付根和嵌套分配的重叠保护，
相同团队也不能将其重新用作业务目录。原冻结清理 manifest 不扩展、不改写。
有效 OWNED 清理继续要求精确交付回执、经验结算、原进程退出证明和不可变库存。

`closeout.retained_allocations` 明确列出弃置记录、保留路径和理由，
`filesystem_observed:false`、`deletion_performed:false` 表示本次只是读取持久记录。
其他有效 work 删除完成仍只代表工作目录阶段；私有数据库行、交付、owner 收据与弃置残留继续保留，
全局 `cleanup_complete` 为 false。

## 数据库兼容和维护

新增 `010_owned_allocation_recovery.sql` 在短事务中把 407 升级为 408，保存原列的原值，
新增可空不可变收据和弃置证据，将团队唯一约束改为非 ABANDONED 的部分唯一索引。
原 schema 和 004–009 SQL 不变。CAS、原证据不可变触发器保留，并增加单向生命周期约束。

Store 构造函数在原 `StoreRecoveryScope` 之后追加可选 010 资产；原嵌入调用省略该资产时
继续使用 407。CLI 显式传入新资产。只读可以观察 407，不能隐式升级；有请求的旧数据库须先：

```text
wls init --db DB
```

这是全库维护，会按原契约恢复全部相关请求。OneRequest/AdmissionOnly 在结构升级和业务恢复前拒绝
带数据的旧库；正常请求命令只恢复目标请求。升级后的 408 必须提供哈希一致的 010 资产。
010 的 DDL、原行复制、元数据与版本号在同一迁移事务中；随后请求恢复属于另一个事务。
若升级已提交而后续恢复失败，408 结构可以保留，恢复中的业务行变化整体回滚；
不把结构升级与全部请求恢复描述成一个事务。

## 验证范围和后续

新增测试使用真实临时 SQLite、目录、跨进程 SIGKILL 和限定到夹具的系统调用故障注入。
固定模型脚本用于构造任务、交付与收尾，不代表真实模型决策质量；进程中断不等于整机掉电实验。
普通用户、双 umask、严格 GCC、Clang、ASan/LSan、UBSan 与原套件结果分别记录。

后续的 doctor 实际部署路径与有界阻塞诊断已由
[ADR-076](../adr/076-doctor-deployment-diagnostics.md)和[R2a.3契约](R2a-deployment-diagnostics.md)
单独承接；其只读观察不增加本契约的分配、采用或删除权限。
R3离线协议边界见[R3.1/R3.2契约](R3-offline-responses.md)，真实接入和长任务容量依赖见
[后继计划](R3-controlled-live-plan.md)。强沙箱部署正向、私有行 purge、后台服务和真实模型验收
仍按各自任务卡完成；本文件的原分配实现与验证范围不随后继计划扩展。
