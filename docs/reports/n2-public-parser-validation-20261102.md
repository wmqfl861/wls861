# N2 公开证据解析 guard 独立复核修正

本记录追加于 `97cf68a1ce92823be303a2246ec95da85e5d0245` 的非作者复核之后，
代码基于收尾修正 `47451537f1d7cc8d19a953777eb072fe2171ad68`。
本次修改 runner 公开 guard、Python 工具自检及当前契约；不修改产品源码、编译选项、
sanitizer 配置、CTest 注册/期限或 CI 上传路径。97 的原报告与所有反例保持原字节。

## 缺口与实现

旧 guard 使用默认 `json.loads`，后面的同名键会覆盖前值。虽然最终解析对象合法，
被忽略的前值仍留在实际上传的 JSON 字节中。旧检查也会把 NaN/Infinity/-Infinity 和
`1e999` 解析后的无限值视为允许的 float。公开索引、身份清单和所有结构投影现统一
通过严格解析函数：递归拒绝重复键、非有限常量和浮点溢出；原字段/hash/range 检查保留。

默认 ElementTree 丢弃注释、处理指令和 DTD，导致只查 DOM 时遗漏原始 XML 中的
额外文本。新 guard 要求原字节等于本入口生成器的精确序列化，再执行既有结构/text/
属性限制。注释、PI、DTD 和其他被解析规范化/丢弃的语法均无法借合法 DOM 放行。
这限定了本入口自己的 JUnit 投影格式，不改变原始私有 JUnit。

这是对合成篡改输入的防线修复；不主张规范生成器曾实际输出真实秘密。guard 失败时
主 runner 退出 1、摘要 NOT_COMPLETE、正式 `public-evidence/` 不存在；pending 和
private 原件保留，不降级到 raw 上传。

## 真实验证

使用 `/mnt/d` 数据盘、UID/GID 1000、既有 Python 测试 venv，未运行任何 C++ 构建或
产品 sanitizer。证据根为
`/mnt/d/codex-workspace/wls861-n0-n4-20261102-0k9_2wlf/evidence/n2/`。
每次独立目录保留实际脚本/测试/目录输入、stdout/stderr、精确 argv/cwd、环境白名单、
起止时间、退出码和 hash 收据。

| 执行 | 证据目录 | 结果 |
| --- | --- | --- |
| 精确 97 runner，5 个新方法 | `runner-97-parser-red/` | exit 1；5 方法，19 个 assertion failures（含子例），1.120s |
| 修正 runner，完整工具自检 | `runner-parser-green01/` | exit 0；50/50 方法通过，4.783s |

五个新方法覆盖顶层索引重复键；summary/source identity/catalog 的嵌套重复键；
summary 和 catalog 内 NaN/Infinity/-Infinity/1e999；XML 根前/根内/根后注释、根前/
根内 PI 和 DTD；以及真实 publication guard 拒绝后保留私有原件、不发布正式目录。
篡改 artifact 后均重算公开 bytes/hash，避免只因旧 hash 不符而假红。所有样本均为
SYNTHETIC_PRIVATE。19 个子例失败不是 19 套独立测试，也不是产品 ASan 失败。

两边使用完全相同的新测试文件。旧版的 18 个直接 guard 断言均为预期 RuntimeError
没有抛出，主流程第 19 个断言为实际返回 0 而预期 1；没有启动/导入/夹具错误。
绿例于 2026-11-02 01:05:41 UTC 完成。

```sh
TMPDIR=<证据根>/tmp PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1 \
  /mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B tests/test_run_gcc_asan.py \
  GateContracts.test_public_package_guard_rejects_duplicate_index_key \
  GateContracts.test_public_package_guard_rejects_nested_duplicate_json_keys \
  GateContracts.test_public_package_guard_rejects_nonfinite_json_numbers \
  GateContracts.test_public_package_guard_rejects_discarded_xml_syntax \
  GateContracts.test_publication_guard_failure_keeps_originals_without_public_directory
```

完整绿例使用相同解释器和环境，去掉方法选择参数。

| 输入 | SHA-256 |
| --- | --- |
| 精确 97 runner | `c639df7bb5d1f0ad07795e64558976d15fd117ddab123365040c332009c990d2` |
| 修正 runner | `ea44f984a3837ada4c758ec6f8b3610050dfad39f05fa3cce0b322168742920b` |
| 两边相同最终测试文件 | `38eea0204057f7c4a327073255e16deae4a99f01005bfe89a28d706b05dac7c2` |
| 未变的 92 项注册目录 | `f8678cfa81b001191540b1372e0b83705e5c85fd5e148ad706d80250f82f08c6` |

收尾复核还指出 quarantine rename 自身若因文件系统错误失败，最终异常原因可能没有
写入私有回执；原 checkpoint 仍为 NOT_COMPLETE、外层 exit 1，但不能宣称隔离总能成功。
该边界将用后继独立小提交补充，不以此处 50/50 关闭它。SIGKILL 任意时刻的跨文件原子
保证不在本实现范围，必须核验真实进程/CI 终态。

本主题工具修正与自检完成。整体 LOCAL_VALIDATION_COMPLETE=false，
PUSH_COMPLETE=false，REMOTE_CI_COMPLETE=false。hosted runner 原件没有新私有持久
副本；NOT_PRESERVED_DURABLY 边界保持，不宣称合并或产品验收完成。
