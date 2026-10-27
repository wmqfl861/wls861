# 0.8.7-r3.3a 有界 HTTPS 组件验证

日期：2026-10-26。基线为 `8d3c36e3dfff1d64f911a89b79b107778f129807`，
蓝图仍为 v0.4.1，Store 仍为 408。本报告只记录本次执行；不改写旧报告或原 handoff。
实现契约见 [R3 HTTPS](../implementation/R3-https-transport.md)，取舍与后继 Store
边界见 [ADR-080](../adr/080-bounded-https-transport.md)。

## 1. 本次交付与证据范围

新增 `https_transport.hpp/.cpp` 受信宿主组件，接入 CMake、Make 和原六路 CI。
所有网络执行均为测试自行启动的 loopback HTTPS 服务，使用临时 CA/证书与合成 bearer。
`wls` CLI、Store、OfflineRuntime 仍为 fake-only；没有新增真实模型准入、凭据解析、
预算结算、工具派发或 live CLI。真实供应商调用与付费尝试均为 **0**。

构建、测试、配置、Schema、固定第三方依赖共 **262 项输入**与普通用户验证副本逐字匹配，
清单见 [r3-https-inputs.sha256](r3-https-inputs.sha256)，清单 SHA256 为
`3f9f72c90035c72576a1ddad65330317fe9d6883b93f2bd639210bfaee3aaa48`。
报告和日志在执行后追加，不改变这份运行输入。GitHub 发布通过原父提交快进，
发布后核对提交 tree 与本地暂存 tree；不覆盖未见远端变更。

## 2. 实际本地门禁

构建与执行使用 UID/GID **65534**。GCC 编译保留 C++20、`-Wall -Wextra -Wpedantic
-Wconversion -Wshadow -Werror`，没有放宽生产权限或测试断言。

| 检查 | 实际结果 | 范围 |
| --- | --- | --- |
| GCC Release，调用 umask 0022 | 64/64 CTest，通过；87.70 秒 | 全部原套件及新增 HTTPS 套件 |
| GCC Release，调用 umask 0002 | 64/64 CTest，通过；83.27 秒 | 同一源码的完整 CTest |
| GCC Debug ASan/LSan，umask 0002 | 1/1 HTTPS 套件，通过；11.33 秒 | 42 个 TLS 测试方法；`halt_on_error=1:detect_leaks=1`，无诊断 |
| Clang 18 严格独立编译 | 2/2 编译单元通过 | 生产 HTTPS 源码及测试驱动；不冒称本地全量 Clang 测试 |

完整 CTest 的 64 是套件数；新增 HTTPS 套件含 **42 个测试方法**，其中有状态码、
输入变体和容量等子用例，不能把套件、方法及子用例重复相加。ASan 重跑不增加独立场景数量。
原 C++ 夹具和部分 CLI 仍按既有规则设置自己的私有模式；调用 umask 不表示所有内部
创建动作均使用该 mask。HTTPS 夹具在普通用户下创建私有临时目录，并在测试结束关闭服务和连接。

环境：Ubuntu 24.04.3、GCC 13.3、Clang 18.1.3、CMake 3.28.3、Ninja 1.11.1、
Boost 1.83、C++ SQLite 3.45.1、OpenSSL 3.0.13、系统 libcurl 8.5.0。
Python 3.12.14、jsonschema 4.10.3；Python 自身链接的 SQLite 是 3.53.1，
不与 C++ Store 所链接的版本混同。libcurl 为 threaded resolver；无显式路由的
hostname 在组件联网前被拒绝，c-ares 外部 DNS 正向未在本环境执行。

## 3. 新增 TLS 场景

| 场景组 | 执行断言 |
| --- | --- |
| 正常传输 | JSON、原生工具、SSE、chunked、多字节 UTF-8 分块；原 tuple、编码/配置 hash、原 body 与上传量分别保留 |
| TLS 与地址 | 正确 CA/主机成功；不可信 CA、错主机失败；显式 hostname→IPv4 保留原名验证；无映射 threaded DNS 发送前拒绝 |
| 环境与重发 | 429/500/503 各只有一个请求；重定向目标零连接；代理和凭据引用环境不被使用；TLS key-log 环境在连接前拒绝且不创建文件 |
| 时间与取消 | TLS、头、体等待均有界；TLS 延迟加响应延迟共用一个 deadline；预取消零连接；服务端 marker 触发中途取消 |
| 完整性 | Content-Length 断流、非法 SSE 终态尾部、不完整 UTF-8、错误内容类型、压缩响应和冲突 HTTP framing 均不可采用 |
| 限额 | 单行头、累计头、JSON body、SSE stream、头体共享预算、事件和输出文本分别受限；较宽 profile 下小合法 SSE 不误拒绝 |
| 部分上传 | 合法约 900 KB 请求、小服务端接收窗口与固定有界客户端发送缓冲；分别断言 supplied、libcurl uploaded、服务端前缀和 deadline，均不称作计费证明 |
| 凭据和证据 | 凭据注入及改动原编码/请求绑定在联网前拒绝；服务端回显秘密只在私有内容，safe summary 不展开；篡改 raw/decoded/options hash 后 summary 拒绝 |

取消终态场景的准确范围是 **已收到 SSE terminal、HTTP 消息尚未结束**：服务端发送
terminal 后发 marker，并扣住 chunked 结束标记；原字节断言确认客户端确实观察到该 terminal。
该测试证明不会在 HTTP 尚未结束时提前释放工具，不声称采样覆盖最终完成线性化点两侧的所有竞态。
25 ms 是 poll 等待粒度，不是操作系统硬实时承诺。记录时间是执行器确认阶段的观察时间，
不是远端收包时刻；callback 字节也不是 TLS 抓包。

## 4. 开发过程与独立复核

生产实现、TLS 故障测试与契约复核分别完成，根代理整合后执行实际验证。复核发现并已修正：

1. `ASYNCHDNS` 可能只是 threaded resolver，cleanup 仍可阻塞。加入严格数字 IPv4、
   精确地址映射或实际 c-ares 后端门禁；没有把普通本地成功写成外部 DNS 通过。
2. 一轮 multi 调用可先完成 TLS/读响应、再让外层观察 TCP。prerequisite 内先确认 TCP，
   再记录 TLS，保留真实观察的先后关系；不填造精确网络时间。
3. 合法 profile 的上限可大于 SSE replay 的硬上限。接收与解码取有效交集，
   避免发送之后因主机调用参数越界而拒绝本来很小的合法响应。
4. 句柄关闭应包含在最终时间判断之前。当前网络路径显式关闭后才做协议校验和完成判断。
5. 关闭 verbose 不足以约束 `SSLKEYLOGFILE`。现在在 libcurl 初始化前拒绝非空设置，
   不打印其值、不更改宿主环境；可信宿主不得在检查后并发改环境或预缓存 key-log handler。
6. 服务端只读到前缀不能证明客户端只上传了前缀。加入统一有界发送缓冲，测试分别保留
   两端观察，未知结果始终不成为免费重试授权。

首轮 38 方法版及随后 40 方法版只作为开发过程结果；本节正式结果绑定最终 **42 方法**。
最终 42 方法在本地 Release/ASan 执行通过，源码与验证副本按上述清单核对。
一轮临时副本同步曾因普通用户所有权而失败，随后以授权宿主身份只同步该临时副本；
没有放宽工作仓库或系统目录权限。该次未更新副本上的旧协议通过不作为本批新增组件证据。
本节点是新组件，不把未曾执行的旧版本失败称为已复现红测。

## 5. 复现命令与原日志

以下命令在普通用户下执行，构建依赖按 README 安装；`python` 必须具备固定测试依赖。
构建并不自动联系供应商，TLS 测试只使用自建 loopback 服务。

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-release --parallel 2
umask 0022
ctest --test-dir build-release --parallel 2 --output-on-failure
umask 0002
ctest --test-dir build-release --parallel 2 --output-on-failure

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_ASAN=ON -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-asan --parallel 2 --target https_transport_driver
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-asan -R '^https_transport$' --output-on-failure
```

机器结果和原日志：

- [r3-https-local-results.json](r3-https-local-results.json)：输入身份、环境、计数、退出码、时间、日志 hash。
- [r3-https-release-0022.txt](r3-https-release-0022.txt)：完整 Release 0022。
- [r3-https-release-0002.txt](r3-https-release-0002.txt)：完整 Release 0002。
- [r3-https-asan.txt](r3-https-asan.txt)：ASan/LSan 专项 CTest。
- [r3-https-asan-detail.txt](r3-https-asan-detail.txt)：42 方法逐项原结果。

提交后的完整 strict、strict-umask-0002、UBSan、ASan、Make-ASan、Clang-18 和
source-snapshot 以**最终提交对应的 GitHub Actions**为准；本地专项不替代这些全量门禁，
也不在提交前预填远端成功。交付回复给出实际提交 SHA、run 链接及已观察结果。

## 6. 后继范围

R3.3b 尚需原 Store profile/attempt 预约、一次性发送权、原传输持久化、不可变结算与
追加用量对账。当前文档中的类型和事务顺序是实施约束，没有新增 SQL 或可执行 Store API。
R3.3c 供应商能力、小任务质量、费用和同模型单 Agent 对照仍需对应配置与明确授权。
管理角色上下文容量、大 owned 工作树容量、强 bwrap/cgroup 正向、daemon/API、private-row
purge 继续各自验收。本地 TLS 通过不代表这些能力已经完成。
