# 受控 live 工作流与模型来源专项验证

日期：2026-10-26（America/Los_Angeles）。基线：`1c572d3aa9121e85cf22aa1e4bb330b25a34a143`。
范围：[两模块接续](../implementation/two-module-continuation.md)中的模块 A 验证与
[ADR-087](../adr/087-model-decision-evidence.md)版本化评测来源。这里记录本专项实际结果；
CLI、完整构建、安装资源、sanitize/Clang、最终提交与 CI 由总体门禁报告记录。

## 1. 实际结果

| 项目 | 实际执行 | 结果 |
| --- | --- | --- |
| v1/v2 评测回归 | `tests/r3_evaluation_tests.py`，真实临时 SQLite + 基线 Release CLI | 31/31，通过；6.913 秒 |
| 原 live 工作流 TLS 集成 | `tests/test_live_workflow.py` + 新 `live_workflow_driver`，当前核心库 | 18/18 方法，通过；20.677 秒；ASan 夹具修正后的当前源码；包含额外身份/预检/取消子例 |
| 原 live 工作流 ASan/LSan | 当前测试 driver + 最终 ASan 核心库；开启 leak detection | 18/18，通过；195.202 秒；无 sanitizer 诊断 |
| 新 C++ driver | 普通 UID/GID 65534；GCC 13.3.0；C++20、`-O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` | 编译及链接通过 |
| Python 源文件 | Python 3.12.14；evaluator、新 TLS 测试、评测测试的 `py_compile` | 通过 |
| 本专项差异格式 | `git diff --check` | 通过 |

上述两套测试均以普通 UID/GID 65534 执行。临时工作区为 0700，合成资料受私有 umask 保护；
未改变生产权限断言。实际代码只接触临时 SQLite、临时文件、一次性自签 CA 和绑定
`127.0.0.1` 的 HTTPS peer。Bearer 为固定测试常量，不读取真实环境凭据。
外部供应商调用、真实消费与线上数据操作均为零。

## 2. 原生工作流的正向证据

正常场景通过真实 HTTPS 编码/解码和原 Store 路径连续执行七次调用：模型原生
`submit_plan`、`issue_work_order`、`fs_write`、`submit`、`check_submission`、
`accept_candidate`、`report_experience`。测试核对七条唯一 send claim、七份原 tool feedback、
七份独立合成用量核对、原任务 ACCEPTED、experience DONE，原 cost 仍为 null，当前投影的
已核对支出为 `7 × 17` micro USD。该数值来自本地 peer 独立写入的合成回执，不是实际模型价格。
第二次空闲 batch 不新增模型调用、send 或业务应用。

另外实际验证了：

- `fs_read(seed.txt)` → `fs_write(result.txt)` → `fs_read(result.txt)` 的原诊断反馈、完整原字节和
  同一绑定顺序；诊断不生成 ops 行。模型继续提交、TeamLead 检查和 Dispatcher 接受。
- 原模型否定接受后，由模型 `change_task` 对同一任务有限纠正，产生较新修订的提交、检查、接受。
- 反复原模型否定接受达到冻结返工上限后，模型 `escalate_task` 关联实际当前用户问题。
  第一批到原冻结管理调用上限时保留未处理目标；测试断言 `manager_limit` 后显式开启第二批，
  没有调大冻结上限或将未处理目标标为完成。
- 两任务 AMEND：第一份模型 `submit_amendment_plan` 只决定 alpha update；beta 仍有原 pending
  目标。后续模型 `resolve_amendment` 对同一计划决定 beta keep，两个当前任务均由模型提交、
  检查和接受。评测来源 helper 对全部 disposition、captured task CAS、原反馈及业务事实逐项核对。

TLS peer 是固定、可解释的测试程序。其原生工具调用验证调度/协议/持久化路径，不能证明
真实大模型会作出这些正确决定。

## 3. 失败、未知与重放边界

无独立用量证据时，原允许的 plan 可以应用一次，随后整个 batch 返回 `needs_accounting`。
下一批在读取凭据或创建 attempt 之前停止。原 usage/null 与结算均保留。

仅 cache-write 未知的独立场景将 input/output/cost 的预留与 unknown 金额都释放到 0，
仍保持 `needs_accounting`，凭据只读一次、网络只发送一次。此处 peer 明确省略原 cache-write；
已经由原响应报告的已知 cache-write 不能被 partial hook 擦成未知。

`Store::live_accounting_status` 的实质边界由原 API 生成并验证：空账本 ready、原预约 pending
不 ready、原未知不 ready、金额为零而 cache 未知仍不 ready、完整独立核对后 ready、矛盾证据
与 over-reserved 不 ready。查询不暴露原传输正文、凭据、独立来源引用或账单正文。

原 settlement + 原 feedback 精确重放时，hook 只执行一次，没有第二次 send，也没有新 event
或重复 plan 应用。已有 settlement 但无原 feedback 的结果继续拒绝事后应用，不通过重发或
重新构造 feedback 恢复业务资格。

独立回执的原 request body hash 不符、hook 返回另一 attempt 或另一 settlement 的 statement，
均保留原结算与错误反馈，零业务应用、零成功核对，并停止 batch。传输 deadline 与显式
stop_token 分别保留 `deadline`、`cancelled`；显式预取消为零凭据/零 attempt，传输中取消保留
原部分观察及未知 usage，工具列表为空。未启用配置、未获主机网络授权或传入宿主 dispositions
均在凭据与预约之前拒绝。

## 4. ASan 暴露的夹具阶段问题与修正

首次整体验证中，ASan `live_workflow` 的 18 个方法出现 8 项失败，耗时 124.700 秒；
没有 ASan/LSan 内存诊断。正常流程原有的 10 秒测试 batch 在插桩下真实达到 deadline；
专用 deadline/cancel 场景原来的 160/180 ms 从 batch 开始、尚未证明请求发送的位置计时，
在原模型上下文捕获完成前
即结束，未达到测试要求的传输阶段。原失败日志保留于
`/tmp/wls861-two-module-validation/asan-final.log`；不能把这些失败解释为核心通过。

修正仅限测试 driver 与 Python peer，未修改生产超时、冻结策略、业务/权限断言或 CMake
套件超时。正常场景的合成宿主显式给每批 60 秒，子进程独立有限上限为 240 秒，覆盖最多
三批及启动/结算开销。没有通过增加模型调用/返工/管理上限完成业务。

传输中显式取消改为独立 peer 完整收到实际请求、开始发送部分响应后写入 marker；driver
的有界 watcher 看到该阶段证据后才发 stop。watcher 最长 30 秒，超时或文件观察错误会产生
明确错误标记，测试仍要求 marker 已观察、没有 marker 错误、唯一实际请求、原 network_started、
`cancelled`、空工具、未知费用和零业务应用。预取消仍断言零凭据、零 attempt、零网络。

专用 deadline 改为显式 5 秒 batch，给原捕获阶段留出启动裕度；peer 连接保持等待到客户端
结束，避免服务端提前关闭伪造 deadline。除原 `deadline`、未知用量、空工具与零业务断言外，
新增核对持久传输 `raw_body` 恰为 peer 发出的 80 字节原前缀。这些阶段观察替代了
“在 Release 上耗时很短，所以一定已经发送”的假设。

同一改动已用最终 Release 核心库重新编译并实际执行 18/18，通过，20.677 秒。
ASan 复验使用最终 ASan 核心库及相同 `-g -fsanitize=address -fno-omit-frame-pointer`、
严格 warning flags；启用 `detect_leaks=1:halt_on_error=1:strict_string_checks=1`。
实际 18/18 通过，195.202 秒，无 sanitizer 诊断，保持现有 CMake 套件 300 秒上限不变。
复验日志分别为 `/tmp/wls861-live-workflow-release-stage-sync.log` 与
`/tmp/wls861-live-workflow-asan-stage-sync.log`。

## 5. 评测解释与反例

31 项评测用例保留原 v1 的固定条件、相同 Worker principal/team、失败与未执行样本。
v1 `passed` 现在明确标记为 `original_structural_checks_only`，模型来源 `not_checked`；
旧计划没有被重写或扩张含义。新 provider 计划必须使用 v2 `model-origin-v1`，Synthetic 可显式选择。

v2 正向覆盖原模型 plan/order/submission/check/accept → CONFIRMED 交付、原 read/write/readback、
同任务否定返工与新接受。TLS 用例进一步使用原 live transport 与 private response 检查模型
应用来源，并覆盖两任务 AMEND 和耗尽升级。

独立代码复核还发现并实际复现了一处跨任务来源拼接。新夹具让模型 A 完成真实原生
read/write/read，随后由受信宿主直接调用 `Store::submit_work`/`check_submission` 保存正确
result.txt；模型 B 则真实提交并检查无关 other.txt。它没有删除 attempt、改写账本、重标事件或
损坏原记录。复核前开发快照（evaluator SHA256
`0c43f546a1bb04d6379781ada093584c48b446d325f0c512d1a2c078730bf526`）对该夹具实际给出
`model_application_records_complete`、`model_tool_roundtrip`、`model_submission_source`、
`successful_tools`、`artifact_sha256` 全部 true，新的拒绝断言因此按预期失败。

修复将原 readback、随后模型 submit、同 submission 的模型 check 和期望文件字节关联到同一
原 binding/提交。v2 文件判据也只消费原模型 submit 的实际原提交。同一夹具修复后保留真实
工具往返为 true，但模型提交来源与模型 artifact 为 false，原模型记录仍完整。旧 v1 对原
`/work/result.txt` 路径的结构读取未改变；没有把 v1 的整个固定工具任务误称为通过。

另一子例真实执行模型返工接受后再由 Owner AMEND。复核前开发快照的旧模型来源仍为 true，
修复后按当前需求/计划及任务/指令版本拒绝；当前接受链还要求实际 acceptance 与 task 仍为
ACCEPTED。升级的当前问题关联同时收紧，实际 AMEND 会由原 Store 关闭旧问题，该边界也保留
拒绝。修复前两项来源拒绝断言实际失败，修复后完整 31+18 方法全部通过；没有将这些合成
关系反例标为固定材料 provider 实验或质量样本。

下列反例均实际失败，不能被最终 Store 状态掩盖：

| 反例 | 保留的真实事实 | 判定 |
| --- | --- | --- |
| 删除原接受 feedback | 原 CONFIRMED 交付仍在 | v2 来源不完整，failed |
| 接受 event 换为宿主 command ID | 原接受/交付仍在 | 不属于原模型应用，failed |
| feedback 换另一 attempt 并重算 hash | 原业务事实仍在 | 原 response 关系矛盾，invalid_evidence |
| 原 captured target 改为另一 submission | 原应用/交付仍在 | 目标/CAS 关系矛盾，invalid_evidence |
| live 原 transport response 换决定 | 原模型 feedback 与最终接受仍在 | 传输响应和原模型决定矛盾，拒绝来源验收 |
| A 的模型往返 + A 的宿主提交 + B 的模型提交 | 原调用、预算、正确宿主文件与 B 原模型链都在 | v2 同提交来源/原模型 artifact 失败 |
| 当前要求下使用旧模型返工来源 | Owner AMEND 与旧原模型链都保留 | v2 当前业务版本来源失败 |
| 多 Worker 身份改称 single_agent | 两个原 Worker principal/team 都可见 | 条件失败，未放宽身份规则 |

所有损坏只在各测试自己的临时数据库中注入；不可变 trigger 在负例中先显式解除并核对注入，
生产代码未解除任何不可变守卫。查询报告仍为只读，并核对原数据库行没有因评测而改变。

报告不声称 provider 质量或普遍费用优势；同一 Worker 的多任务基线身份设计与通用执行器仍开放。
本专项没有执行真实供应商、小任务十个配对样本或强隔离部署验收。

## 6. 实际命令与复现入口

本专项评测命令（运行器外层以 UID/GID 65534 执行）：

```sh
/tmp/wls861-r33a-venv/bin/python \
  /tmp/wls861-live-validation/source/tests/r3_evaluation_tests.py \
  /tmp/wls861-five-validation/build-release/wls \
  /tmp/wls861-live-validation/source

/tmp/wls861-r33a-venv/bin/python \
  /tmp/wls861-live-validation/source/tests/test_live_workflow.py \
  /tmp/wls861-live-validation/build-release/live_workflow_driver \
  /tmp/wls861-live-validation/source

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
  /tmp/wls861-r33a-venv/bin/python \
  /tmp/wls861-live-validation/source/tests/test_live_workflow.py \
  /tmp/wls861-live-validation/build-asan/live_workflow_driver \
  /tmp/wls861-live-validation/source
```

模型来源修复阶段的 driver 最初链接 `/tmp/wls-live-core-validation/build-release/` 的核心库。
ASan 夹具修复后，同一当前 driver 分别链接
`/tmp/wls861-two-module-validation/source/build-release/` 与 `build-asan/` 中既有最终
`libwls_s1.a`、`libwls_kernel.a`；未修改或重建核心实现。旧核心兼容回归由
[核心兼容报告](r3-live-core-compatibility-20261026.md)单独记录。全量构建已注册 CMake
`live_workflow`/`r3_evaluation`，Make `test` 同样执行新套件；集成后的最终结果由总体门禁记录。
本报告不会把专项源码快照的通过提前写成最终 Git SHA 或 CI 已通过。

源码一致性指纹（本专项成功运行时；仅本地一致性，非外部签名）：

| 文件 | SHA256 |
| --- | --- |
| `scripts/evaluate_r3.py` | `4b9c5e3c73f0d21cc1afb3e4800cc6fc9ae00c1ac7640a0e5b7d1c90494427df` |
| `tests/r3_evaluation_tests.py` | `69fd6700756032ef808ca04acc516bb4d4534d1448cd4fbeccb18b4bafbd64d5` |
| `tests/live_workflow_driver.cpp` | `b90e4b89d1c3901471f5c10e6ec6a81f60427a77154de885b82b096dd0cfb881` |
| `tests/test_live_workflow.py` | `34f4d8e21e8dddaeeb6b915906ef85e7f11b79581726b484fabd4d101427320d` |
