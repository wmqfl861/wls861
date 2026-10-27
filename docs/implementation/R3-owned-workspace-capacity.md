# R3：owned 工作区贯通容量契约

依据 [ADR-084](../adr/084-owned-workspace-capacity.md)，接续
[S1 owned 清理](S1-owned-cleanup.md)及 [R2a 分配恢复](R2a-allocation-recovery.md)。
本契约实现后继计划第 6 节选择的单清单容量方案；实际命令和结果见
[节点验证报告](../reports/r3-owned-capacity-validation.md)。

## 1. 新请求选择与旧请求兼容

在现有 `system.cleanup` 对象中可增加以下字段，其余原必填配置仍须保留：

```yaml
owned_workspace_capacity:
  version: owned-capacity-v1
  max_entries: 4095
  max_total_bytes: 251658240
  max_inventory_bytes: 8388608
```

该对象须恰有以上四个字段。`max_entries` 是 1–4095 的整数，`max_total_bytes` 是
1–251658240 的整数，`max_inventory_bytes` 是 262144–8388608 的整数；只允许在这些
范围内缩小。缺省不补字段，不选择 v1；显式 null、未知版本、附加字段和超限均拒绝。
`cleanup.enabled` 的独立门禁不变，容量策略本身不启用删除。

新配置只作用于后续冻结的新请求。现有请求从原 `config_snapshot` 恢复；原分配从原 intent
恢复；原库存从持久化原字节恢复。旧 Workspace 默认仍是 20000 项、256 MiB，总清理边界仍
是 4096 项、256 MiB、8 MiB 编码库存。历史 execution_policy 不填入新的字段。

## 2. 分配和执行策略

`prepare_owned_workspace` 从原请求快照取得容量，写入意图的 `capacity_policy`，并在 marker
中写入 `capacity_policy_sha256`。这一动作在 mkdir 之前完成。v2 创建收据原有的意图 hash、
marker hash 以及完整目录代次一起绑定策略；恢复时不能换策略、复制收据或采用另一目录。
策略字段与 marker hash 必须成对存在，移除一侧也拒绝。

WorkerTools 绑定时要求同团队已有完整 runtime-owned 分配，原策略与请求快照一致，目录
path/device/inode 与分配相同。将实际 WorkspaceLimits 绑定到该策略后，才写入或验证团队
execution_policy。同一 Workspace 实例再次绑定只能完全相同；后续工具使用再次校验实际
限制、原分配和持久执行策略。只读工具实例同样先绑定冻结限制。

新 execution_policy 在旧字段之外记录：

- `owned_workspace_capacity`：原四字段对象。
- `business_effect_scope: file-tools-only`。
- workspace limits 中的 `max_inventory_bytes`、`inventory_root_budget_bytes` 和
  `inventory_stage_budget_bytes`。

新策略不声明 `run`。若受信调用者直接传入该工具名，WorkerTools 在操作 PREPARED 或任何
进程效果前拒绝；不存在“先运行，再判断是否超额”的路径。控制、诊断和原操作的无效果重放
继续遵守既有门禁。没有此策略的旧请求保留原进程工具政策。

## 3. 写入前容量闭合

业务条目计数包括文件和目录，不包括 `work` 根本身。业务总字节是树内所有普通文件长度之和。
单文件仍最多 16 MiB，Worker 参数单段仍最多 65536 bytes；本策略不提高任何文件工具限额。

`fs_mkdir`、`fs_write`、`fs_patch` 在原根锁和描述符解析边界内检查完整当前树，再校验新增项、
最终逻辑总字节和库存编码投影。预检超限返回 `EffectNotStarted`，不创建目录、stage 或目标文件。
WorkerTools 将它持久结算为明确 FAILED；真实执行后持久化失败仍保留原 UNKNOWN/暂停机制。

原子替换在 rename 前会同时保留旧文件和一份 stage，因此保留如下空间：

| 维度 | 业务上限外的预留 | 原清理硬上限 |
| --- | ---: | ---: |
| 条目 | 1 | 4096 |
| 内容字节 | 16 MiB | 256 MiB |
| 库存根证据 | 131072 bytes | 包含在配置的库存编码限额内 |
| 每条目编码 | `canonical_json(path).size() + 1024` | 包含在配置的库存编码限额内 |
| stage 编码 | `6 × 4096 + 1026 = 25602` bytes | 包含在配置的库存编码限额内 |

128 KiB 根预留覆盖资源 ID、原策略和三个至多 4096 bytes 的绝对根路径及固定代次字段。
每项 1024 bytes 预留覆盖类型、模式、文件大小、完整固定代次、哈希和 JSON 标点；路径按实际
JSON 转义计费，因此控制字符不会被当作单字节库存。库存生成仍检查实际表示是否满足各项
上界、完整 JSON 是否在限额内，不以预估替代最终证据。

新增文件保守同时预留其最终记录与一次 stage 记录；替换不增加最终记录，但仍须保留 stage。
业务目标的父路径还必须容纳完整 stage 名称。崩溃残留的 stage 是下次完整遍历中的普通条目，
不能通过保留名称逃避计数。若已占用了预留空间，进一步业务写入会在效果前拒绝；清理仍能
捕获原所有权内的完整树，并保存包括该 stage 在内的库存。

这些是受控文件工具容量，不是任意进程或同 UID 宿主的操作系统磁盘配额。

## 4. 原库存、删除和超限出口

新策略库存增加一个 `capacity_policy` 字段，其值须与分配 intent 完全一致。Store 的首次
库存事务校验策略、原身份、条目数量、总内容字节和实际规范 JSON 大小。所有资源库存持久化
成功前不 unlink；事务失败完整保留树。

Linux 清理入口从原 intent 派生 `max_entries + 1`、`max_total_bytes + 16 MiB`，再与调用者
给出的更小限制取交集；不得超过既有 4096/256 MiB/16 MiB/32 层硬界。删除每轮仍预检全部
尚存项；新增项、同字节新 inode、原哈希或模式变化、链接或跨挂载均拒绝。一次 pass 的计数
仍包括删除 `work` 根，默认 1024、最大 4096；4096 项的中断树需额外 pass 删除根。

原库存一经提交不扩展、不重采样、不分片。清理重启继续同一完整库存和 hash，缺失项只能
依据已持久化的原授权恢复，原 claim 的代次/CAS 围栏不变。更改外部配置不能使旧库存获得
更大额度，也不能让新的更小额度阻断原库存的精确重放。

首次库存生成因条目、内容字节或编码容量不足而失败时，运行器在领取清理权之前返回：

```json
{
  "capacity_exit": {
    "status": "RETAINED_CAPACITY_EXCEEDED",
    "resource_id": "原资源 ID",
    "original_capacity_policy": null,
    "reason": "本次库存观测的容量原因",
    "inventory_committed_this_pass": false,
    "deletion_performed_this_pass": false,
    "host_action_required": "retain_or_explicitly_handle_outside_automated_cleanup"
  },
  "pass_removed_entries": 0,
  "workspace_absence_verified_now": false
}
```

示例 null 表示历史策略；v1 资源返回原四字段对象。实际返回还保留正常 closeout 状态，并
增加容量 blocker。`this_pass` 只说明本次未执行新清理效果，不否定更早持久检查点或中断效果。
只读 `closeout` 本身不扫描文件系统；上述容量观察来自本次清理入口。

可信宿主应保留超限资源及原证据，另行明确安排人工处理或后继迁移。当前没有 force、自动
分片、静默删除额外文件、放大 cleanup limits 或将 `OWNED` 改为 `REMOVED` 的绕过路径。
失败/取消请求、未完成费用核对、原进程退出证据、精确交付与经验结算门禁继续独立生效。

## 5. 存储、验收和未包含范围

本节点不增加或修改 SQL。新增策略利用既有不可变快照、intent、allocation receipt、inventory
和 execution_policy 字段；407/408 的原字段解释保持。其他节点可能增加自身迁移，不能把它们
归为本容量策略所需的旧表重写。

测试分别覆盖真实目录的边界和真实 SQLite/WorkerTools 的贯通路径，含普通用户权限、SIGKILL
及限定到自建目录的故障。单机进程中断不等于掉电，也不代表任意同 UID 对手隔离。
大树分片、子进程磁盘配额、私有数据 purge、失败成果保留与强环境部署验收仍是独立工作。
