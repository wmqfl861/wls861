# S1.5 业务验收、交付与经验增量实际验证报告

日期：2026-10-20（UTC）。目标蓝图：**v0.4.1**；库/CLI：**0.6.0-s1.5**；原 M1 入口保持 **0.1.1-m1**。开发基线：`98fe590ce1cedda8c0427bd21251299d7b19cd8e`。

本批沿 S1-02/03/08/10/11/12，接入当前候选的 Dispatcher 业务判断、原字节不可变交付包、精确回执、显式经验报告和独立留存收尾作业。代码、008迁移、模型输入 Schema、蓝图、路线图、任务卡、CLI 演示和本报告共同交付。**整个 S1 尚未完成**；实际清理、完整服务、供应商与强环境正向验收继续按依赖实施。

**最终稳定源码的Make严格门禁、UBSan门禁均退出0：S1 C++ 263/263、实际S1 CLI 55/55、原M1内核/CLI 55/55、Python参考契约36/36全部通过；独立CMake Release / CTest 29/29组通过。** 本批新增Store 25项、运行器19项、交付文件17项和实际CLI 17项，不将不同构建模式重复计为产品验收。

## 1. 本批实际接入

| 模块 | 当前行为 | 实现和证据 |
| --- | --- | --- |
| SQLite 406 | 追加008事务迁移及资产指纹；保存业务判断、原源经验报告、冻结交付意图与独立回执；旧接口和只读兼容 | [Store](../../src/store.cpp)、[008迁移](../../db/migrations/008_acceptance_runtime.sql)、[Store回归](../../tests/acceptance_store_tests.cpp) |
| Dispatcher 验收 | 捕获一个当前候选及任务/提交CAS，核全AC、原证据、权限/版本、阻塞问题和未决操作；肯定接受与经验job同事务 | [输入Schema](../../schemas/accept_candidate.schema.json)、[运行器](../../src/offline_runtime.cpp) |
| 原字节交付 | 从持久提交快照组包，先存意图，再实际写文件/fsync/原子目录发布；原目录重试核全包，不覆盖 | [DeliveryDirectory](../../src/delivery.cpp)、[文件回归](../../tests/delivery_tests.cpp) |
| 精确回执 | 核包ID、哈希、需求/计划版本、接收方、方式、key和本次耐久证据；确认/业务终态/收尾留存同事务 | [CLI](../../src/cli.cpp)、[实际CLI回归](../../tests/s15_cli_smoke.py) |
| 经验恢复 | TeamLead显式报告或明确无新增经验，绑定原source/hash和job CAS；终态仍计预算，只允许专用管理动作 | [EventRuntime](../../src/event_runtime.cpp)、[运行器回归](../../tests/acceptance_runtime_tests.cpp) |
| 收尾查询 | 回执、截止时间、独立cleanup job、恢复扫描与阻塞原因可查；没有删除或purge运行器 | [S1.5契约](../implementation/S1-acceptance-delivery.md) |

`CANDIDATE_READY`、任务 `ACCEPTED`、交付 `STORED` 和请求 `DELIVERED` 是不同事实。TeamLead检查不会自动补出Dispatcher决策，发布不会自动确认接收，已有回执也不会自动将收尾标为DONE。

## 2. 分层回归与完整门禁

### S1 C++ 和实际 CLI

| 检查组 | 用例数 | 关键范围 |
| --- | ---: | --- |
| Runtime types | 4 | 严格JSON、SHA、typed ID与完整binding |
| 原Store | 30 | 402/403、事务、身份/CAS、操作/预算、目录及不可变执行事实 |
| 原Event Store | 15 | 404、领取/部分消费/恢复、独立计划、AMEND、等待、原反馈 |
| 原Workflow Store | 21 | 405、原问题/路由/回答、原始文件快照、检查/返工、容量 |
| 新Acceptance Store | 25 | 406、验收/经验原子性、捕获对象、历史AMEND、交付/回执、控制额度、双目录和晚到问题 |
| Config loader | 15 | 严格来源、冻结依赖、继承、恢复与未支持能力拒绝 |
| Model / Context | 15 | 原调用输入/绑定/用量、完整输入帧、前缀和依赖 |
| Schema | 12 | 17个本地Schema与13个原完整示例，模型字段和完整记录分开 |
| 原Offline runtime | 9 | 原note路径、AMEND、迟到响应、预算、取消和恢复 |
| 原Event runtime | 14 | 双任务、逐项AMEND、wait、错误/额度、跨运行原反馈 |
| 原Workflow runtime | 14 | 问答、单目标捕获、原文件和二进制、返工、旧响应与恢复 |
| 新Acceptance runtime | 19 | 原候选上下文、伪造/旧响应拒绝、历史经验、终态恢复、预算和控制容量 |
| Workspace | 19 | 真实文件、SHA/CAS/patch、路径/链接/容量、并发和故障 |
| Sandbox | 17 | 强模式拒绝、doctor与私有进程监督fixture，不含强隔离正向成绩 |
| WorkerTools | 17 | 真实文件/Store/Fake、权限与控制、唯一派发和结算失败围栏 |
| 新DeliveryDirectory | 17 | 原字节、原子发布/重开恢复、并发、不覆盖、路径/身份/篡改、同步失败分类 |
| **S1 C++合计** | **263** | 既有202项，加本批25+19+17项 |
| 原S1.1 / S1.2 / S1.3 / S1.4 CLI | 8+8+10+12 = **38** | 既有实际C++ CLI回归 |
| 新S1.5 CLI | **17** | 验收→真实发布→精确ACK、只读预检、原字节/二进制、旧版本/篡改、经验/收尾和双目录 |
| **S1实际CLI合计** | **55** | Python启动实际C++ CLI、SQLite与临时文件系统 |

原S1.2/S1.3 CLI仅将新库预期版本405调整为406。旧402–405 Store和运行器测试继续使用其原接口与资产。固定模型脚本只提供可复现的决策和用量，不作为自然语言判断质量的证据。

### 原内核、参考契约和来源

| 检查组 | 数量 | 证据范围 |
| --- | ---: | --- |
| M1 Journal / Config / Scheduler / TaskEngine | 10+5+9+28 = **52** | 原内核状态、并发、持久化及合成审核/回执 |
| M1实际CLI | **3** | 原合成生命周期和输入错误行为 |
| Python Schema / SQLite参考 | **36** | 13项结构/语义参考、23项事务参考；Python本身不受UBSan插桩 |
| v0.3原交接包 | **33文件** | 固定原件清单；原51项产品验收仍未执行 |
| v0.4原审核包 | **49文件** | 固定原件清单 |
| 固定上游原件 | **29文件** | 实际字节数和Git blob SHA-1核对 |
| 原schema与004–007迁移 | **字节未变** | 与开发基线逐字节比较，新能力只追加008 |

### 最终稳定源码门禁

| 门禁 | 实际状态 |
| --- | --- |
| Make严格完整门禁 | 退出0；S1 C++ 263/263、S1 CLI 55/55、原M1 55/55、参考契约36/36及原件完整性通过 |
| Make UBSan完整门禁 | 退出0；同样的完整分层回归全部通过，无UBSan错误 |
| 独立CMake Release / CTest | 退出0；29/29组通过，实际测试总时长67.45秒 |

CTest的29组由20个C++组、6个CLI组、1个Python参考组和2个原件完整性组组成。不同构建模式、重复执行和组内用例不合并成产品验收通过数。

## 3. 关键路径的实际验证范围

**当前候选与不可变原事实。** Dispatcher上下文包含原提交文件视图、任务AC、WorkOrder和TeamLead检查。运行器固化单一提交ID、任务/提交CAS与原调用输入；伪造目标、身份、版本、source或工具字段均拒绝。Store重复检查当前原证据，不把模型声称的通过或TL pass当成授权。要求正式独立审核的任务在本增量保持阻塞。

**验收与经验的原子性。** 真实SQLite触发器故障回归验证：经验job写入失败时，验收事实、任务状态和事件一起回滚。双线程对同一捕获对象竞争只允许一个胜者；精确命令重放不生成第二份验收。AMEND将受影响ACCEPTED任务阻塞，原提交、判断、经验source/hash继续保留；原经验报告可用当前管理权限完成，不能给旧源改盖新版本。

**原字节包。** 正常固定流程使用7次Fake调用：Dispatcher计划、TeamLead说明、Worker写入、Worker提交、TeamLead检查、Dispatcher验收、TeamLead报告。实际文件随后被改变或移除，发布结果仍与原持久提交逐字节一致。二进制、空文件、哈希和完整文件集有独立核对。包还保留完整原始需求及AMEND、冻结计划、验收、经验/待办、操作副作用、原问题及未答非阻塞问题的限制。

**容量与用量。** 每包最多64个文件、单文件2MiB、manifest最多1MiB，清单与解码文件合计最多4MiB，超限不截短原证据。包内用量冻结于准备交付时，回执用量冻结于业务终态时；后续经验仍计入保留的预算与attempt账本。未来实际purge前须再做最终汇总，不能把旧快照视为最终总量。

**跨存储发布恢复。** SQLite先保留PREPARED意图；文件模块在固定根内建立本次stage、写全体原字节、同步并以不覆盖rename发布，再同步根目录，最后记录STORED。回归覆盖实际系统调用、并发相同意图、重开已有包、发布后根同步失败，以及模拟rename错误。无法证明效果未发生时保留OutcomeUnknown和原意图。不存在绕过耐久步骤的生产fault开关；故障注入局限测试子进程和受控夹具。

**路径与失败清理。** 根规范路径/device/inode固定；Store双向检查所有已绑定业务workspace与交付根隔离，控制数据库也在交付范围外。路径穿越、非法名、符号链接、硬链接、FIFO、根身份变化、非原文件和额外条目拒绝。临时失败只清理本次确证创建且inode仍匹配的stage内容，不按前缀递归删崩溃孤儿。

**精确回执与只读预检。** CLI核包ID、完整清单哈希、需求/计划版本、接收者、方式及key。错请求、旧版本、换根、篡改和额外文件在写者接管前拒绝；测试比较整个SQLite逻辑快照确认没有业务状态、代次、事件或回执变化。同键同意图跨进程返回首个结果，同键改接收者/方式冲突。只读查询及verify不签回执；ACK重新核全包并同步，首份fresh publication证据保存到原回执。

**终态经验与必要控制。** 缺脚本或耗尽预算不会自动填写“无经验”。请求交付或取消后，管理入口仅允许捕获的TL经验job，仍计冻结预算，不恢复普通Dispatcher规划、发WorkOrder或Worker业务。Takeover递增job CAS/lease，旧响应保留结算但不应用。普通4096事件容量耗尽后，专用经验预约和必要控制使用有界8192控制容量；普通验收历史256条、必要负面控制最多512条，不绕过付费调用预算。

**收尾留存。** ACK和取消在业务终态事务内保存独立receipt及cleanup job；冻结TTL回归使用7天并核实际截止时间。查询展示未完经验、UNKNOWN、未结算用量、交付与所有权/退出运行器缺失等阻塞。包、工作目录和请求行继续存在；本批没有删除、purge、TTL清除或cleanup DONE证据。

## 4. 交叉审查与修正

- 只读预检与取得写者之间可能发生AMEND。新增FS前 `authorize_delivery_publication`，写后Store再核原意图；旧预检不能作为写文件的授权。
- ACK不能只引用早先存下的publication。接口强制传入本次核验和同步的 `fresh_publication`，首份proof进入原回执；精确重放也重新核包，但稳定幂等身份不包含易变proof字段。
- 同一请求向两个根发布后，确认一份曾使另一份永久停留STORED。ACK现在同事务将其余PREPARED/STORED置SUPERSEDED并递增CAS，保留原内容。Store覆盖两个原状态，实际CLI覆盖两份目录字节保留、假待办消失和精确重试。
- 可选Worker在包准备/保存之后提出阻塞问题，曾能被旧意图绕过。共用 `delivery_current` 现在覆盖FS前授权、发布记录和ACK的全请求阻塞问题复核。正规初始可选计划回归证明三个入口均拒绝且零回执；回答后可重试同一原包，不改写清单或哈希。
- 用量说明曾误写后续经验计入终态回执。现按包、回执和持续账本各自的实际冻结时点说明，不修改原回执或伪造最终总量。
- CLI篡改夹具恢复只读权限后再断言同长度字节篡改被拒绝，避免权限拒绝提前掩盖真正的内容校验；额外文件测试同样保留合法权限，只改变文件集合。

最终日志取以上修正全部纳入后重新编译、完整回归的结果。没有降低警告、禁用断言、改写旧迁移资产或启用弱生产沙箱。

## 5. 环境、复现与原始证据

环境：Linux x86_64、内核6.18.55、glibc2.39、GCC13.3.0、CMake3.28.3。系统依赖为Boost1.83.0、C++ SQLite3.45.1、OpenSSL3.0.13、LibYAML0.2.5。Python3.12.14、jsonschema4.10.3；Python sqlite3为3.53.1，与C++链接版本分开记录。

在仓库根目录执行：

```sh
make -j2 test BUILD_DIR=/tmp/wls-s15-make \
  PYTHON=.local/test-venv/bin/python
make -j2 ubsan BUILD_DIR=/tmp/wls-s15-validation \
  PYTHON=.local/test-venv/bin/python

cmake -S . -B /tmp/wls-s15-cmake -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/absolute/repo/.local/test-venv/bin/python
cmake --build /tmp/wls-s15-cmake --parallel 2
ctest --test-dir /tmp/wls-s15-cmake --output-on-failure
```

Make强制C++20及 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`。UBSan增加 `-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`，设置 `halt_on_error=1:print_stacktrace=1`。各构建目录独立；最终修正后各自增量重编并执行完整测试，不把前一构建的旧二进制成绩当成最终源码结果。

证据文件：

- [Make严格完整门禁](s1.5-make-test.txt)
- [UBSan完整门禁](s1.5-ubsan-test.txt)
- [CMake配置、构建与CTest](s1.5-ctest.txt)
- [活动源码、配置、契约及文档SHA-256](s1.5-source-SHA256SUMS.txt)

日志归一化仓库绝对路径为 `<repo>`，保留实际独立构建目录并去除行尾空白。活动文件清单覆盖177个文件；报告/日志及清单自身不参与递归哈希，原始档案和固定vendor按各自原清单校验。最终三路门禁运行期间另外比对143个运行时、构建、测试、数据库、Schema及配置输入的SHA-256，全部相同。发布格式检查随后仅移除新Store测试末尾一个多余空行；无语义变更，下面的源码清单记录最终文件字节。

在仓库根目录运行 `sha256sum -c docs/reports/s1.5-source-SHA256SUMS.txt` 可核对完整活动文件；原报告与旧清单不随本批版本更新。

本报告记录推送前本地结果。GitHub同一提交的strict和UBSan Actions在推送后独立核验；不在提交产生前预填CI成功。历史报告与历史源码校验清单保持原提交含义。

## 6. 保留边界和五阶段承接

全部模型输入来自固定Fake脚本，没有真实模型调用、费用、供应商网络、服务部署或用户数据清理。语义判断质量、跨功能独立审核和模板确定性验收器不由这些状态/文件回归证明。

`durable_local` 表示完整字节已保存到指定本地目录，不表示用户已阅读；`explicit_ack` 为可信宿主提交的确认，网络接收者认证及HTTP路由尚未实现。文件权限和合作锁不隔离任意同UID宿主代码；本批没有整机掉电、底层硬件缓存或全部异常文件系统的耐久实验。

406升级要求可验证的旧资产；已有参考收尾状态但缺原来源的旧库拒绝编造历史。仍没有缺冻结快照的非空401升级方案。普通workspace未登记可删除资源所有权，本批只留存收尾待办，不执行递归删除、私有行purge或TTL删除。

下一增量继续S1-11：受控资源所有权清单、停止/退出证明、UNKNOWN核清、最终用量汇总、经验与精确回执门禁、幂等删除及可恢复checkpoint，再推进S1-12本地daemon/API。强环境正向、残留cgroup、真实input和后台操作按接口依赖并行推进。S2承接独立审核/DAG，S3承接记忆版本闭环，S4承接MCP/评测，S5按证据扩展互通及可选严格清除；原51项产品验收与整个S1仍按各自完成条件验收。
