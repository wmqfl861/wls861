# 0.11.0 发布后 CI 复核与预约校验复用

本页接续首次发布 `1df86cefbf7a18e618251c1dbdf72fe88c5d479b`（tree
`7faa4b32dc74cc14267eaf8ea49a07aef69b07b8`）。两个功能模块及本地原始门禁见
[联合报告](billing-baseline-validation.md)；本页保留发布后实际失败及后继输入，
不把未完成的任务或原候选结果计为修订后的成功。

## 1. 首次发布的远端结果

[原 CI 运行](https://github.com/wmqfl861/wls861/actions/runs/40531153587)
在同一 SHA 上实际执行六路矩阵：

| 路径 | 实际结果 | 证据范围 |
| --- | --- | --- |
| GCC Release，umask0022 | 78/78，0失败，302.93秒 | 完整CTest |
| GCC Release，umask0002 | 78/78，0失败，296.90秒 | 完整CTest |
| Clang18 Release | 78/78，0失败，305.16秒 | 完整CTest |
| GCC UBSan | 78/78，0失败，1443.59秒；无UBSan诊断 | 完整CTest |
| GCC CMake ASan | 整体cancelled；70通过、1超时、7未完成 | 原`acceptance_runtime`在90.13秒超时；开始第72套件后整体结束，不是完整通过 |
| GCC Make ASan | 整体cancelled；完整目标未执行完 | 原acceptance的19方法全过、约93.37秒；联合E2E只完成首个方法 |

两项ASan任务从15:27:29至16:07:47 UTC，均在原40分钟总预算后收尾为cancelled；
日志记录`The operation was canceled.`。取消不能证明后续尚未执行的测试通过。
机器记录与原/归档SHA见[billing-baseline-ci-initial-results.json](billing-baseline-ci-initial-results.json)，
原输出见[CMake ASan](billing-baseline-ci-asan-initial.txt)及
[Make ASan](billing-baseline-ci-make-asan-initial.txt)。归档只去行末空白，行数和末尾换行保留。

CMake ASan中的新增billing为6.73秒、single两组43.63/42.19秒、联合E2E497.88秒、
评测109.19秒，均通过。整体任务仍未完成，不能用这些专项替代完整ASan结果。
[首次源码快照](https://github.com/wmqfl861/wls861/actions/runs/40531153528)已成功，
其source artifact对应上述SHA和tree；快照成功也不替代测试成功。

## 2. 独立定位与最小修正

旧基线`1b7020e`的原ASan运行曾记录event/workflow/acceptance分别
25.64/25.11/56.25秒；首次发布对应42.80/41.21/90.13秒超时。
多个旧套件均变慢，单凭远端两轮耗时不能确定原因。静态核对确认原framed-v1/Fake/406
路径未启用新的logical-Agent历史，原19方法、测试配置和90秒限制均未改。
同机器、同普通用户、同ASan/LSan设置的旧基线、首次发布与修订计时在第三节分别列出。

确定存在的重复工作位于原`Store::reserve_attempt`：同一个`BEGIN IMMEDIATE`事务
已完整恢复并验证冻结配置，但逐个声明工具的`ready`→`tool_grant`又重复执行相同配置的
完整JSON/YAML、依赖字节与角色校验。默认Worker的多个声明工具使同一验证反复执行。

修订只在这一事务中传入已验证局部对象，每次仍把其**完整规范序列化字节与SHA**和本次
`authorize`返回的原请求行精确比较。对象不离开当前调用栈，没有全局、线程或跨事务缓存。
所有其他入口继续执行原完整`restore`。

每个工具原有的actor、epoch、binding、当前任务/团队、AMEND版本、UNKNOWN、WAITING，
以及冻结模板工具、当前请求allowlist与当前`denied_tools`交集检查全部保留。
该变化不缓存权限，不跳过任何当前数据库授权，不改变公开API、Store410、已发布SQL、
测试方法或断言。独立代码复核与实际负例回归分别记录，静态等价不能代替执行结果。

首候选仍超时后，独立临时插桩仅用于定位，19方法在96.522秒正常结束：配置`restore`
共2123次、内部累计39.642秒。管理问题查询475次/8.850秒和Worker `policy_for`
293次/5.402秒属于已有上层局部配置的重复恢复，合计768次/14.252秒。
这是诊断中的成本分布及可去除成本估计，不是原90秒通过，也不等于最终实际加速。

组合修订因此增加一个仅在`src/`内部使用的配置匹配helper。Worker的`bind`与
`verify_workspace`、运行器的管理问题查询复用当前调用栈上的不可变配置，每次仍重新
读取请求，并在采用配置前同时比较完整canonical bytes和SHA。Worker继续读取当前task
模板、host actor、epoch、workspace身份、owned容量及执行策略；管理路径继续读取当前
binding、host actor、management view、问题、任务、提交和等待状态。
原公开wrapper仍保留完整恢复路径，实际`run`的原`policy_for`也保持；没有实例/static
或跨调用缓存，没有缓存可变授权。临时诊断源码和binary与验收产物分开保存；最终构建
须实际重编原config loader并确认不含诊断标记后再执行原门禁。

CI总容量另作独立调整：仅`ASan`、`Make-ASan`两路由40分钟改为55分钟，其他四路仍40。
原Make在约39分钟才进入联合E2E；按同源CMake实测497.88秒估算，完成它已约47分钟，
之后还需两个原CLI套件和收尾。这是容量估算，不冒称Make已完成的实测。
55分钟保留合理余量；所有单测试、子进程、业务batch期限、普通用户、umask、ASan/LSan、
严格编译和原Make用户CPPFLAGS门禁保持。提高总预算不能消除原90秒套件失败。

## 3. 修订输入与实际复验

### 同机器原90秒对照

三轮依次执行，没有并发构建干扰计时。普通UID/GID65534、空附加组，外层umask0002，
原夹具0077，`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`。PTY仅用于原输出逐行刷新，
原测试函数、断言和可执行体内容未因采集进度而改动。

| 输入 | 原90秒结果 | 已完成 / 未完成方法 | 第16方法累计耗时 |
| --- | --- | --- | --- |
| 旧基线1b7020e | 90.152秒超时 | 16通过、3未完成 | 89.378秒 |
| 首次发布1df86ce | 90.112秒超时 | 16通过、3未完成 | 89.036秒 |
| 首个局部复用候选，Store SHA55c06327… | 90.089秒超时 | 17通过、2未完成 | 80.326秒 |

原19方法测试文件SHA在三轮均为
`88faa11f3b5902dda75cdf51f29e820614dcb7b8681b72fe86f959e39f6f29c4`。
完整输入、binary SHA、逐方法计时及原/归档日志hash见
[初始同机对照](billing-baseline-acceptance-initial-comparison.json)，原输出分别见
[旧基线](billing-baseline-acceptance-old-baseline.txt)、
[首次发布](billing-baseline-acceptance-first-publish.txt)及
[首候选](billing-baseline-acceptance-candidate-01.txt)。

这些运行没有观察到断言失败或ASan诊断，但超时进程未完成正常退出，不能声明完成LSan退出检查。
前两轮同一完成位置仅差0.342秒，本机对照不支持把远端整体变慢归因于新logical-Agent历史。
首候选约一成改善仍未通过原期限，故保留为失败候选；后续定位和修订结果另列，不能反写成功。

### 组合修订后的原门禁

[新输入清单](billing-baseline-post-ci-inputs.sha256)固定306项，SHA-256为
`93d930c437487c7eb9914f81667b58b8db0d12b56a2ba4a432369a21e5f809a4`。相对于首次305项清单，
只有5个已有运行时源码、1个新增内部header及CI总预算变更；原测试、断言、公开header、
config loader、SQL、Schema、供应商依赖和归档handoff均保持原字节。
普通用户执行副本逐项匹配此清单，构建与回归后再次核对。

临时插桩的[诊断记录](billing-baseline-restore-profile.json)、
[原输出](billing-baseline-restore-profile.txt)、[构建输出](billing-baseline-restore-profile-build.txt)
和[诊断专用差异](billing-baseline-restore-profile.patch.txt)分别归档，诊断代码未进入产品源码。
最终ASan实际重新编译原config loader，并检查最终binary不含诊断标记。

组合修订在原90秒硬期限内完成原19方法：**19/19，78.964秒，正常退出，无ASan/LSan诊断**。
随后通过原CTest注册入口顺序运行五套原测试（`--parallel 1`），均保留原90秒限制：

| 原套件 | 方法 | CTest实际秒数 |
| --- | --- | --- |
| store | 31/31 | 10.948 |
| offline_runtime | 10/10 | 6.1227 |
| acceptance_runtime | 19/19 | 77.710 |
| worker_tools | 17/17 | 26.9834 |
| live_model_store | 15/15 | 8.49438 |

合计**5/5套件、92个不同方法通过**，CTest墙钟130.26秒；19方法的独立PTY运行不重复计数。
普通UID/GID65534、空附加组，外层umask0002，原夹具0077，ASan/LSan启用且无诊断。
最终验收binary SHA-256为
`a22099e92db6e3b33117d4b9964422f7b98b263ef9bc93e3b77c7dfd3e4fd625`。
完整源/二进制身份与逐方法计时见[最终同机对照](billing-baseline-acceptance-final-comparison.json)
及[ASan机器记录](billing-baseline-post-ci-asan-results.json)，原输出见
[19方法](billing-baseline-post-ci-acceptance.txt)、[五套CTest](billing-baseline-post-ci-asan.txt)、
[JUnit](billing-baseline-post-ci-asan.xml)和[最终构建](billing-baseline-post-ci-asan-build.txt)。

root另以同一306项输入执行完整Release构建及两轮全套回归，普通UID/GID65534、空附加组，
`ctest --parallel 2`，严格编译参数保持：

| 门禁 | 实际结果 | CTest墙钟 |
| --- | --- | --- |
| Release，umask0022 | **78/78，0失败、0跳过** | 138.07秒 |
| Release，umask0002 | **78/78，0失败、0跳过** | 143.86秒 |

完整构建进程40.176秒，退出码0。实际CLI SHA-256为
`c0fe1336f3cd35a5fab1a23725aeb5389adad9ffba9ef7de28d3745f8191fd46`。
详见[最终本地机器记录](billing-baseline-post-ci-local-results.json)、
[构建](billing-baseline-post-ci-release-build.txt)、
[Release0022](billing-baseline-post-ci-release-0022.txt)及其[JUnit](billing-baseline-post-ci-release-0022.xml)、
[Release0002](billing-baseline-post-ci-release-0002.txt)及其[JUnit](billing-baseline-post-ci-release-0002.xml)。
原/归档日志hash和唯一行末空白归一化规则均保留在机器记录中。上述本地结果属于当前组合修订；
五套ASan用于验证此次修改路径，完整六路远端门禁仍以精确发布SHA的后继运行判断。


## 4. 精确提交后的最终核验

修订提交仍须完整执行六路CI和同SHA的源码快照。发布前记录不能预填这些远端结果；
最终交接按该提交的Actions实际结果、Git tree和干净工作树核对。
真实供应商调用、付费调用及生产数据操作仍为0，本页不扩展两个模块的产品验收边界。
