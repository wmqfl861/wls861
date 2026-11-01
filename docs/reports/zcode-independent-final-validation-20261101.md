# ZCode 整改与分配置最终验证补充（2026-11-01）

本轮已经实现测试观测修复、Store 事件命令字节复用和独立计费入口诊断补丁。code08 的两种 umask 完整 Release 各92项通过，但本机 GCC ASan/LSan 为48通过、42超时、2失败，**整体 LOCAL_VALIDATION_COMPLETE=false**。code08 与 ceaa 各自的远端7个 workflow、17个 job 已独立核验成功；它们不能覆盖本机失败、历史根因或主协调者批准。本报告冻结于本机正式串行矩阵与唯一单方法诊断均已结束之后、文档提交发布之前。文档自身的最终 SHA、PR 和 CI 由发布后外部收据、PR 正文及最终交付回传登记，不能用本文的代码输入身份代填。

本文承接原 [测试观测报告](zcode-native-diagnostics-followup-20261101.md) 的03:24:58截点及追加、[事件字节报告](store-event-command-bytes-validation-20261101.md) 的04:20:59截点及V3观察，不覆盖原文或历史失败。`E/` 指数据盘 `/mnt/d/codex-workspace/wls861-zcode-followup-20261101-wvm7aclp/evidence/`，本机索引不等于公开附件。执行用户UID/GID1000，数据盘为实测 `/mnt/d`、ext4，checkout、构建、私有TMPDIR和证据均置于该盘。全阶段完成只依据真实终态与身份收据；root 曾在整个 UBSan 阶段运行期间冻结一个已经结束的 Async CTest 输出块，其精确截点及后续终态绑定另列，未将活动阶段前缀当作完成。

**精确输入、作者与PR依赖。**

| 层次 | 完整提交与直接依赖 | 实际范围 |
| --- | --- | --- |
| 原P1审核，PR10 | `99c9063f3e29cd6d69c56fb15815715d499a374a`，base `86b82f8f63a3c60c7659c9cd8f9b8e7d7252870b` | 保留原CHANGES_REQUIRED。 |
| 前批P1整改，PR11 | `7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a`，base99 | 两项生产整改属于前批，不归为本轮ZCode新成果。 |
| 六文件测试工具 | `579e3c7675bc2abb336dde6a95bfddf2139f47cd`，base7dc | C++即时观测、async等待诊断、格式兼容及自检。 |
| 正式产品输入，PR12 | `08dad7e815b9c3c84d3bd2393818b2030a6e0cde`，base7dc | 包含579及Store函数内字节复用/手动探针；794份tracked源码。 |
| 独立计费补丁，PR13 | `ceaa9b3e18618c592345001dfc597bdaf579cf81`，base08 | 两测试文件和两报告，796份tracked源码；未改正式08矩阵输入。 |
| 新文档分支 | `docs/zcode-final-validation-20261101`，从08派生 | 与PR13为兄弟分支；报告冻结时尚未提交；最终commit/tree/ref/PR由 `E/final-docs-publication-01/` 的真实发布收据及PR正文记录。 |

现有依赖为 [PR13](https://github.com/wmqfl861/wls861/pull/13) → [PR12](https://github.com/wmqfl861/wls861/pull/12) → [PR11](https://github.com/wmqfl861/wls861/pull/11) → [PR10](https://github.com/wmqfl861/wls861/pull/10)，均为Draft/open、未合并。新文档PR也以PR12@08为base，不含ceaa的计费测试补丁，仅引用其独立证据；它的自身CI不能从任一兄弟PR继承。最后完成的远端快照main仍为 `a6b8559018a599d7a806b05942857a56d69a00ce`。PR10的GitHub review状态为COMMENTED，正文CHANGES_REQUIRED/不批准，不是APPROVED。本轮未retarget既有PR、写main、强推或合并。

前批 R-A3-01 区分真实AMEND/cancel状态转换与精确重放，只停止实际废止的捕获工作，保留Actor/CAS/指纹与迟到证据；R-A4-01 在占用任一M1池执行槽前检查跨池资源FIFO资格，保留公平、有界队列、控制/结算和审核预留。原99自身头/库上的真实红例和候选四合法调用真实完成见原整改报告；不得把这两项归因于本轮序列化修改，也不以其定向通过代替完整门禁。

**本机ZCode实际工作与红绿。** 已安装CLI为 `3.16.3-39 / runtime0.17.0`。round01–05编写观测修复，06只读建议，07–09编写机制探针，10修改Store；编码工具真实调用及原收据保留。随后还有一次真实计费修复调用，作者编辑完成后重复等待，按授权SIGINT并reap，exit130、outer_timeout=false；不能写成作者会话成功，也不能声称本轮ZCode调用为零。编码工具授权不扩展为WLS真实供应商/付费产品验收授权；WLS诊断使用FakeModel、合成loopback TLS和私有临时数据，没有真实WLS供应商或付费WLS运行时请求。

六文件为 `tests/test_support.hpp`、`tests/async_coordinator_tests.cpp`、`tests/test_native_test_observation.py`、`scripts/run_a4_fairness_red_green.py`、`tests/test_a4_fairness_gate.py`、`.github/workflows/ci.yml`：即时flush原输出、增加等待位置诊断、精确兼容一条同名START及工具自检接线。原断言、五秒等待、期限、退出码和92项产品注册均未放宽。原support上同一自检编译成功、到达独立屏障后因缺即时完整观测而失败；候选GCC/Clang各验证一个runner的三个合成场景。fairness九方法旧版8通过/1目标失败，候选9通过；runner24、immediate4、transport11各按自身工具方法计数，不加入产品CTest。首次夹具、检查器和后续修正均留存，不伪称生产漏洞。

Store变更仅 `Store::Impl::event`：容量守卫及event_id之后，取得一次 `json_text(command, payload_limit)`，用同一const字符串计算SHA并写payload，五增一删；新增一份手动探针。原校验/限额和事务、Actor/session/epoch、CAS、UNKNOWN、Workspace及清理守卫未删除，无跨调用或跨事务缓存，Store仍410。编码失败仍在INSERT前回滚；不夸称低层分配/crypto等异常求值顺序完全相同。

同一冻结探针分别配各版本自己的头/生产库，旧版编译成功，业务、持久字段/事件、精确重放与拒绝断言均到达，仅最后首次序列化count==1失败（实测2，exit1）；候选首次1、重放1，exit0。五项定向CTest另通过。探针不是第93项CTest；count减少是机制证据，不是加速测量或历史Timeout解释。历史比较只覆盖明确列出的字段，不宣称全数据库字节等价。源码/红绿与工具的非作者工程复核保留作者范围；同模型多角色不等于第二模型评测，亦不代签主协调者验收。

**code08本机矩阵按配置分别登记。** 下表计数均为CTest注册项层级，顺序为passed/failed/timeout/skipped/notrun；JUnit截断或未审内部输出不能据此写成全部逐方法验证。两个Release是同92项执行两次，不是184个不同套件。

| 实际配置 | passed / failed / timeout / skipped / notrun | 真实退出和证据 |
| --- | --- | --- |
| Release，umask0022，parallel1 | 92 / 0 / 0 / 0 / 0 | exit0；04:07:37.786883结束；recorder893.977124秒。 |
| Release，umask0002，parallel1 | 92 / 0 / 0 / 0 / 0 | exit0；04:20:56.417195结束；recorder776.829678秒。 |
| GCC13.3 Debug ASan/LSan | 48 / 2 / 42 / 0 / 0 | CTest exit8；outer exit1、outer_timeout=false；注册/选中/有结局92，missing_outcomes0。 |
| GCC13.3 Debug `-g` 完整UBSan，umask0002 | 64 / 2 / 26 / 0 / 0 | CTest exit8；outer_timeout=false；记录器6394.274523秒，92项全部有结局。 |
| Clang18严格 Release 完整测试，umask0002 | 92 / 0 / 0 / 0 / 0 | exit0；09:55:05.632500结束；recorder1082.371176秒。独立终态审计另列。 |
| 本机Make-ASan | NOT_RUN | 本批未安排，不能用共享GCC替代。 |
| GCC runner自检 | 24工具方法通过，exit0 | 04:20:59结束，独立于92项和产品sanitizer结果。 |

`E/final-matrix-handoff-audit-01/` 已核两种Release各自JUnit/完整LastTest、原92名称/期限、794份源码对Git08映射与61项binary/control身份；源码/二进制前后相同。`E/gcc-asan-terminal-audit-01/` 于07:06:50完成GCC独立终态核验，同样核原目录、命令/cwd/end marker、794源码及61实体。GCC两项Failed为https_transport、live_model_store_tls；42项Timeout单列，不能把内部失败方法再加成独立CTest结果。

GCC CTest耗时8255.999286秒，outer9385.470002秒。原inner summary仍保留 `status=NOT_COMPLETE`、ended_at06:57:23.431379、error=ctest exit8及identities_unchanged=true；未修改runner或历史关闭标志。准确结论是GCC阶段已终态取证、产品门禁失败，不是整个driver结束。整个串行driver末阶段于09:55:06.958748结束，统一工具会话11592真实返回exit1和最终marker；`E/full-driver-root-terminal-01/receipt.json` 为root观察收据，SHA256 `630e3061ff982f87825733c6c27e454367d9c2d3098d4e986fa57d23130a9b4f`。该文件不是driver原生completion，也不是重建的完整stdout；只保存实际末次工具返回及原events身份。随后只读进程检查未发现本任务正式驱动/编译残留，未杀其他任务。最终计数补充而不改写事件原报V3七项Timeout的局部观察。

UBSan 独立终态审计为 `E/ubsan-terminal-audit-01/`：configure/build/test 六份源码 before/after 共用同一794输入，与冻结Git08 blob/SHA映射及当前实体一致；61项实际binary/control与前后清单一致。构建自身元数据为GNU13.3.0、Debug、`-g`、UBSAN ON/ASAN OFF，92条编译命令含`-g -Werror -fsanitize=undefined`。92名称/期限及JUnit、完整LastTest、console、failed-list一致。测试07:35:50.955124—09:22:25.230238 UTC，CTest自报6392.81秒，记录器6394.274523秒；两个时长分别保留。审计33项校验和全部通过不等于产品UBSan通过。

该轮Failed为HTTPS partial-upload的`len(peer.prefixes)`断言0!=1，以及结构化CLI capacity方法在`pump-fake`内的50秒`subprocess.TimeoutExpired`。后者CTest为Failed（101.111秒），不能加成第27个CTest Timeout；原worker-calls16/max-calls64保留，没有由异常推断容量守卫正确或错误。HTTPS后续uploaded-body/retry-authority断言未到达，根因仍OPEN。

UBSan公开日志投影也独立生成：三份输出各替换同一697字节argv（路径与运行时request ID），只在LastTest替换14893字节主机doctor观察对象；后者不是请求/结果字典。原`programs_executed=0`、`strong_prerequisites=false`、其余traceback和所有范围外字节保留，JUnit outcome不变。精确范围、原件/区间/投影SHA在该审计recipe，不套用GCC的7424字节规则。原采样cgroup字段缺失/null照录；另一次08:20只读观察绑定实际driver/CTest所在可见cgroup层级，当时CPU/memory上限均为max，不能回填整轮状态或推导超时原因。数据盘观察与资源收据分别在`E/matrix-resource-observation-0739-01/`和`E/matrix-cgroup-observation-01/`。

Clang独立终态审计为 `E/clang-terminal-audit-01/`：真实Clang18.1.3、Release、`-O3 -DNDEBUG`，92条编译命令均保留`-Wall -Wextra -Wpedantic -Werror -std=c++20`；ASan/UBSan均OFF，不因环境变量存在就计为sanitizer。原92名称/期限、JUnit、完整LastTest和console一致，794源码/61实际binary-control前后匹配Git08。计费注册项128.218秒、原16内部方法OK，10固定单元passed仍只计1个CTest。LastTest仅对本文件的14893字节host doctor对象作精确投影，保留原安全计量汇总；console和JUnit为已审原字节。审计26项校验和OK；首次元数据probe误把整数contract当字符串的exit1及stderr保留，是取证工具失败而非Clang产品失败。该审计没有启动产品或编译，也没有自行构造整个driver终态。

**code08与ceaa各自的远端CI。** 两个独立审核分别完成于06:42:41和07:46:03；每个SHA均有7个workflow、17个job自然completed/success、17份job原日志实际checkout和7份archive身份，未触发重跑。链接逐个对应自己的输入：

| workflow / event / jobs | code08 run | ceaa run |
| --- | --- | --- |
| Linux六路 / Push / 6 | [41271460248](https://github.com/wmqfl861/wls861/actions/runs/41271460248) | [41282664727](https://github.com/wmqfl861/wls861/actions/runs/41282664727) |
| Linux六路 / PR / 6 | [41271462706](https://github.com/wmqfl861/wls861/actions/runs/41271462706) | [41282669494](https://github.com/wmqfl861/wls861/actions/runs/41282669494) |
| Windows组件 / Push / 1 | [41271460257](https://github.com/wmqfl861/wls861/actions/runs/41271460257) | [41282664647](https://github.com/wmqfl861/wls861/actions/runs/41282664647) |
| Windows组件 / PR / 1 | [41271462756](https://github.com/wmqfl861/wls861/actions/runs/41271462756) | [41282669512](https://github.com/wmqfl861/wls861/actions/runs/41282669512) |
| fairness / Push / 1 | [41271460270](https://github.com/wmqfl861/wls861/actions/runs/41271460270) | [41282664744](https://github.com/wmqfl861/wls861/actions/runs/41282664744) |
| fairness / PR / 1 | [41271462726](https://github.com/wmqfl861/wls861/actions/runs/41271462726) | [41282669560](https://github.com/wmqfl861/wls861/actions/runs/41282669560) |
| 源码快照 / Push / 1 | [41271460295](https://github.com/wmqfl861/wls861/actions/runs/41271460295) | [41282664700](https://github.com/wmqfl861/wls861/actions/runs/41282664700) |

code08 Push实际checkout为08，PR为 `47534906df0402b20651b1971ac642c98aa1af9a`，同tree `149bca333bce2460ba940ace3c76cc1111dfdd83`；ceaa Push为ceaa，PR为 `e50c7f70b97d3c91a74d484e0c8d02f4e41c015b`，同tree `a068734b3c44c3cdef428b7dd5f6376b6be8bf32`。实际检出来自job原日志，不能只凭API head_sha。

code08两份共享GCC产物各92项run/pass、0失败/跳过/未运行，794源码与61binary/control前后相同；其完整源码原字节审核复用冻结audit03，此次重新绑定同一ZIP/Git映射，不称重新语义审阅794份实现。ceaa审核独立读取全部796份、34,930,778原字节，核blob/mode/SHA并递归重建tree a068；source ZIP artifact13073741687为24,094,891字节、SHA256 `9f68330bdd7e993bead8aba4b5a5ed8dc8f51d33ec39e77e7dc384939f796b4f`。

ceaa Push/PR的strict、strict0002、Clang18、UBSan及共享ASan各自原92项全部通过，共同一目录十次完整执行，不能把它们说成920个不同测试。两份共享GCC仍是GCC13.3/Debug/ASan/LSan及原期限，796source、61binary/control前后相同；billing成功case的JUnit各仅保留7/16条完整内部ok行，不能补造缺失方法原文。另两次Make-ASan均有原recipe及成功step/job，billing16方法及Ran16/OK完整；这是独立Make目标，不另计92项，未另提供数字退出码收据。远端归档不含ELF/PE本体，不能独立重哈希远端可执行载荷。

ceaa采集首轮六次GET失败及collector v1 JSONDecodeError/exit1均原样保留；六次GET失败另行补读；v2在请求失败时不再解析失败响应，并将采集并发降至2，后续full/status仍按原入口采集，不能说所有后续采集只读失败项。这是取证工具/网络失败，不是产品CI失败。08的PR fairness检查器首次错误要求merge等于head，修正及原exit1也保留。

**计费小补丁的独立红绿与发布。** 原helper的addSubTest已正确；billing真实__main__没有选用它。ceaa仅接入已有ImmediateTextRunner并新增真实runpy入口自检，原四方法和业务AST不变，helper/生产/权限/期限未改。合成loader在一个subtest登记失败后发出独立屏障并等待stdin；audit hook拒绝child子进程/网络，不运行计费夹具或WLS。

同一新增方法配原08入口到达缺traceback断言，exit1；配候选1通过、完整工具5/5，exit0。冻结探针两边都在登记后活着、TERM/-15并reap，stderr在终止前后相同；旧无原trace，候选原trace/witness在终止前已flush。ZCode作者exit130、配置未变与无已知会话残留见最终handoff，独立审阅和真实红绿不被作者退出覆盖。此修复保留失败原因，不解释底层billing失败或超时。

PR13正常push后读回ceaa；07:49:20正文补齐了自身CI结论，body SHA256 `114ee4ab79d6a23cc332edced1e09b1f27e349eda07433d0c55a6b9cf5129f88`，读回匹配，head/base/Draft/open/未合并不变，收据 `E/pr13-ci-final-update-01/completion.json`。ceaa本机仅工具5/5，没有完整重跑该SHA本机产品矩阵，LOCAL_VALIDATION_COMPLETE不能标TRUE。远端四次工具自检各五方法不等于20个不同方法。新文档来自08，所以仍只有四个immediate自检方法；报告引用ceaa不会自动带入第五方法。所有输入产品CTest仍92。

**T4已获得的有界观察与尚未定位的原因。** `E/live-workflow-diagnosis-08-01/` 只读分析了已经结束的live_workflow段，原18内部方法中观察到6ok、2FAIL，余10为NO_START_MARKER_OBSERVED；整个注册项Timeout300.10秒，不把它拆成18个CTest或把无start算通过。原300/240/60秒三级期限、顺序和断言未改，没有诊断重跑。

第一条cross_task断言前driver exit0且stderr空，JSON却有threw=true、`openat2 workspace path: No such file or directory`，无reports；ledger.consistent及accounting.ready为true。它们不能说明结果文件存在、全部安全断言通过或没有业务效果。driver将局部reports延迟到phase全部完成后才附回，Error可能让已返回pump摘要不可见；源码的host读取是候选检查点，没有调用栈/phase证据，不能断言缺失的就是result.txt或为何缺失。第二条model_exhaustion只留下False is not true，manager_limit断言为false；没有原result，blocked/stop_reason/后续phase均未知。两条断言均在TLS peer上下文正常返回后出现，但不证明整个进程树或cleanup完成；后续未见第九方法标记也不定位cleanup卡住。

原raw结果仍私有；安全投影显式省略身份、正文及完整账目，保留错误、字段存在性、计数与未知。不能从六次credential/evidence读取推出六次业务效果。GCC终态ctest/LastTest/JUnit包含同一完整字典行，公开候选仅按精确字节区间替换该行、保留其余trace/失败/结果，原件和投影SHA及算法保留。投影不是原日志，有界秘密模式扫描不是所有私密内容的证明。旧缺失traceback不能回填；当前有界分析不建立Clang、ASan、磁盘、序列化或Store锁的因果关系，根因仍OPEN。

共享HTTPS partial-upload单方法诊断已且仅执行一次，见 `E/partial-upload-current-run-01/completion.json`。此前静态工具自检的五拒绝子例通过且外部进程/网络尝试0；它们与ceaa五个unittest方法是两组计数。root在整个driver/编译真实结束后发出新SHA固定的私有通知；原方法、15秒子进程界限、12秒elapsed断言、3500ms deadline、900000 padding、4096 prefix、2048 receive buffer及原driver不变。实际方法ERROR：invoke触发15秒 `subprocess.TimeoutExpired`，诊断launcher记录的leader returncode=1、wrapper_exit_code=1，outer_timeout=false；不据此给invoke内部被超时的产品子进程补造正常exit1；方法15.410秒，含794源码/61binary及ELF闭包前后验证的包装器54.976837秒。输入和已解析依赖身份前后相同，leader已reap，专属进程组已不存在。十条连续观察含最终observation_finished，关闭时peer accepted/prefixes/requests均0、live_threads0。此次没有到达原prefix断言，不能称为原GCC/UBSan的0!=1断言再次命中，也不能由peer的零接受计数推断无连接、已定位生产阶段或权限缺陷。分类始终DIAGNOSTIC_NOT_GATE，原完整失败及根因继续OPEN；未重试到绿色。

单次诊断的终态取证在 `E/partial-upload-current-run-review-01/`：审计再次核对794源码的Git blob/mode、61binary/control及去重42个当前已解析ELF实体，原12条metadata命令均exit0，输入前后完全相同。原stderr精确路径投影与显式字段summary均标明衍生记录，保留原异常及未执行断言；不公开原completion、随机临时路径/配置/证书或数据库。审核者曾编写launcher，root先做了非作者静态审核并实际执行，本次终态取证不冒称另一位非作者launcher代码审核或主协调者批准。

原Release0002 owned_capacity、UBSan/ASan Async Timeout、ASan资源五秒断言、更早GCC48PASS/2FAIL/39Timeout、Clang29/39含10Timeout、旧live_workflow根因全部继续OPEN；新本机48/2/42是另一次精确08输入，不能改写更早数量。`historical_clang_asan_failures_closed=false`不变。当前GCC两Failed与live_workflow注册项Timeout分别存在，不混同为同一结果。

当前UBSan Async的独立范围事实另见`E/ubsan-async-completed-review-01/`与终态绑定：code08源目录29个内部方法，前28个有START、前27个有PASS，无FAIL/RESULT/UNTIL-DEADLINE。第28个`resource FIFO spans all classes while independent siblings progress`只有开始标记，第29个AMEND方法未观察到开始；不能补造其失败或通过。第23个方法的四调用/四接受证据不能代替第28个方法。3881字节冻结块和22237字节早期console前缀都精确匹配最终console；早期观察当时`whole_UBSan_phase_ended=false`保留，本次新增全阶段终态绑定。没有逐方法耗时，也不能从无UNTIL推断poll/谓词已返回或从总180秒推断具体停顿阶段、死锁或单一性能根因。

**Windows与最终包边界。** code08及ceaa各自的Windows原始完整LastTest都核实每轮4个CTest/27个不同方法（3/4/14/6），同方法重复执行不累加成不同方法，亦不加进Linux92目录。实际均是Windows Server2022/build20348、elevated、NTFS及原生MSVC Release组件切片；Windows11普通用户实机NOT_RUN，完整Store/Workspace/进程/强沙箱/清理runtime为NOT_IMPLEMENTED。两SHA的新旧run都保留runner image `20261025.346.1` 与artifact `20261026.347.1`冲突，精确镜像身份UNRESOLVED，不能用另一轮成功解释冲突。

原native V2/event V3报告及两原ZIP仍在数据盘保留冻结字节。非作者扩展成品审核对原native ZIP的174成员未发现本次定向隐私阻塞，可按原摘要发布；原event ZIP的92成员中存在一份未省略的主机诊断对象，故原ZIP只留私有证据，不进入此次Git提交。[event安全公开修订版](store-event-command-bytes-receipts-20261101-public-v2.zip)使用新文件名、有效控制清单和原件到投影的明确映射；原event报告的旧ZIP引用与原SHA作为历史身份保留，当前公开资产以相邻索引为准。最终文本证据归档名为 `zcode-final-validation-evidence-20261101-v2.zip`，仓库相邻的 `zcode-final-validation-evidence-20261101-index.json` 在归档之外记录其成员、摘要和非作者成品审核，避免自引用。沿用原307份来源映射，其中304份逐字复用、3份作显式隐私修正，保留被替代原映射；新增GCC LastTest采用原始文件双区间投影。其他新增项显式列出来源与逐文件投影；原99份私有CLI日志、临时配置/数据库、凭据、原始结果字典和二进制均不获准公开。成品审核、正常发布与远端读回属于后继真实操作，以该索引和 `E/final-docs-publication-01/` 记录为准。文档分支不改变code08正式输入；其自身Actions、实际checkout/源码树在发布后独立核验，不猜PR编号或借用08/ceaa绿色。

首个完整523成员包（3022267字节，SHA256 `4040aef760317176109a997d2b571b86c42bee962ed66fb777cf30cab872fdfe`）被非作者审核拒绝发布，失败包、原选择及exit1记录均保留。通用秘密模式0候选没有发现结构化对象的跨副本残留；定向扩展在主包找到4个、旧event包找到1个完整host doctor对象，原native无命中。三个包共789成员CRC/UTF8全读，递归输出字符串、转义结构、完整/截断对象与原私有结果前缀均检查，没有新增截断或原结果字典候选。原guard SHA256 `df9b3be5a5fceaf9a84aa0b39a7a72aab0bf07016cf5d3ab7f339237cf4c874c` 实际exit1，整改后必须用同一guard对准确新包验证，不能把原失败改为通过。

三份基线替代为Release0002 LastTest与两份远端job日志；远端两份仅允许先原LOG_C0_ESCAPE、再精确BYTE_RANGE的受限组合，逐步绑定摘要。新增GCC LastTest从原raw保留7424字节私有结果范围和14893字节doctor范围各自的投影身份。范围外字节、合法固定schema计量、全部FAIL/Timeout、JUnit结果及期限保持。assembler05、原307准备、原报告与被拒绝ZIP不覆写；新assembler06只接受明确成员替代和受限组合。最终准确v2包、公开索引与发布读回由后继成品审阅和真实收据记录，这项归档缺陷不改写任何产品测试结论或关闭历史根因。

| 交付状态 | code08 / PR12 | ceaa / PR13 | 本报告及未来文档PR |
| --- | --- | --- | --- |
| IMPLEMENTATION_COMPLETE | TRUE，限定工具及事件字节复用 | TRUE，限定计费测试工具 | 报告冻结；成品证据与发布单独核验 |
| LOCAL_VALIDATION_COMPLETE | FALSE，GCC失败；历史和后续分列 | FALSE，本机仅工具5/5；该SHA完整产品矩阵未重跑 | FALSE，不代替代码门禁 |
| PUSH_COMPLETE | TRUE，精确08已读回 | TRUE，精确ceaa已读回 | PENDING，须真实发布回执 |
| REMOTE_CI_COMPLETE | TRUE，仅08/同tree PR输入 | TRUE，仅ceaa/同tree PR输入 | PENDING，新SHA自身CI待取证 |
| PR_REVIEW_COMPLETE | PENDING，主协调者正式决定 | PENDING，主协调者正式决定 | PENDING，未代签批准 |
| MERGED | FALSE | FALSE | FALSE |

报告整理和成品审阅不运行归档代码，也不修改原报告、原ZIP或正式code08工作树。报告撰写、STATUS/ROADMAP及新文档工作树的实际写入均属于本次交付。正式矩阵、单次诊断与已授权ZCode编码会话的真实执行按各自收据登记，不把整理动作的范围误写成整个任务未执行产品或编码工具。后继正式采纳仍依主协调者决定，本轮无任何PR合并。

报告首轮正文冻结于2026-11-01T10:06:14 UTC，保留在被拒绝v1包及freeze02的retained-freeze01副本。修正后的本轮正文冻结时间（UTC）：2026-11-01T10:39:05.390492+00:00。发布后身份与CI只追加外部回执，不回写历史正文。
