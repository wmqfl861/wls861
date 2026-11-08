# G1-2 Responses 兼容子集验证记录（2026-11-08）

一页公开验证摘要；原始命令输出、逐套件 RED/GREEN 日志与工具链记录保留在私有实施证据
目录（`protocol-implement-01/evidence/`）与根的观察快照中，不复制原始日志。本地运行
（Release、UID 1000、loopback）；远端 CI 矩阵与真实供应商验收见"未执行范围"。

## 行为与版本边界

- opt-in 扩展 `responses_compatibility`（`wls-responses-compat-v1` /
  `openai-responses-2026-11-08`）：alias key 必须精确等于 profile.model，1..8 个显式
  snapshot，规范化排序入哈希；错 key/未知字段/未知版本/重复/空值在准入前拒绝。
  无扩展 profile 的 to_json/哈希/编码字节保持不变（既有黄金断言继续通过）。
- 新 profile：instructions 为六段固定标题纯文本，`instructions_sha256` 入 evidence；
  所有请求显式 `include:["reasoning.encrypted_content"]`；响应 model 接受 alias 本身或
  显式 snapshot。off/default/effort 声明规则不变。
- reasoning 原件：`ModelResponse.responses_output`（wls-responses-output-v1）保存完整
  有序 output 原值；缺密文 → `reasoning_replay_unavailable`；reasoning-only 不伪完成；
  下一轮按原顺序原样回放，provider item/call ID 跨轮重复在编码前拒绝；无扩展历史的
  `history_...` 断言逐字保持。
- refusal：HTTP/SSE 合法 refusal → Error `model_refusal`，text/tool 清空、usage 与原件
  保留；协议错误优先；混合工具不执行；无自动重试。传输窄分支：完整 200 refusal 的
  transport 观察为 Completed(reason=model_refusal) 且 response_usable=false；HTTP 500、
  framing 等传输异常仍以 http_error/protocol_error 覆盖模型分类。OfflineRuntime 反馈
  追加 `failure_layer:"model", model_error_code:"model_refusal"`，重放同一错误。
- 计量：worker/management 窗口与 single-agent 消息计量计入 opaque 字段；最近
  reasoning-bearing 轮超 inline_bytes → 预约前 Quota，不降级 history_reference；更早
  完成轮保持原引用策略。Store 410 无 schema/SQL 变更（user_version 保持 410）。

## 实际 RED→GREEN（同测试字节与命令）

- 阶段 A（基线，产品零改动）：model 1/protocol_codec 8/r1_structured 2/sse_replay 5
  个新用例失败于配置准入（"missing or extra provider profile fields"）；TLS 两套件与
  r3 evaluation 同样失败（driver/评估器拒绝扩展）。日志：red-stage-A-*.txt。
- 阶段 B（仅 profile 脚手架，中间源码哈希另存）：四类业务 RED 真实失败——
  reasoning HTTP 非 Completed、SSE 非 Completed/unsupported_event、refusal 分类错误、
  历史/下一轮 payload 丢失。日志：red-stage-B-case-list.txt、stage-B-source-hash.txt。
- 阶段 C（完整实现）：同一套件、同一测试字节转 GREEN（见下）。
- 两处测试预期偏差（记录在案）：reasoning-only 的显式错误码按计划"显式模型输出错误"
  采用既有 invalid_response（原误写为 reasoning_replay_unavailable）；instructions
  窄预算边界由 65536 校准为 640（纯文本 instructions 小于旧 JSON 包装，原边界不再触发）。
  其余断言字节未变。

## 本地运行结果（Release，UID 1000，逐轮如实记录）

- 目标套件 ctest（9 项注册）为**中间轮**：8/9 通过，`live_model_store_tls` 失败，含
  两个 compat 子例断言失败（http_error 与 http_500 命名、TLS 两轮重放的 actor/frozen
  binding 不一致）；随后经测试/源码修正，单独重跑 TLS 目标套件通过
  （green-stage-C-tls.txt、green-stage-C-lms.txt）。该中间文件不与其他轮合并表述。
- 全量第一完整轮（LastTest 11:26–11:35 UTC，protocol-prior-lasttest.log）：78 注册中
  76 通过，`s14_cli_smoke` 与 `s15_cli_smoke` 失败（分别 10、17 个 PermissionError
  子例）；该轮期间观察到 protocol 重建，失败根因未证实，不宣称。此轮不作为最终门。
- 全量最终轮 `ctest -j1`：78/78 注册全部通过（0 失败），与基线注册数 78 一致，未增删
  CTest 注册（green-full-ctest.txt，根 12:16 独立快照同内容）。该轮与 billing 模块的
  树内编译**重叠执行**，非串行最终组合门；串行组合与六配置矩阵状态为 PENDING（由既有
  CI 绑定最终 SHA 执行）。
- 空白检查：tracked 代码/文档 `git diff --check` 干净；纳入暂存后
  `git diff --cached --check` 恰报 3 处，全部为新增 SSE 线上夹具的"new blank line at
  EOF"（reasoning-empty-summary.sse:30、reasoning.sse:48、refusal.sse:30）。判定：该
  空行是 SSE 终态事件的**必需事件分隔符**——回放契约（src/sse_replay.cpp）对缺少最终
  分隔符的流抛 `stream_interrupted`，且仓库全部 4 个 `.sse` 夹具（含既有 text.sse）
  均以 `\n\n` 结尾。夹具字节保持原样、未编辑；建议的机械检查为
  `git diff --cached --check -- . ':(exclude)tests/fixtures/responses/*.sse'`（实测退出
  码 0）加"每个 .sse 以事件分隔符结尾"的字节级断言，代码/文档空白检查保持不变；未改
  任何 Git 配置。
- 产品 delta（含构建脚本配置，新增+删除）：**794 行** = 567 行 tracked 产品改动 +
  224 行新内部头 `src/responses_compat.hpp` + 3 行 CMakeLists 测试 driver，≤800 上限；
  分项如实列示，不以重贴标签规避上限（逐文件 numstat 见私有证据 numstat-final.txt）。

## 下游发现复验（2026-11-08 12:41 UTC 起，首轮硬审核之后）

- 发现：下游规划审查静态提出多 native 轮次编码风险——`src/protocol_codec.cpp`
  每轮 provider call_id 向量在消息循环外累积而 `native_consumed` 每轮清零，两个
  不同合法 native 历史轮时第二轮 `function_call_output` 可能拿到第一轮的 provider
  call_id。静态发现不是 RED，须在当前候选（commit `4c66e41`、
  `protocol_codec.cpp` sha256 `6f4a97b…`）上确定性复现。
- RED（生产零改动）：`tests/protocol_codec_tests.cpp` 新增双轮正例（rs/fc/provider
  三类 ID 与 host attempt/call 全不同，其余绑定、工具声明与原件事实保持合法）。
  首次构建仅测试源编译失败（GCC dangling-reference，改按值拷贝；编译失败非 RED）；
  修正后同目标同命令 exit=1、38 通过/1 失败，失败断言恰为第二轮
  `function_call_output.call_id == "call_compat_round_b"`（实得第一轮
  `call_compat_fixture`）；A 轮配对与 B 轮 reasoning/function_call 原样回放断言
  全部通过。RED 测试字节 `92db8d0e…`，完整日志在
  `protocol-native-pair-fix-01/evidence/`。
- 修复：`src/protocol_codec.cpp` 单行——每个 native round 开始处清空该轮专用
  provider call_id 向量（跨轮重复 provider item/call ID 的拒绝集合不变），修复后
  源码 `85bbdfc1…`。
- GREEN：同测试字节、同命令 exit=0，39 通过/0 失败；受影响套件重跑（protocol_codec、
  model、sse_replay、r1_structured、live_model_store、https_transport、
  live_model_store_tls、live_workflow、r3_evaluation 共 9 项注册）9/9 通过，未增删
  CTest 注册（新用例为 protocol_codec 套件内子例，38→39）。
- 产品 delta：src/include/scripts 口径 792 行（756+36）；含 CMakeLists 3 行的
  本报告原口径 795 行，均 ≤800 上限。
- 局限：首轮 hard-review PASS 早于本发现，是历史结论，不覆盖本双轮条件；新源码
  须另经全新独立硬审核。全量 78 注册与六配置 CI 仍在最终精确 SHA 上 PENDING。

## 未执行范围

- 真实供应商/付费请求 0；fixture 端点仅 `.invalid` 与本地 loopback TLS。
- 串行最终组合门与 sanitizer/Clang/Make 六配置矩阵未在本会话运行，状态 PENDING（由
  既有 CI 在最终 SHA 上执行，不虚称 PASS）；Windows 11 保持 NOT_RUN；私有托管证据
  保持 BLOCKED。
- 首轮全量中 s14/s15 的 PermissionError 根因未诊断、未宣称；由后续 CI/串行轮覆盖。
- G1-3 纠错重调、G1-4 分页/all_turns 全历史、G1-6 付费保护不在本项范围。
