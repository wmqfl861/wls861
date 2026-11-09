# G1-5 实施验证记录（宿主 DNS、CURLOPT_RESOLVE 与显式代理）

状态：**IMPLEMENT: BLOCKED（产品 delta 超出 FINAL 计划预算）**。本页由实施
ZCode 会话按 `plan-final-v1.md` 的文件责任表记录实际状态与限制；不是执行通过的
验收报告。计划 SHA-256 `ab81c0be3882cf56e4f7ab5a35488272f0dbcbfd148936ffd76ab313143199c1`，
版本记录 `planner-record-final-v1.json` SHA-256
`8aed06deba1f9c30e6bda7b5871c1b34ccc1e96405b632540eb58ea609ce59cf`，
工作流 `f49ff366cacb15df96ade89e4aa3d4412cb50836e4f11f3be407713a844bf990`。
基线 main/HEAD `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631`（origin/main 一致，
本地 main ref 陈旧为 a6b8559，不作为度量基线）。

## 实际执行与停止点

1. 附件与门禁核验通过：三个清单附件、阶段输入及记录内 review/receipt/decisions
   哈希全部一致；作者/模型/推理档/FINAL/implementation_mode=yolo 均按记录成立。
2. 必读清单 61 个文件已由实施会话完整读取（另加 M1-contract.md）；工作区初始
   porcelain 干净，未做任何 Git 写入。
3. 完整写出计划文件责任表中的全部生产源码/构建接线/测试文件（见下）。
4. **早期门禁（计划验收序列第 2 步）失败并按计划立即停止**：
   `git diff --numstat 4987f2c -- src include CMakeLists.txt Makefile cmake`
   得 +561/−59；计划强制的新生产文件
   `src/https_resolver_internal.hpp`(49) + `src/https_resolver_main.cpp`(76) +
   `src/https_resolver_posix.cpp`(367) = 492 行。**产品 delta 合计 1112 行，
   超过计划 800 行上限约 39%。** 计划明确禁止以删安全校验或把生产代码放进
   测试凑数，压缩注释无法弥补约 310 行缺口，故不进入构建/测试步骤
   （构建、聚焦 CTest、78 项注册矩阵、ASan/UBSan 全部 **NOT_RUN**）。

结论：FINAL 计划要求的三个新生产组件（pinned helper ELF、父进程有界生命周期、
transport 代理/CONNECT 集成）与其 800 行单 PR 预算在当前设计下不相容；需计划
作者重设计（例如拆分为 resolver helper 与显式代理两个 PR，或显式提高预算）后
按新 FINAL 记录重新实施与硬审。

## 已写内容（未编译、未执行测试的实际状态）

- `src/https_resolver_internal.hpp`、`src/https_resolver_main.cpp`、
  `src/https_resolver_posix.cpp`：固定 WLS-RESOLVE-1 协议、仅链接系统
  resolver/标准库的 helper ELF、父进程 O_PATH|O_NOFOLLOW 逐段 pin、fork 前固定
  argv/env、child 仅 async-signal-safe 调用后 execveat、非阻塞管道 + 单一
  deadline + stop、EOF+exit0+reap 才接受、pidfd 优先 SIGKILL、25ms 有界清理窗、
  未 reap child 留在进程本地 slot 并阻止后续 live transport。
- `include/wls/https_transport.hpp`、`src/https_transport.cpp`：
  network/proxy typed options 与严格 grammar 校验（http/https authority、显式端口、
  字母起始域名或严格 IPv4、无 userinfo/path/legacy 数字；basic 仅 HTTPS proxy）、
  DNS 编排（映射/numeric 兼容且优先）、精确 CURLOPT_RESOLVE、代理
  CURLOPT_PROXY/HTTPPROXYTUNNEL/CONNECT_TO（CONNECT 目的行为数值地址，URL/Host/
  SNI/证书不变）、HTTPS proxy 独立 CA blob 与 TLS≥1.2、单次显式 Basic、header
  回调有界 CONNECT 子状态机（仅一个 200 块、1xx 中间块、407/3xx/截断即终）、
  dns_started/network_started/resolver_cleanup_pending 观察与私有 route evidence
  （safe summary 仅枚举/布尔/计数/时间/route hash，legacy summary-v1 字节不变）、
  共享 v1/v2 options descriptor（v1 缺省字节不变，v2 包冻结策略+CA hash+引用名，
  不含值）。
- `include/wls/live_model.hpp`、`src/live_model.cpp`：transport 可选 network 段
  解析/序列化、`live_transport_options_sha256` 改为委托同一共享算法、local_tls
  在 loopback-only resolver 策略下允许纯 hostname 端点（运行时逐地址强制
  127.0.0.1）、observe/execute 增加缺省空 secret 透传参数。
- `src/event_runtime.cpp`：在宿主授权、冻结快照与预算检查之后、reserve/claim/DNS
  之前，用同一受信 credentials resolver 与 RAII 短生命周期解析代理引用并做有界
  格式复查。
- `CMakeLists.txt`、`Makefile`：helper 可执行目标、wls 依赖其构建、
  `wls_install_runtime_resources(wls wls861 wls861-resolve-ipv4)` 同 bin 安装
  （非 SQL/schema/manifest 资产）、聚焦 ctest 命令附带受控故障夹具与真实 helper
  路径（CTest 注册数保持 78 不变）。
- 测试（不在 800 行预算内）：`tests/https_transport_fixture.cpp`（按 hostname
  标注的受控故障 helper）、driver 网络/代理/探针扩展、`test_https_transport.py`
  新旧策略对照、受控故障、deadline/取消、代理正反例与环境免疫、
  `live_model_store_tests.cpp` v2 hash/claim/准入负例、
  `test_live_workflow_cli.py` 代理引用解析与作用域、
  `runtime_resources_cli_tests.py` 安装布局断言。

以上均为**未验证源码**：没有编译、没有本地或远端 CTest、没有 sanitizer、没有
旧/新能力对照执行、没有安装实测。任何条目都不构成验收证据。

## 证据分类

- 本地合成夹具（TLS peer、CONNECT proxy、受控故障 resolver）：**NOT_RUN**。
- 契约参考/Store 回归：**NOT_RUN**。
- 真实系统 localhost libcurl 集成：**NOT_RUN**。
- 远端矩阵（三核心配置 + source）：**NOT_RUN**。
- Windows 11：**NOT_RUN**；私有 CI 证据托管：**BLOCKED**；外部 DNS/供应商/中转：
  **NOT_RUN**（无任何真实模型调用、无付费请求、无生产数据操作）。
- Git 写入/提交/推送/PR：**0**（全部改动保留在 worktree 未跟踪/未暂存状态）。

## 磁盘与环境

/mnt/d 仅约 1.3 GiB 可用（低于计划预期的 ≥512 MiB 余量要求的同时也非常紧张）；
未创建任何构建目录、未删除旧构建/日志。系统 curl/libcurl 8.5.0（threaded
resolver）与 python3 3.12.3(jsonschema) 只读核验过，未用于产品验证。
