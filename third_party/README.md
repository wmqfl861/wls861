# S1 基础层依赖与来源

本目录用于 S1 增量库 `wls_s1`。现有 M1 Journal 内核源码继续保留；S1 显式接入 SQLite、JSON、YAML 与 Schema 校验，不把历史 Journal 当成 SQLite 数据库迁移。依赖安装和编译不会启动模型、环境检查命令或系统服务。

## 固定导入的源码

| 依赖 | 固定版本与上游提交 | 导入范围 | 许可证 |
| --- | --- | --- | --- |
| LibYAML | [0.2.5，`2c891fc7a770e8ba2fec34fc6b545c672beb37e6`](https://github.com/yaml/libyaml/tree/2c891fc7a770e8ba2fec34fc6b545c672beb37e6) | 原始 `include/yaml.h` 与许可证；链接系统提供的 LibYAML 运行库 | [MIT 许可证](libyaml/License) |
| Valijson | [v1.0.6，`4edda758546436462da479bb8c8514f8a95c35ad`](https://github.com/tristanpenman/valijson/tree/4edda758546436462da479bb8c8514f8a95c35ad) | Boost.JSON adapter 所需的 26 个上游头文件与许可证，保留原始字节 | [BSD 2-Clause 许可证](valijson/LICENSE) |

Valijson 固定提交的上游树为 `685cda74f3934d2b5a3a944d9fcef4694842f02a`。其余 JSON adapter 和可选网络下载实现未导入。`SchemaRegistry` 的 `$ref` 解析限制在已注册的本地 Schema；配置与测试不通过网络补齐未知引用。

导入清单 [upstream_files.json](upstream_files.json) 记录以上 **29 个文件**的路径、字节数和上游 Git blob SHA-1。本批已经逐文件核对全部 29 项。Git blob 标识用于核对与固定上游文件的字节一致性；它不等同于对上游质量或所有依赖的安全保证。`README.md` 和清单本身是本项目维护的说明，不属于上游原件。

在仓库根目录可重新核对原件：

```sh
python3 - <<'PY'
from pathlib import Path
import hashlib
import json

entries = json.loads(Path("third_party/upstream_files.json").read_text())
for entry in entries:
    content = Path(entry["path"]).read_bytes()
    digest = hashlib.sha1(
        b"blob " + str(len(content)).encode() + b"\0" + content
    ).hexdigest()
    if len(content) != entry["bytes"] or digest != entry["blob_sha1"]:
        raise SystemExit("Upstream source mismatch: " + entry["path"])
print(f"PASS {len(entries)} upstream files")
PY
```

版本升级须显式调整固定提交、原件、清单和实际验证结果。构建入口没有 `FetchContent` 或隐式联网下载源码步骤。

## 系统开发依赖

| 依赖 | 构建要求及用途 | 本批工作区实际版本 |
| --- | --- | --- |
| Boost | **1.83 或以上**，提供 Boost.JSON 头文件；仅 `src/boost_json.cpp` 包含 `boost/json/src.hpp` 来编译其实现，不另外链接 `libboost_json` | Ubuntu `libboost1.83-dev 1.83.0-2.1ubuntu3.2` |
| SQLite | **3.37 或以上**，满足 `STRICT` 表需求；使用系统开发头和运行库，Store 验证 WAL、FULL 与外键设置 | `libsqlite3-dev 3.45.1-1ubuntu2.7` |
| OpenSSL Crypto | **3 或以上**，以 EVP 接口计算 SHA-256；当前用途不涉及 HTTPS 或模型网络调用 | `libssl-dev 3.0.13-0ubuntu3.15` |
| LibYAML | 本批使用 **0.2.5** 的系统运行库；上游对应头文件已固定导入，不需要构建时下载 | `libyaml-0-2 0.2.5-1build1` |

Boost 使用其 Boost Software License 1.0；OpenSSL 3 使用 Apache License 2.0；SQLite 原始头文件声明放弃版权。以上系统库及其许可证由安装的发行版软件包提供，没有复制进本目录。原始版权及许可证说明分别可见系统头文件、[Boost 官方仓库](https://github.com/boostorg/json)、[SQLite 官方源码](https://sqlite.org/src/)、[OpenSSL 官方仓库](https://github.com/openssl/openssl)。

CMake 使用 `find_package` 检查 Boost、SQLite、OpenSSL 最低版本，再定位系统 LibYAML。Make 在当前 Linux 环境链接 `-lsqlite3 -lcrypto -l:libyaml-0.so.2`。头文件导入只解决 LibYAML 开发头缺失，运行库仍须存在；整个工程不是完全静态、无依赖的单文件分发。

GitHub Actions 的 Ubuntu 24.04 构建安装 `libboost1.83-dev libsqlite3-dev libssl-dev libyaml-dev`。系统软件包按发行版仓库提供，因此没有声称与此工作区的补丁版本逐位相同；严格构建、UBSan 及实际测试结果验证对应环境。编译器、CMake、Python 契约测试依赖及完整运行命令见仓库 [README](../README.md) 和本批验证报告。

## YAML 与配置加载范围

配置通过 LibYAML 事件解析器读取，而不是自制 YAML 文本替换。加载器保留错误来源，拒绝重复键、未知键、非字符串映射键、anchor/alias/merge、多文档、显式标签和 YAML 指令，并限制文件字节、总字节、节点和深度。可能在 YAML 版本间产生歧义的 `off/on/yes/no` 必须加引号；布尔值使用 `true/false`。原始提示词、功能模板、环境模板和解析后的配置一起进入请求快照；相对依赖路径及内容哈希不包含检出目录。

环境模板中的 `check` 仅作为数据冻结，加载和恢复均不执行。此批只支持 `fake`、`adapter_version: "1"`、`protocol: fake-v1`；实际网络模型、付费重试、记忆运行器、非空 pinned Skill、额外挂载和真实文件清理尚未接入，相关启用配置明确拒绝。`reasoning` 支持继承、`provider_default`、明确 `"off"` 和已声明的 effort；token budget 思考模式及 `NORMAL` 耐久配置仍待后续实现。显式配置迁移 API 也没有因快照恢复完成而自动存在。

配置解析成功证明对应格式、字段、引用和政策检查通过，不证明环境已安装工具、沙箱已获得 cgroup 委派、模型能力已获供应商验证或真实业务已经验收。
