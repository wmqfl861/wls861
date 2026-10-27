# 0.11.0-r3.3c-baseline 两模块联合验证

日期：2026-10-27（UTC）。开发基线为已经推送并完成原门禁的
`1b7020e3676a57da233aa097d32956a80942734e`，蓝图 v0.4.1，Store 410。
本批沿[接续范围](../implementation/two-module-billing-baseline.md)完成签名计费适配、
单 Agent 连续执行及各自实际验收。此前版本的报告保留原来源范围。

本页第2至5节的发布前输入与“最终”本地结果属于首次发布提交
`1df86cefbf7a18e618251c1dbdf72fe88c5d479b`。该提交的远端门禁随后出现两个ASan任务
未完成及原验收运行时90秒超时；原结果不回写为成功。当前同事务校验复用、CI容量修订、
新输入及复验范围见[发布后复核](billing-baseline-ci-followup.md)，不能用本页旧输入替代修订后的门禁。

## 1. 交付及实际边界

| 模块 | 新增行为 | 本批证据与限制 |
| --- | --- | --- |
| 签名计费与连续 CLI | 显式冻结唯一 challenge，独立 Ed25519 收据绑定原 wire 请求/响应；政策原字节绑定 v2 授权；同步核账、迟到追加、事务内来源唯一归属和只读精确重放 | 临时密钥、原 Store 和自建 loopback TLS。协议是本项目定义的受信中转契约，实际中转支持与真实费用来源仍需独立验收 |
| 一个逻辑 Agent 与 v3 基线 | 实际跨角色原响应/工具反馈进入每次原生输入；原预约事务核对全局链与完整消息；显式 live/fake CLI、跨重启继续；v3 独立重建输入和恢复来源 | 固定五任务、两个条件、十个样本位全部保留。角色继续限定权限；共享标签或宿主预制成果不能代替模型历史。真实模型质量/成本优势未建立 |

新增入口是 `pump-single-live`、`pump-single-fake`、`reconcile-billing`，原
`live-status` / `pump-live` 增加可选计费政策与严格 v2 授权。原无计费适配的 v1 行为、
旧 profile/hash、公开结构的源代码聚合初始化、原假调用和原结算继续兼容。
全 Store 业务工作区路径检查发生在授权、政策和收据内容读取之前；使用一次捕获的字节。

原费用未知值不被回填，收据只追加投影。错误响应可有独立账目，但仍为原 ERROR；
核账不能重发、重新应用旧响应、恢复业务资格或清除工具 UNKNOWN。
Store 保持410，没有新增或改写已发布 SQL；vendor、handoff 原件不变。

本批真实外部供应商调用、付费调用及生产数据操作均为 **0**。
真实中转接入、同模型品质/费用对照、强沙箱部署正向、并发租约、daemon/API 和私有行
purge 继续单独验收，不能由本地签名或工程测试推出完成。

## 2. 最终输入与完整门禁

[billing-baseline-inputs.sha256](billing-baseline-inputs.sha256) 固定 **305项**构建、
源码、测试、配置、SQL、Schema、提示词、依赖及 CI 输入，清单 SHA-256 为：

`576dd97a0632bc71a1301bff620997ceaf48c78bab3992585ff0accca8b43b48`

普通用户执行副本与该清单逐字一致。最终 CMake/CTest 注册 **78个套件**，保留此前
全部74个套件；本批增加 billing、single_agent、single_agent_runtime 和独立联合 E2E。
单 Agent 的原11个方法只按6+5分组，不增加或删去原函数和断言，两组仍各为90秒。

构建与测试使用普通 **UID/GID65534、空附加组**，严格 C++20 及
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`。
下表仅计最终输入的实际执行，不采用较早候选替代：

| 门禁 | 最终实际结果 | CTest墙钟 |
| --- | --- | --- |
| GCC Release，umask0022 | **78/78通过**；完整CTest | 165.92秒 |
| GCC Release，umask0002 | **78/78通过**；完整CTest | 170.44秒 |
| GCC Debug ASan/LSan，umask0002 | **9/9通过**；两个模块、原live Store/协议、真实CLI/TLS及评测，无诊断 | 604.30秒 |

机器记录见[billing-baseline-local-results.json](billing-baseline-local-results.json)，
原输出归档见[Release0022](billing-baseline-release-0022.txt)、
[Release0002](billing-baseline-release-0002.txt)、[ASan](billing-baseline-asan.txt)
及[ASan逐项记录](billing-baseline-asan-detail.txt)。保存时只移除行末空白；原始与保存
hash 均由机器记录列出。最终清单不包含这些报告本身，避免自指。

本地工具为 GCC13.3.0、Clang18.1.3、CMake3.28.3、Ninja1.11.1、OpenSSL3.0.13；
CMake 发现的 C++ SQLite 为3.45.1、libcurl为8.5.0。测试 Python3.12.14、
jsonschema4.10.3；Python SQLite 与 C++ Store 所链接的库分开解释。
ASan 始终使用 `halt_on_error=1:detect_leaks=1`，没有关闭泄漏检查。

外层 umask0022/0002 分别执行完整套件；原 C++ 和 Python 私有夹具内部显式0077，
不能把外层值冒称为全部内部创建动作的权限。没有改变生产工作区的权限守卫。

## 3. 专项与独立证据

| 范围 | 实际结果 | 证据解释 |
| --- | --- | --- |
| 计费原生模块 | billing 12/12，原 live Store 15/15、protocol 30/30；Release 和 ASan 各3/3套件；Clang18六个关键编译单元严格 syntax-only 通过 | 自有冻结输入的专项，完整命令、输入及输出见[计费报告](signed-billing-validation-20261027.md)，不冒称全部最终 Clang/UBSan运行 |
| 单 Agent 原生与评测 | 原11个方法；评测40/40，其中31个为旧v1/v2回归；实际两条件各两次恢复和损坏中间证据负例 | [单 Agent报告](single-agent-baseline-validation-20261027.md)记录各轮输入、旧候选及最终分组；不把重复执行相加 |
| 独立 CLI / TLS 联合验收 | 最终14/14方法，43.683秒；固定10/10样本位通过 | [独立报告](billing-baseline-independent-validation.md)、[最终日志](billing-baseline-independent-final.txt)及[十单元原报告](billing-baseline-independent-paired-report.json) |

联合验收使用实际 CLI、原 Store、实际 loopback HTTP 请求和独立 OpenSSL 签发流程，
不注入测试专用计费 hook。生产 verifier 验签后继续原事件流程；签发端与生产验签端
使用同类密码库，独立性是输入、签发流程和断言分离，不声称两套独立密码算法实现。

固定任务为原生工具往返、两个任务的部分 AMEND、原拒绝后的有限返工、返工耗尽后的
用户问题、原交付与精确回执。每个条件对应9/19/13/25/7次原调用，共146次；增加显式
批次没有增加原调用或重试。全部原成本仍为null，每次合成签名投影为17 micro USD。
这些金额只是夹具数据，不是实际价格或收益；报告的两项真实质量/成本优势声明为false。

单 Agent 在服务器实际收到的 wire 中保留此前每个原工具名称、参数和原结果，另核列表
长度与唯一ID数，防止用字典折叠掩盖重复。v3 从原 SQLite 重建实际历史、完整反馈、
请求 fingerprint 和唯一链；旧v1/v2的身份与来源解释不变。
跨进程来源以不可变原工单和整个 recover 区间证明；`previous_*`始终指原工单epoch，
每次实际投影连续递增并精确到提交，不能伪造中间binding或只看当前业务状态。

独立反例还包括错签名、错challenge/body、重复header、缺cache维度、超预留、HTTP429/503、
迟到精确重放、同库兄弟工作区、错误执行入口、执行时修改公钥文件和反转条件标签。
签名receipt trailer没有单列本批新专项；不能把原传输行为扩大为新增验收结论。

## 4. 保留的失败、修正及输入范围

1. 首次严格构建在新测试的 Boost.JSON/std::string 比较和 range-loop 复制处失败；
   改用明确字符串提取与引用，编译规则不变。原生初次10/11的唯一失败是错误码预期，
   原权限校验已经正确拒绝；仅修正测试预期。
2. 新v3最初错误读取配置hash所在表；现从原 `request_runtime` 读取并核原快照。
   跨重启原工单来源另按版本化v3恢复证据修正，旧v1/v2判据不变。测试配置路径、
   对抗夹具的原CAS以及分进程Fake脚本局部索引问题均有原日志，未放宽生产规则。
3. 首轮独立十单元为1 passed、4 failed、5 invalid_evidence，保留在
   [原失败日志](billing-baseline-independent-initial.txt)。100000的共同合成窗口确实触发
   原能力守卫；[只读诊断](billing-baseline-independent-quota.json)证明原三库不变。
   后续为两条件共同选择262144窗口，原历史上限、预算和生产编码检查不变。
4. CLI预检修复曾因构建副本保留旧mtime而没有重编对象；全表快照负例仍发现恢复副作用。
   实际重编后同一断言通过，固定 Release CLI SHA为
   `4ae6766372f54651260cb346fb607d13a8674d3c66a04bef486c04e48c2e7eb2`。
   不能仅凭源码已变或Ninja返回成功推定实际binary包含修复。
5. 前一完整候选Release两种umask均77/77通过（167.67/167.13秒），但ASan只6/8通过，
   521.81秒。新native11方法合计约71次原模型调用，在90.07秒被终止；因旧输出缓冲，
   不能据空日志推定卡在某个具体方法。最终只改本测试main为严格分组选择和逐项刷新，
   原11个函数及断言逐字不变，CMake/Make都明确执行6+5，两组各90秒。
6. 同一候选ASan联合测试478.801秒，两个single场景准确返回 `deadline`，聚合8 passed、
   2 failed。原调用均正常结算，但60秒batch尚未完成目标，不能评为成功。最终两条件
   同样采用每batch最多6次、最多8个显式batch，保留原60秒、CLI150秒和套件900秒。
   更多恢复仍须通过原来源链；没有放宽生产期限、裁掉历史、自动重试或删除失败断言。

较早输入见[候选清单](billing-baseline-candidate-inputs.sha256)，原ASan失败见
[候选原日志](billing-baseline-asan-initial-failure.txt)。首次严格构建失败另见
[首次构建日志](billing-baseline-build-initial-failure.txt)及
[首次构建输入](billing-baseline-build-initial-inputs.sha256)。新成功只属于第二节的最终输入，
不回写这些候选结果。开发过程的全部成功、失败和参数子例不相加成新的测试总数。

## 5. 复现与提交后门禁

以下在普通用户下执行，Python具备 `requirements-test.txt` 中的依赖：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-release --parallel 2
umask 0022
ctest --test-dir build-release --parallel 2 --output-on-failure
umask 0002
ctest --test-dir build-release --parallel 2 --output-on-failure

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_ASAN=ON \
  -DPython3_EXECUTABLE=/path/to/test-python
cmake --build build-asan --parallel 2 --target wls billing_tests single_agent_tests \
  live_model_store_tests protocol_codec_tests live_model_store_driver
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-asan --parallel 2 \
  --output-on-failure -V \
  -R '^(billing|single_agent|single_agent_runtime|live_model_store|protocol_codec|live_model_store_tls|live_workflow_cli|r3_evaluation|billing_baseline_e2e)$'
```

完整UBSan、CMake ASan、原Make ASan、Clang18及两种Release umask继续由既有六路CI
执行；整个job仍40分钟，原有各测试期限和sanitizer断言保持。本页在提交前记录本地证据，
不能把本地专项或祖先提交的CI作为最终提交通过。发布交接需核本页所在最终SHA、tree、
干净工作树及同SHA的[六路CI](https://github.com/wmqfl861/wls861/actions/workflows/ci.yml)
和[原提交源码快照](https://github.com/wmqfl861/wls861/actions/workflows/source-snapshot.yml)。
提交之后生成的远端状态以该精确SHA关联的工作流结果为准。
