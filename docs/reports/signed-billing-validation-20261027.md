# 受信中转签名账单模块专项验证

日期：2026-10-27（America/Los_Angeles）。基线为
`1b7020e3676a57da233aa097d32956a80942734e`，Store 410。
接口决定见 [ADR-088](../adr/088-signed-billing-adapter.md)，格式见
[R3-signed-billing](../implementation/R3-signed-billing.md)。

## 实际范围与输入

本专项使用真实临时 SQLite、原预约/唯一发送 claim/恢复/原结算/投影和真实 Ed25519
签名验签。签发密钥是测试专用合成 seed；晚到收据属于 local_tls_fixture 信任域。
未将这些合成账目称为真实中转账单，也未运行真实供应商、付费调用或生产数据操作。
完整 HTTP/TLS、CLI 连续流程、header/trailer和两个条件的固定任务验收另由联合测试记录。

测试以冻结副本运行。其299项源码、依赖、配置、SQL、Schema和提示词输入清单为
[signed-billing-inputs-20261027.sha256](signed-billing-inputs-20261027.sha256)，清单SHA-256：

`399fb62aaa3fd5c71a79ba03404e3153af4e0ae980f6677c6738c7b75c3ff621`

机器记录及本模块11项关键文件hash见
[signed-billing-validation-source-20261027.json](signed-billing-validation-source-20261027.json)。
记录时这11项与工作树逐字一致。副本中同时存在正在联合开发的其他模块；本报告只断言
下面实际执行的专项，最终整树普通用户/umask/完整CI必须使用最终交付输入另行核验。

## 已执行结果

构建和测试均为普通 **UID/GID65534、空附加组**。外层umask0002；原native
`test::run`夹具内部显式0077并在结束后恢复，不能把外层值冒称为所有夹具创建时的值。
严格C++20及 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` 保持。

| 检查 | 实际结果 | 实际耗时 |
| --- | --- | --- |
| GCC Release：billing | 12/12方法通过 | 1.08秒 |
| GCC Release：原live_model_store | 15/15方法通过 | 1.23秒 |
| GCC Release：原protocol_codec | 30/30方法通过 | 0.04秒 |
| Release三套件联合 | 3/3，0失败 | CTest1.23秒；外部进程1.244秒 |
| GCC Debug ASan/LSan：billing | 12/12方法通过 | 8.00秒 |
| GCC Debug ASan/LSan：原live_model_store | 15/15方法通过 | 9.09秒 |
| GCC Debug ASan/LSan：原protocol_codec | 30/30方法通过 | 1.05秒 |
| ASan/LSan三套件联合 | 3/3，0失败，无诊断 | CTest9.09秒；外部进程9.100秒 |
| Clang18关键编译单元 | 6/6严格syntax-only通过，输出为空 | 14.925秒 |

ASan使用 `halt_on_error=1:detect_leaks=1`，没有关闭泄漏检查或放宽原套件期限。
Clang的六项为billing、provider_profile、protocol_codec、https_transport、store及
billing_tests；这不是完整Clang运行成绩。套件数、方法数和不同构建的重复执行不相加。

实际工具为GCC13.3.0、Clang18.1.3、CMake3.28.3、Ninja1.11.1、OpenSSL3.0.13；
测试Python3.12.14；CMake探测的C++ SQLite3.45.1、libcurl8.5.0。
原输出见[Release日志](signed-billing-release-20261027.txt)及
[ASan日志](signed-billing-asan-20261027.txt)。归档仅移除每行末尾空白，原始和归档hash
均在机器记录中；没有删除失败行或重写测试结果。

## 12组新回归覆盖

1. 旧provider profile序列化与旧编码报告保持；原公开聚合初始化仍可严格编译；相同wire
   body的不同原attempt产生不同challenge；更改challenge不能保留原record指纹。
2. 信任政策缺失/多余字段、公钥格式、完整profile hash、配置hash、额外模型和未冻结
   challenge的旧profile被拒；有效政策只读通过。
3. 合成签名晚到账单经过原恢复/结算后填费用；原响应缺失仍缺失、原nullable结算字节
   不变、accepted不恢复、零工具反馈，账本可重建且完整后原ready门禁正常。
4. 错私钥、改金额、全零签名和未授权key拒绝，零新reconciliation。
5. 错issuer/account/domain、错误单位、负值、遗漏cache维度和模型自报verified拒绝。
6. 其他attempt的challenge、其他原body hash和错误精确模型标识拒绝。
7. receipt ID归属跨重启持久；签发密钥轮换和revision改变不能把原收据用到另一attempt。
8. 同一Store的兄弟请求不能认领原收据；原请求预算保持，错误请求政策不能读取映射。
9. 同receipt的后续修订可补齐null维度；矛盾值保持原已知金额、记录disputed并阻止继续。
10. 对账event提交故障时，费用投影、预算和来源归属一同回滚；修复故障后可按原事实提交。
11. 只读精确重放跨writer epoch返回原结果，零新请求/预算变化；更改CAS/key/Root身份拒绝。
12. 无原send claim或原观察证明network未开始时，独立签名也不能记为该次收费；原无header
    的pre-cancel观察通过同步hook返回无收据，保留原中断与未知账目。

独立审核还推动了两个边界修正：完整HTTP失败体可能没有Responses response ID，
因此签名字段允许显式null，完整body仍必须匹配，实际观察到的ID仍必须相同；同步原header
为空时返回无收据，避免为连接/取消失败另造一个billing格式错误。这些边界的实际TLS/CLI
覆盖由联合测试报告给出，本native报告不冒称完成网络集成。

## 开发过程与复现

首次Store重放辅助的严格syntax检查发现未定义的 `obj` 助手，以及Boost.JSON value和
std::string直接比较导致编译失败，随后改为正确的 `as_object()` 和已有字符串读取助手。
没有放宽权限、账目、身份或签名规则。较早候选Release3/3成功后，又将新可选字段放到
公开结构末尾并给出默认初始化，加入旧聚合初始化回归；上表均为这一最终专项输入。

以普通用户、具备仓库固定测试依赖的Python复现：

```sh
cmake -S . -B build-billing-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-billing-release --parallel 2 \
  --target billing_tests live_model_store_tests protocol_codec_tests
ctest --test-dir build-billing-release --parallel 2 --output-on-failure -V \
  -R '^(billing|live_model_store|protocol_codec)$'

cmake -S . -B build-billing-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWLS_ENABLE_ASAN=ON -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-billing-asan --parallel 2 \
  --target billing_tests live_model_store_tests protocol_codec_tests
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-billing-asan \
  --parallel 2 --output-on-failure -V -R '^(billing|live_model_store|protocol_codec)$'
```

本模块没有生产签发私钥或默认可信供应商，没有跨独立数据库的全局receipt去重服务，
不会自动生成缺失的原settlement，也不恢复旧响应资格。真实签发端、真实账单来源、
实际供应商费用/质量和强部署隔离仍须独立验收。
