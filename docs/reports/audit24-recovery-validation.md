# 第二份 Claude 审核：蓝图、三个 P1 与接续验证

实现 `0.7.2-s1.6`，蓝图 `v0.4.1`。本报告区分设计、实际实现、临时系统回归和未执行的部署验收。
最终计数由本批日志采集，不沿用原审核或聊天中未经核验的成绩。

## 来源及恢复

基线 main：`e7fa64b231f1f7d3002aaf330e6c9b2616e06a37`。
只新增审核材料的分支仍为 `d79a536a2722f9ab632405dde11fa4d13403e215`；
材料已合入工作分支 `012bed74a8e688ce4831c09b0800c4cd14cdfc2a`，旧patch没有应用。
原报告SHA-256为 `40e097c5472bb550e21bd4813b26b144e82527aad04eee4b4ba03e277581c5a0`。

额外发布分支保存了三份逐提交patch及原始Git commit，series.tar.xz的SHA-256为
`d9703263fb2617c573fdb9ca7050a4d7d961dc845a459d059a24679e0f3a262a`。
恢复时逐项核对patch、raw commit、父提交和源码tree，不把传输分支的工作流或临时payload合入代码。

| 分项 | 精确恢复的提交 | tree |
| --- | --- | --- |
| 蓝图、处置、ADR-064/065/066 | `1e8f376c39c0e69773d10282d5f1e67fe9009e8b` | `371813eaff3c89782b6c65973e15145745ac90b9` |
| P1-3 语义说明恢复 | `b0b519219d216013dd932c00951a7c3cb247daff` | `14b7dc75db191041b9f5d64c0bf65d3436578144` |
| P1-1 强证据结算/Root核对 | `673a6df4134a4059827df12ac7e5b2fd0214b411` | `49fe4d583dffb2204b41e354406c9655242a7405` |

上述三项已在工作分支。远端导入验证run40071479903以普通用户通过当时38组Release CTest；
它不覆盖本轮后续P1-2源码。main和read-only审核分支均未被本轮改写。

## 蓝图决定与实现

设计顺序仍为P1-3→P1-1→P1-2→R1结构化请求/可恢复上下文→R2否定验收/返工→R3单协议回放及
获准真实闭环→R4同模型单Agent对照。R2a分配中断/目录工具独立推进，私有purge和服务化后置。
P-01/P-05未配置，不发送真实模型请求，不默认付费重试；M1保留，不引入弱沙箱降级。

P1-3保持不可变instructions，通过recover事件授权当前epoch投影；fresh ack不调用TeamLead，
不会重置工具额度或解除AMEND、WAITING、UNKNOWN。P1-1只在强隔离、断网、全后代退出、完整前后
工作区清单及差异同时成立时结算非零退出/超时/输出超限；Root核对入口具备证据、CAS和精确重放。
缺证明或结果写入失败仍UNKNOWN，人工核对也不伪造清理所需的原始进程退出事实。

P1-2新增冻结store_capacity配置、旧快照缺整块的历史等价值、新准入一致性检查、独立业务/控制计数、
必要结算余量、告警/持久准入暂停，以及Root窗口CAS检查点。没有新表/迁移，没有删审计行或改变旧
command/hash/result；检查点不重置工具/预算/返工或语义版本。文件和上下文的字节上限没有放宽。
写者事件计数缓存随回滚/外部data_version失效；只读不缓存动态计数。详见对应三份实现契约和ADR。

## 先红后绿与回归范围

- 原reopen转换回归在旧代码运行：1项通过、6项失败，fresh ack仍报obsolete execution context。
- 原event_cap程序实际在1024操作报quota，控制填充后4096总事件阻断说明/计划，取消仍可写。
- 转换后的长计数断言在旧库失败：`operation capacity exhausted`。它要求1100次操作和3600条额外
  控制动作后还能发布新计划；不是仅提高常数或删除原检查。
- P1-3增加7项C++和2项实际CLI；P1-1增加8项观察/分类、4项Store和4项CLI；P1-2增加8项Store、
  1项长计数和6项CLI。长计数操作效果是明确合成的Store状态；文件观察和CLI运行真实临时文件/SQLite。
- P1-2覆盖旧快照与配置源移除、缺字段/预算不匹配、Root角色、窗口CAS、原key重放、缓存回滚、
  AMEND不被清除、UNKNOWN与未知模型用量不可绕过。原负向撤销/交付/经验结算回归继续执行。

中途运行并非全部成功。首轮混用了旧编译配置加载器和新增配置后作废；后续新夹具错误包括冻结
配置没有同步原始依赖字节、wake参数拼错、复用旧command但使用新expected版本，以及故障注入未
提升状态版本，均在确认真实错误后修正。完整Release随后发现旧容量负向夹具仍依赖4096共享总数，
改为从请求冻结值填满独立业务池，并继续断言结算/撤销可用。

这轮兼容复核同时发现两个真实遗漏：业务水位不应阻止使用独立控制池的新问题，以及经验管理
结算不能被pump容量暂停挡住。现已修复，并保留原语义/预算门禁。新问题仍受独立控制及问题数量
限制，既有回答/取消/结果记录可推进。没有靠移除失败用例、放宽权限或关闭sanitizer获得通过。
首次UBSan/ASan全量因上述Release问题被主动中断；最终重跑与中断日志分别留存。

## 环境与命令

测试均为oai/uid1000，主验证umask0002，另跑Release/0022；Linux x86_64，GCC14.2、CMake3.31.6，
Boost1.83、SQLite3.46.1、OpenSSL3.5.5、LibYAML0.2.5，Python3.13.5及预装jsonschema4.26.0。
托管CI固定jsonschema4.10.3与GCC13/Clang18，本地不冒称使用了该环境。

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_UBSAN=ON -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DWLS_ENABLE_ASAN=ON -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
# each directory, as ordinary user, umask 0002:
cmake --build BUILD_DIR --parallel 2
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 \
  ctest --test-dir BUILD_DIR --output-on-failure -V
make -j2 test BUILD_DIR=build-make-review2 CPPFLAGS=-DWLS_REVIEW2_USER_FLAGS=1 PYTHON=/opt/pyvenv/bin/python
```

## 最终结果

P1-2独立代码提交：`43b45168594f98ad62268102893c293a736f18e6`；源码tree：`65bf365803d61f7f49ca9f401ad51b6aa0fe541b`。
本报告与结果JSON是其后的纯证据提交，不修改执行输入。

| 最终验证（普通用户） | 结果 |
| --- | --- |
| GCC Release / umask0002 | **41/41组，退出0** |
| 同一Release产物 / umask0022 | **41/41组，退出0** |
| GCC UBSan | **41/41组，退出0，无UBSan运行时错误** |
| GCC ASan（含泄漏检测） | **acceptance_runtime单独1/1 + 其余全部40/40，均退出0** |
| Clang17 Release | **41/41组，退出0** |
| Make严格入口，保留命令行CPPFLAGS | **完整目标退出0** |

各模式覆盖同一组 **S1 C++338、实际S1 CLI90、M1 C++52+演示3、Python参考36**。
相对0.7.1审核修复基线，新增28项S1 C++、12项CLI；模式重复运行不计为更多产品用例。

第一次完整ASan的acceptance_runtime在并行构建/测试负载下触及原90秒测试超时，未报地址错误。
复核发现新增容量检查对每次模型调用都重复解析管理目标，已改为仅在容量hold时读取；没有提升
原超时、关闭sanitizer或减少断言。冻结最终代码后，该套件单独通过（19个用例），再运行排除
该套件的全部40组；两份日志合计覆盖41个不同CTest目标，明确不是一次41组单命令成绩。

最终执行输入203项，复验前后SHA-256全部一致；清单SHA-256为
`f406dbfeabca9aaa2ca51dc52968b5ee5118930bcfe2b4bbb4a48813cbfea412`。原件、已发布SQL、vendor与交接档案无差异。
[机读结果和各日志字节指纹](audit24-results.json)保存实际计数和命令退出状态；完整日志、
环境、红色基线输出、输入清单与前次失败/中断记录一并放在 `wls861-audit24-validation-logs.zip`。
本批新P1-2没有托管CI成绩，不能用之前38组Release导入运行代替。

ASan的两次命令如下，使用同一份冻结产物，不调整测试超时：

```sh
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-asan -R '^acceptance_runtime$' --output-on-failure -V
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 ctest --test-dir build-asan -E '^acceptance_runtime$' --output-on-failure -V
```

## 验收和发布边界

没有bwrap可执行程序/普通用户可用cgroup委派，因此没有强沙箱正向部署成绩。P1-1分类测试中的
强证明为合成输入，实际清单、SQLite、故障注入和CLI测试与其分开记录。无付费模型/生产数据操作。
本轮复核来自同一实施者，不宣称独立第二模型或人工审核。

R1上下文增长、R2否定验收/返工及按请求恢复、R2a分配中断/fs_mkdir/清理放弃，以及seccomp、
内容寻址和安装资源契约仍未完成。容量窗口只解决Store准入可恢复性，不代表历史存储压缩或S1完成。

当前GitHub连接仅提供读取，已连接电脑离线；本地提交会随恢复包交付。未上传的P1-2不填远端CI成功。
远端最后核对：main为e7fa64b、干净工作分支为673a6df、review仍d79a536；准确完整SHA见上方及恢复包。
原始日志、冻结输入哈希、结果JSON和Git提交包放在会话制品中；下次有写入通道时须先核对远端
是否前进，再非强推发布并运行同一提交的托管CI，不能合入临时传输分支。
