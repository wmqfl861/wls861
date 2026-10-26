# ADR-074：有界目录创建工具

日期：2026-10-25。状态：采纳；实现目标 0.8.3-r2a.1；蓝图 v0.4.1，R2a / S1-06。

## 聚焦范围

先独立接入 `fs_mkdir`，补足 Worker 无法在工作区创建输出子目录的缺口。
接口仅接受 `{ "path": "relative/directory" }`：父目录必须已存在，仅新建一个空叶目录，
固定权限 0700；已有目标一律不作为成功，不提供 parents、exist_ok、mode 或宿主身份字段。
这是业务工具，副作用类别 workspace；诊断入口不能调用。无需 SQL 迁移或新 CLI 子命令。

PREPARED 运行时所有权分配、原子 staging 发布、恢复收据和显式放弃属于 R2a.2。
它们涉及历史状态/唯一索引、弃置路径保护、清理目标选择及容量结算，另设 ADR 与验收。
本工具不创建或登记 runtime-owned envelope，也不使未归属目录取得清理资格。

## 文件效果与证据

复用当前 Workspace 的 UID/权限检查、openat2 限根、固定目录身份、mutex 和合作 flock。
预检路径、父目录、目标不存在、现有树和创建后条目数；目录组件数加一不得超过 max_depth，
保持 collect(".", 1) 的旧遍历定义和冻结 limits 不变。创建后整个树必须仍可被原工具遍历。

通过固定父目录 fd 的 mkdirat 创建；随后固定新目录 fd，验证目录身份，固定 0700，
同步新目录和父目录，再核根、父路径、新目录路径及 inode。完整成功才报告 Succeeded。
只读、路径、授权、配额或已有目标等在 mkdir 尝试前失败属于 EffectNotStarted。
mkdir 尝试后的任何失败，包括竞争 EEXIST、打开/权限/同步/最终身份检查失败，保守为 OutcomeUnknown。
证据记录原路径、根/父/新目录身份、所处阶段、mkdir_attempted、created 和同步标记。
异常时不盲目 rmdir，不根据目录存在或不存在自动认领、重试或核清 UNKNOWN。

不改进程全局 umask；常规 umask 0002/0022/0077 下最终目录权限均应为 0700。
合作锁仍不是任意同 UID 宿主攻击隔离；不扩大原工作区信任边界或强沙箱声明。

## 原运行时与兼容

继续走 WorkerTools 的冻结模板/request allowlist/当前撤销政策、完整版本和 ready 检查、
持久操作意图、首次 dispatch_claimed、业务额度、原结果结算和结算失败围栏。
同 key 同参数读回原事实，UNKNOWN 或已成功记录都不再次 mkdir；同 key 改参数仍冲突。
AMEND、取消、接管后的迟到事实保持原 binding，不能自动变成当前授权或清除 UNKNOWN。

工具使用独立 `wls-fs-mkdir-v1` schema，严格字段校验；新 general 模板显式授予它。
不改共享 `wls-worker-tools-v1` policy 版本，不补写旧模板、allowlist 或快照；旧请求继续不具备新 grant。
Store 的静态副作用分类和配置加载器工具名表必须与 registry 同步，执行分支不得落入 run 分支。

## 验收

覆盖成功创建后文件写入、已有目标/缺父目录、链接/路径/权限/只读、投影条目与深度预算、
创建前后与目录同步故障、根/父/目标替换、精确重放与首次派发、UNKNOWN/AMEND/配额/结算失败，
以及从新冻结配置到 CLI Fake Worker 的真实 SQLite/文件链路和旧请求授权兼容。
测试私有 syscall 注入，不向生产 API 增加测试回调；不把模拟故障说成物理掉电证明。
按 AGENTS 保留普通用户、双 umask、严格编译、ASan/LSan、UBSan、Make 和 Clang 验证，
逐项记录实际结果与限制。真实模型、PREPARED 分配恢复和 doctor 部署正向验收单独推进。
