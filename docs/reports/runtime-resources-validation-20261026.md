# 运行资源与迁移注册表专项验证

日期：2026-10-26（America/Los_Angeles）。基线提交：
`1c572d3aa9121e85cf22aa1e4bb330b25a34a143`；本模块保持 Store 410。
对应 [ADR-086](../adr/086-runtime-resources-and-migration-registry.md) 和
[实施契约](../implementation/runtime-resources-and-migrations.md)。

## 执行范围与环境

构建和所有测试均以 **UID/GID 65534、空附加组** 执行。源码先复制到独立的
`/tmp/wls-runtime-resources-validation/source`，仅给该临时副本赋予测试用户所有权；
没有放宽工作仓库、生产工作区或已有配置的权限。首次默认沙箱不能 chown 临时副本，
因此通过获批的宿主命令只改变该新副本所有者；实际构建/安装/数据库/FIFO 测试始终
以普通用户运行。

环境为 Linux、GCC 13.3.0、CMake/Ninja Release、Python 3.12.14
（`/tmp/wls861-r33a-venv/bin/python`）。构建使用 `-O3 -DNDEBUG -std=c++20`
以及 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`。
检测到 Boost 1.83.0、SQLite 3.45.1、OpenSSL 3.0.13、libcurl 8.5.0。

源文件、二进制和测试均为本地合成资料，没有实际供应商调用、付费尝试或生产数据。
本报告只证明模块专项；完整 Release、ASan/LSan、UBSan、Clang、完整 Make 和最终
推送 SHA 的 CI 由两模块联合验证报告记录，不能由此处的局部结果推导通过。

## 实际结果

| 执行项 | 调用环境 | 结果 |
| --- | --- | --- |
| `runtime_resources_tests` | 普通用户，外层 umask 0022 | 14 / 14 |
| 原 `store_tests` | 同上 | 31 / 31 |
| 原 `schema_tests` | 同上 | 12 / 12 |
| 真实项目 CMake prefix/DESTDIR 安装集成 | 普通用户，umask 0022 | 10 / 10，0.787 秒 |
| 上述三个 C++ suite | 普通用户，外层 umask 0002 | 57 / 57，3 / 3 CTest suite |
| 真实项目 CMake prefix/DESTDIR 安装集成 | 普通用户，umask 0002 | 10 / 10，0.813 秒 |
| Make 两参数安装脚本入口 | 普通用户，umask 0002 | 10 / 10，0.873 秒 |

C++ 公用 `test::run` 原本会把自己的临时 fixture umask 设为 0077，退出时恢复。
上表因此明确标为“外层” umask；Python 安装集成实际继承 0022 / 0002，并验证资源
文件没有任何写权限。没有将 C++ fixture 的私有模式冒充全部内部创建使用宽松 umask。

安装集成真的运行 `cmake --install --component wls-runtime --prefix /opt/wls-runtime-test`
并设置临时 `DESTDIR`，随后执行安装后的真实 `wls`。Make 两参数入口使用同一安装
模块及同一个真实二进制创建轻量 CMake 安装工程；它是 Make 调用的专项入口验证，
不是另一次完整 `make test`。

## 关键观察

- 编译资源集合为 **35 份、160549 字节**。源码模式与已安装模式得到相同 bundle 身份；
  安装文件为 0444，配置和测试目录未被捆绑。
- 真实 `init` 创建 Store 410；全部注册前缀 402–410 的原版本和持久迁移 hash 正确。
  已有 402 请求在 admission/request 范围下拒绝升级，全库维护才升级至 410；原快照、
  原请求文本和升级前只读事件保持。新入口仍阻止第二写者，允许只读观察。
- 原可信 SQL API 仍可执行故障注入：005 后追加错误语句会回滚该步，库保持 402，
  新增表不存在；随后注册表升级成功。原 SQL 文件完全未改。
- 资源清单遗漏、额外字段、重复项、重复 JSON 键、错误 hash、未知版本、缺文件和
  修改 SQL 后自报新 hash 都被拒绝。CLI 参数化八种故障在数据库与 `.writer-lock`
  创建之前结束；实际 FIFO 在 3 秒 subprocess 上限内拒绝，不等待写者。
- 新 Schema 字节入口与原目录入口的 schema 集合/内容一致，原 Schema 回归全部通过。
  完整资源捕获后删除其整个文件树，仍可从内存构造 Schema 和 Store。
- 移动已安装前缀至含空格的新位置后，从无关 cwd 可执行资源发现和 `init`。
  cwd 的伪清单/SQL 及 `WLS_RESOURCE_ROOT` / `WLS_SOURCE_ROOT` 环境变量没有改变选择。
- 另一独立资源源目录被实际安装后删除；安装程序继续执行资源诊断、Store 初始化和
  外部配置检查。该用例删除的是本次安装使用的完整临时来源，未移动共享测试源码。
- 已安装配置命令缺 `--config`、缺 `--functions-dir` 或二者皆缺时拒绝。
  显式外部提示词改变请求配置身份，但不改变 bundle 身份；提交后损坏当前外部配置，
  仍能读取原冻结请求快照。
- 安装器发现靠后的源资源 hash 错误或实际 FIFO 时，资源目标目录仍不存在。
  目标根为符号链接时拒绝，链接指向的目录保持空。没有通过提高权限使故障 fixture 通过。

## 本轮修正与只读复核

首轮严格编译发现新测试把 `boost::json::value` 与 `std::string` 直接比较。
修正为读取 JSON 字符串后比较，重新编译上述目标并执行全部列出测试；生产代码
没有为测试放宽约束。

安装代码 review 指出，新建目录之后再 chmod 可能作用于并发替换的另一个目录。
已删除这条 chmod 路径：目录按 mkdir(0755) 及调用方 umask 创建，所有既有目录
模式保持。仅在本进程独占创建的临时普通文件 fd 上设 0444。

对共享 CLI 的资源集成做了只读复核：原 Store mode/scope 沿用，新 Schema 使用捕获
字节，安装配置显式选择，默认提示词冻结后与捕获字节核对，协议 encode/replay
保持独立提前分支。未发现需要修改 CLI 的模块 B 问题。

## 重现与源码身份

普通用户在临时源码副本中执行：

```sh
cmake -S source -B source/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/tmp/wls861-r33a-venv/bin/python
cmake --build source/build-release \
  --target runtime_resources_tests schema_tests store_tests wls wls861 --parallel 2
ctest --test-dir source/build-release \
  -R '^(runtime_resources|schema|store)$' --output-on-failure
/tmp/wls861-r33a-venv/bin/python source/tests/runtime_resources_cli_tests.py \
  source/build-release/wls source source/build-release
/tmp/wls861-r33a-venv/bin/python source/tests/runtime_resources_cli_tests.py \
  source/build-release/wls source
```

执行后对比共享仓库与临时副本，15 份模块实现/API/测试/安装文件逐字一致，清单见
[专项源文件身份](runtime-resources-validation-source.json)。它不代表尚在并行开发的
另一模块或完整发布源码快照。

| 身份 | SHA-256 |
| --- | --- |
| 15 文件记录规范 JSON | `af81c64c46cc1de9fc1471bcd4c6ec2f6196be8e5997062190d3156eb59fc76d` |
| 认可 bundle | `b330122ee40abaf4a3a88197f57d640e765015a70cb1be55d6d6023adccb74cf` |
| 原 manifest 文件 | `b9f5468c6858ebd467fb4e6905d2ef4b8bd51a006e4fd2a636f8e4db1fde55cf` |
| 专项 Release `wls` | `a4c01d23f08893944a221fdaa0da112502e25b868c318f4cf8083a99c0398354` |
| 专项 Release 资源测试二进制 | `72243816f3b5e6125e29f03accc4e97dd77071bf61adad3c7ffd0bf591345e7f` |

安装逐文件原子，manifest 最后发布；没有执行整机断电实验，也不声称整套 bundle
或整个 CMake 安装是事务。生产服务部署、实际模型质量和单 Agent 全五任务对照不属于
本模块的完成结论。
