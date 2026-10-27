# 运行资源、迁移注册表与安装契约

日期：2026-10-26（America/Los_Angeles）。基线：
`1c572d3aa9121e85cf22aa1e4bb330b25a34a143`。决策见
[ADR-086](../adr/086-runtime-resources-and-migration-registry.md)。
本模块保持 Store 410，不新增 SQL 迁移。

## 1. 资源与请求配置的身份

运行程序认可的资源由 `src/runtime_resource_catalog.inc` 固定，源码分发清单是
`resources/runtime-manifest.json`。当前资源为 35 份、160549 字节：

| 类别 | 内容 | 消费者 |
| --- | --- | --- |
| SQL | 原 `db/schema.sql`，原 004–012 迁移 | `MigrationRegistry` / 原 Store |
| Schema | 直接位于 `schemas` 的 21 份 `.schema.json` | 捕获字节的 `SchemaRegistry` |
| 公共提示词 | `common.md`、`dispatcher.md`、`team_lead.md`、`worker.md` | 新请求默认配置冻结与核对 |

Manifest 的 format 为 `wls-runtime-resources-v1`，Store 版本为 410。顶层只能包含
`format`、`store_version` 和 `assets`；每个按路径排序的 asset 只能包含
`path`、`kind`、`sha256` 和 `size_bytes`。整个对象必须等于编译认可的对象，JSON
重复键在解析阶段拒绝。修改资源后同时修改旁边的清单，不能使旧程序接受新字节。

`bundle_sha256` 是认可清单的规范 JSON hash，源目录、安装位置和清单排版不进入它。
`manifest_file_sha256` 单独报告实际清单原字节身份。资源诊断仅在完整捕获成功后返回
`complete=true`。外部配置的 `snapshot_sha256` 与这些程序身份分开；安装路径变化
不改变同一组提示词/功能/系统配置的冻结语义，已有请求继续读取原 Store 快照。

配置示例、供应商 profile、端点、认证值、用户请求、测试数据、SQL 示例和 handoff
都不在运行 bundle 中。程序资源校验不授予网络、业务工具或模型支出权限。

## 2. 读取边界与捕获 API

`RuntimeResources::from_source_tree(root)` 读源码布局，
`RuntimeResources::from_bundle(root)` 读安装布局。两者都完整捕获同一组原字节。
资源根必须是普通目录组成的路径；根、父目录和叶文件中的符号链接一律拒绝。

| 上限 | 数值 |
| --- | --- |
| 输入根/可执行路径 | 4096 字节 |
| 注册相对资源路径 | 512 字节 |
| 清单原字节 | 64 KiB |
| 每份资源 | 1 MiB |
| 全部资源 | 8 MiB |
| 编译注册数量 | 128 |

先用 `O_PATH | O_NOFOLLOW` 打开并检查类型和大小；确认是普通文件后，才从
`/proc/self/fd` 打开已固定对象的可读描述符。读取最多上限加一字节，并要求实际 EOF、
实际大小与原观察一致。读取前后比较设备、inode、类型/模式、大小、mtime 和 ctime，
再检查编译大小和 SHA-256。拒绝 FIFO 不需要等待写者，也不先打开设备的数据通道。

`bytes(relative_path)` 仅返回注册文件；`schema_documents()` 和 `prompt_documents()`
返回文件名到原文本的映射。消费者不能在校验后另行打开原路径替代这些字节。
`SchemaRegistry(map<string,string>)` 使用原 Schema 编译、引用和投影规则，保留旧目录构造入口。

默认提示词属于配置加载过程的一部分。CLI 冻结配置后调用
`validate_frozen_prompts(frozen)`，要求四份默认依赖各出现一次且内容/hash 等于捕获资源。
因此冻结期间改动默认提示词会被拒绝。显式 `--prompts-dir` 允许调用方提供另一组
外部提示词；它们仍通过原有安全加载和冻结过程，但不冒充分发默认资源。

## 3. 迁移注册与原 Store 边界

`registered_store_migrations()` 返回不可修改的注册视图；`MigrationRegistry(resources)`
默认选择全部原迁移。可显式选择 402–410 的连续前缀，用于旧版本兼容和升级验证。
不能跳步、重排、提供未知目标版本或由目录中额外文件增加迁移。

| 原版本 → 新版本 | 原 SQL | 持久元数据表 / 元数据版本 |
| --- | --- | --- |
| 401 → 402 | `004_s1_store.sql` | `store_runtime_metadata` / 4 |
| 402 → 403 | `005_worker_tools.sql` | `worker_tools_metadata` / 5 |
| 403 → 404 | `006_event_runtime.sql` | `event_runtime_metadata` / 6 |
| 404 → 405 | `007_workflow_runtime.sql` | `workflow_runtime_metadata` / 7 |
| 405 → 406 | `008_acceptance_runtime.sql` | `acceptance_runtime_metadata` / 8 |
| 406 → 407 | `009_owned_cleanup.sql` | `owned_cleanup_metadata` / 9 |
| 407 → 408 | `010_owned_allocation_recovery.sql` | `owned_allocation_metadata` / 10 |
| 408 → 409 | `011_live_model.sql` | `live_model_metadata` / 11 |
| 409 → 410 | `012_usage_reconciliation.sql` | `usage_reconciliation_metadata` / 12 |

注册表另行固定 base 和每份迁移的 SHA-256，不只信任资源清单。对已选择前缀之外但
已知的后缀版本，`migration_sql(version)` 返回空 view，沿用原构造接口的可选后缀语义。

```cpp
const auto resources = RuntimeResources::from_bundle(resource_root);
const MigrationRegistry migrations(resources);
Store store(database_path, migrations, StoreOpenMode::Takeover,
            StoreRecoveryScope{StoreRecoveryScope::Kind::AllRequests, {}});
```

以上示例中的全库维护范围应由宿主对应的显式维护命令选择。普通请求写入必须继续
使用原 `admission()` 或 `request(id)` 范围，不能因为新构造入口存在就扩成全库接管。
CLI `init` 保留显式全库维护入口；只读连接不隐式升级，限定范围连接不能升级已有请求的库。

新入口直接委托原迁移构造流程，保留数据库安全打开、单写者文件锁、每步事务、原
`user_version`/元数据 hash 检查、WAL、FULL、外键及随后独立的请求恢复事务。某一步
失败只回滚该步，已完成步骤保留；未知数据库版本不执行迁移。

原十三参数可信 SQL 构造接口完整保留。它用于已有宿主调用方和故障注入回归，没有
新增全局 compiled-hash 限制；其不可信模型输入边界也没有改变。新 CLI 使用注册入口，
不能借文件路径、清单字段或模型文字获得原可信 SQL API 的能力。

## 4. 安装布局与发现

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-release --parallel 2
cmake --install build-release --component wls-runtime --prefix /chosen/prefix
```

Staging 可以独立指定目标根：

```sh
DESTDIR=/temporary/stage cmake --install build-release \
  --component wls-runtime --prefix /opt/wls861
```

结果为 staging 根下的 `opt/wls861/bin/{wls,wls861}` 和
`opt/wls861/share/wls861/{runtime-manifest.json,db,schemas,prompts}`。
`wls861` 保留原 M1 内核 CLI；`wls` 是消费本运行资源的 S1 CLI。安装需要 Python 3
标准库和 Linux 文件描述符语义，安装后的运行程序不依赖 Python 或安装来源目录。

安装模块在 CMake configure 时复制一份构建清单。安装器对该冻结清单、源码注册表和
全部资产完成有界捕获及一致性验证后，才开始资源目标写入。目标路径逐段拒绝链接和
特殊文件，临时文件用独占创建的描述符写入并设 0444，fsync 后按文件原子替换并 fsync
父目录。清单最后发布，新目录遵守调用方 umask，不 chmod 已有目录。

这是逐文件原子发布，不是程序和全部资源的全局事务。若安装中断，后续加载的完整
编译身份检查会拒绝混合版本；修复方式是完成同一版本的安装。可信操作者管理安装
前缀及并发升级策略；安装器不提供抵御同 UID 目录搬移的独立沙箱。

`wls` 的选择顺序如下：

1. 显式 `--resource-root DIR` 使用安装布局。
2. 显式 `--source-root DIR` 使用源码布局，与前一选项互斥。
3. 否则从 `/proc/self/exe` 的实际位置尝试：`../share/wls861`、`share/wls861`、
   可执行文件目录的源码布局、其父目录的源码布局。首个存在清单必须通过完整验证。

第三项覆盖 `prefix/bin/wls`、源码下构建目录和显式打包构建资源。完全独立于源码的
外置 build 目录应安装资源或显式传根；不存在 cwd 或环境变量 fallback。通过 PATH
启动程序仍以实际可执行文件位置选择资源，设置 `WLS_RESOURCE_ROOT` 不改变选择。

`wls resources` 返回安全的资源和迁移诊断。安装模式的 `config check`、新请求提交等
需要当前配置的命令必须显式提供两个配置位置：

```sh
/chosen/prefix/bin/wls config check \
  --config /external/config/system.yaml \
  --functions-dir /external/config/functions
```

`--prompts-dir` 是可选的外部提示词覆盖。源码模式保留 `config/examples` 默认值；
资源加载本身从不读取这些可变配置。查询已有请求的冻结配置无需重新选择当前配置。
独立离线协议 encode/replay 入口保持原输入范围，不因安装模块获得 Store 或网络权限。

## 5. 验证入口及未声称完成的范围

`runtime_resources_tests` 覆盖资源/API/Store 的真实临时文件和 SQLite 行为。
`tests/runtime_resources_cli_tests.py WLS_BINARY SOURCE_ROOT [CMAKE_BUILD]` 必须以
普通 UID 执行：有构建目录时使用项目真实 CMake 安装；Make 入口使用同一原安装模块
和同一实际可执行文件建立轻量安装 fixture。两者均执行真实 prefix/DESTDIR 安装。

专项包括非源码 cwd、重定位、移除安装来源、外部配置、坏 bundle 在数据库/写者锁
前拒绝、FIFO 无阻塞、安装源全部验证后再写入、目的链接拒绝；兼容测试保留原
Store/Schema 回归。实际结果见专项报告，并由最终同 SHA 的完整 CI 再核对。

本模块完成程序资产和迁移入口的工程边界；服务管理、daemon/API、生产沙箱部署、
独立全系统断电验证、真实供应商质量/费用验收仍分别按其契约完成。
