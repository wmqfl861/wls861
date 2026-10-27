# R3.3b 原 Store 与追加账本验证

日期：2026-10-27。开发基线：`56185a48303deb1c72b2ec53d896e8c0b32055f0`。
本轮实现版本0.9.0-r3.3b，Store410；本报告覆盖五节点中的原预约/发送及原结算/对账两项。
取舍见 [ADR-081](../adr/081-original-live-store-accounting.md)，API和容量见
[实施契约](../implementation/R3-original-store-live.md)。
最终源码身份、完整双umask门禁、ASan/LSan、日志hash及发布SHA的六路CI统一记在
[五节点总体报告](five-node-validation-20261027.md)，本报告不以旧提交CI替代新交付证据。

## 1. 证据范围

所有执行使用临时SQLite、普通UID65534、自建loopback TLS、临时CA和合成bearer。
provider kind的负例仅做开关拒绝或预取消零网络检查。真实供应商调用和付费尝试均为0。
新CLI/OfflineRuntime仍为Fake执行；本地TLS通过不能证明某实际端点、模型、计费来源或模型质量。

C++ `live_model_store_tests` 有15个测试方法。Python `live_model_store_tls` 有10个主方法；
包含429/500、4类在途绑定变化和5个实际SIGKILL子场景。套件、方法及子场景不相加，
不同umask或sanitizer重跑也不算新的独立场景。

## 2. 本轮执行记录

最终输入清单SHA256为`42b2f25aba2bc04b13777d9b5a5518beeb2a898e118eb37031a4e29a6c9cb59b`，
与普通用户副本相同。以下同时保留开发候选和最终运行，二者不混为同一源码证据。

| 运行输入 | 实际结果 | 解释 |
| --- | --- | --- |
| 早期13方法C++版，UID65534/umask0002 | 11通过，2失败 | 新夹具的逻辑信封hash及原编码完整性前置错误码；修复夹具，不改生产校验 |
| 集成14方法C++版，UID65534/umask0002 | 14/14通过；CTest1.18秒 | 已包含无claim恢复及host_no_dispatch；尚无最终容量边界方法 |
| 早期8方法TLS版，UID65534/umask0002 | 7通过，1错误；CTest2.05秒 | 测试查询不存在的operations表，改为真实ops表；原生产表不改 |
| 候选15方法C++版，UID65534/umask0022 | 15/15通过；CTest1.03秒 | 已含新live预发送容量拒绝 |
| 候选10方法TLS版，UID65534/umask0022 | 10/10通过；CTest2.68秒 | 已含合法大输出及实际容量超限；该候选只有4处SIGKILL边界 |
| 最终15方法C++版，UID65534/umask0022 | 15/15通过；CTest1.09秒 | 最终原Store、迁移、事务和账本反例 |
| 最终10方法TLS版，UID65534/umask0022 | 10/10通过；CTest2.78秒 | 已包含实际网络等待期间的第5处SIGKILL，原服务未收到任何重发 |
| 最终15方法C++，ASan/LSan、umask0002 | 15/15通过；CTest7.64秒 | 无ASan/LSan诊断；是本节点聚焦套件结果 |
| 最终10方法TLS，ASan/LSan、umask0002 | 10/10通过；CTest14.64秒 | 五处SIGKILL、实际TLS及容量边界均通过；不把受控SIGKILL当泄漏检查 |

早期集成日志、初次ASan发现的管理视图缺陷及修复前候选结果均由总体报告保留。
修复后的最终Release0022完整门禁为70/70、110.97秒；其中本节点两套的原日志段落见
[r3-original-store-local-detail.txt](r3-original-store-local-detail.txt)。此文件逐字摘录最终
CTest两套结果，包含命令、UID/umask、全部方法名和时间，SHA256：
`2c8cf572ac73cf096125786bdc46f2b852366cd718fd63e478f995975ce94c25`。完整双umask和全部sanitizer结果继续以总体报告为准。

## 3. 原 Store C++ 反例

| 场景 | 断言范围 |
| --- | --- |
| 默认关闭与完整信封 | 无宿主授权、冻结开关false、伪装fake、零费用预留、合法重算但不同绑定均在原预算改变前拒绝 |
| 新live容量 | 明确3MiB累计等包络；更宽HTTP、stream、text或累计配置在发送前拒绝 |
| 原身份与唯一claim | 精确预约重放不再预留；篡改原body/options/CAS拒绝；两个并发领取者只有一个新发送权 |
| 原预约/claim原子性 | 真实SQLite触发器在新身份或event写入处失败，原attempt、预算及claim全部回滚 |
| 原传输和结算 | 未保存原transport不能直接结算；篡改原观察hash拒绝；观察/结算写入失败整体回滚；精确重放不改字节 |
| 部分用量 | 只释放新确认维度；同键精确重放不重复记账；异证据和旧projection CAS拒绝；补齐后原结算不变 |
| 零余额及cache未知 | input/output/cost明确0不清cache null；仍阻止新paid预约和要求完整账目的收尾 |
| 矛盾与超预留 | 矛盾不覆盖旧值；完整实际超预留支出照实保存、保留告警并停止新paid准入 |
| 核对原子性及私有来源 | projection/预算/追加记录/event同事务；失败不留部分更新；安全查询不展开私有source bytes |
| 有claim恢复 | 当前writer不能自称丢失；前一writer UNKNOWN可宿主中断结算；迟到transport不替换原结算、不重发 |
| 无claim恢复 | 使用独立reason及原writer边界；原结算全null；只有匹配无claim标记的完整五维0证明可追加核对 |
| 终态与请求范围 | 当前Root可对终态原attempt核对；不恢复业务；OneRequest不能访问兄弟，兄弟原request/预算不变；ReadOnly拒绝写 |
| 旧408兼容 | OneRequest拒绝隐式升级；AllRequests显式升410；原Fake快照/attempt/usage/结算/预算保持；旧资产不能误读新结构 |
| 当前政策与身份 | 首次发送前再核政策/跨请求身份；失败零claim |
| 证据来源及角色 | 缺宿主verifier、错误Root、错误settlement hash、把loopback证据用于provider均拒绝 |

所有预算检查通过`verify_usage_ledger`从原预约、原结算和逐条核对重建，不只比较最终余额。
SQL不可变触发器和CAS也有直接反例；不删除审计、不放开工具UNKNOWN、不覆盖原usage/null。

## 4. 原 Store + 本地TLS反例

| 场景 | 断言范围 |
| --- | --- |
| 实际HTTP完整路径 | 原Store预约→唯一claim→真实loopback TLS→私有原观察→原结算；同调用重放只发一次；原nullable cost和后到核对分开 |
| SSE原生工具终态 | 保存原function_call；原Store accepted与工具派发分开，ops表仍为0 |
| 私有回显 | 服务端回显合成bearer的header/body精确保留；safe查询和transport event不展开它们 |
| HTTP429/500 | 每个错误都只发一次；保留未知预留；没有自动付费重试 |
| 半响应deadline | 原前缀持久化、工具为空、accepted=0、费用未知预留保持 |
| 预约前拒绝 | 冻结关闭/SQLite预约故障均零socket接收、零attempt、零claim及零预算变化 |
| AMEND/取消/恢复/政策 | peer完整读到原请求后扣住响应；执行对应原Store变化再放行，迟到完整响应仍带原tuple且accepted=0；核对不改原结算 |
| 五处SIGKILL | 预约后、claim后、网络等待中、收到完整transport但未存观察、存观察但未结算；杀实际driver再重开原库；每个原请求不重发 |
| 合法大转义输出 | 约1MiB原HTTP body含合法控制字符文本，原settle event序列化超过1MiB；新live有界4MiB路径保存完整原字节/hash，旧event限制不改 |
| 实际容量超限 | 合法SSE comment填满3MiB原头体累计预算；精确观察前缀和截断标志完整落库，仍形成原未知用量结算 |

SIGKILL后的宿主中断响应明确不是供应商回复。有claim且无原响应时，仍保存全null原结算；
测试后追加的用量证据是受信本地夹具，不称作真实计费确认。无claim时仅按原无发送权标记
接受host_no_dispatch的完整零用量证明；原null结算不被改写。

## 5. 本轮发现及修复

1. 无claim的已预约UNKNOWN没有合法对账入口。新增原writer、明确无发送权宿主标记和
   独立host_no_dispatch来源，使此崩溃阶段可保守恢复，仍不伪造transport或原费用。
2. 新paid门禁原先只看三个预算维度，cache null仍可能被略过。现改为live五维独立判断；
   管理/Worker窗口及收尾也消费独立projection，保留原usage。
3. 独立组件可接受的响应上限不等于Store可持久化组合上限。新live profile在准入前采用
   有说明的容量包络；测试精确前缀、合法大输出和event转义大小，避免可预知的发送后存储超限。
4. 默认event payload1MiB与原settlement fingerprint4MiB不一致。仅新增live结算显式用4MiB；
   旧Fake及其他event保持原1MiB，不通过改写原响应或事后截断使测试通过。
5. 测试夹具两处错误码、少传structured-context参数和一处表名错误已修正；这些开发失败
   不称作旧版本生产红测，也不把未执行的计划反例记作通过。

## 6. 复现与限制

在普通用户及已安装README所述依赖的环境中：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-release --parallel 2 --target live_model_store_tests live_model_store_driver
umask 0002
ctest --test-dir build-release -R '^live_model_store(_tls)?$' --output-on-failure
```

没有provider凭据解析、真实调用、真实价格估算、通用单Agent执行器或部署强沙箱正向验收。
Verifier API表达可信宿主边界；本地合成证据不提供供应商来源认证实现。SHA256只证明原字节
一致性，不替代签名或远端账单。实测仅支持本报告列出的本地协议/状态机边界。
