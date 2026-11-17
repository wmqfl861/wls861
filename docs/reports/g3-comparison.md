# G3 对照报告：B1 真实基线与 G3-C 合成对照

**B1 十格已完成并封存：完整 oracle 为 4 PASS、4 MISSING、2 FAIL，仅两格有原始确认交付。** 每个任务、每种条件各运行一次（n=1）；终态不同，不能据调用数或 token 总量宣称普遍性能改善或同等验收比较，也不构成 v4 PASS、G3 出口或 0.15.0 发布。

基线 [main f9da424](https://github.com/wmqfl861/wls861/commit/f9da424)，实现 0.13.1；[#150](https://github.com/wmqfl861/wls861/pull/150) D-50 与 [#151](https://github.com/wmqfl861/wls861/pull/151) B0 已合入。本轮是原五任务文本加冻结场景补充的新请求，两条件共用输入/约束，不能改称 v4 输入。均使用 `gpt-6.1-sol`、medium、非流式、`observe-only-v1`。

plain = `single_agent_plain`，multi = `multi_agent`。文件字节列只认不可变提交快照；完整 oracle 还检查原件、约束、版本、问题或回执等场景证据，不等同于业务验收。耗时为每格 elapsed，输入/输出为保守占用。

| 任务 | 条件 | 文件字节 | 完整 oracle | 工作流/验收 | 场景 | 调用 | 输入 | 输出 | 秒 |
| --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: |
| tool_roundtrip | plain | 通过 | PASS | 验收待定 | 已触发 | 4 | 21,266 | 383 | 34.046 |
| tool_roundtrip | multi | 通过 | PASS | DELIVERED + 原始回执 | 已触发 | 10 | 81,219 | 5,005 | 178.064 |
| partial_amend | plain | 原件/右通过；新左缺失 | MISSING | AMEND 待处理、无进展 | 已触发，转换未完成 | 6 | 36,873 | 565 | 46.999 |
| partial_amend | multi | 原件/目标通过 | PASS | 预算守卫停止、验收待定 | 已触发 | 19 | 175,755 | 5,903 | 265.341 |
| rejected_rework | plain | 原件/目标缺失 | MISSING | 开放用户问题 | 未触发 | 2 | 9,817 | 289 | 27.709 |
| rejected_rework | multi | 原件/目标缺失 | MISSING | 开放用户问题 | 未触发 | 6 | 45,102 | 2,125 | 94.863 |
| exhausted_rework | plain | 失败/阻塞快照缺失 | FAIL | 开放用户问题 | 未触发 | 2 | 10,189 | 278 | 18.301 |
| exhausted_rework | multi | 失败/阻塞快照缺失 | FAIL | 开放用户问题 | 未触发 | 6 | 46,726 | 2,139 | 84.518 |
| delivery_receipt | plain | 目标通过 | MISSING | 验收待定、无确认回执 | 未触发 | 4 | 21,034 | 333 | 32.373 |
| delivery_receipt | multi | 目标通过 | PASS | DELIVERED + 原始回执 | 已触发 | 8 | 54,077 | 2,459 | 99.113 |

合计 **67 次原始调用、502,058 输入、19,479 输出**，各格耗时相加 881.327 秒（不是整轮墙钟）。plain 合计 18 次 / 99,179 输入 / 1,848 输出；multi 合计 49 次 / 402,879 输入 / 17,631 输出。完整 oracle 的六次非零退出全部保留；两格原始 `CONFIRMED` 回执分别属于 multi roundtrip 和 multi delivery。

每格冻结上限 20 次 / 300,000 输入 / 20,000 输出 / 720 秒，Worker 累计上限 16。系统冻结 runtime retry **2**、provider codec retry **0**，执行授权每批 `max_calls=1` 且 `max_total_calls=1`，没有内部重试余量；本轮实际 paid retry **0**，后续独立模型轮次不冒充重试。67 次原始费用均为 `null`，合计费用未知，不写成零。G3 累计 67/600 次、502,058/12,000,000 输入；到 v1 累计 263/3,000 次、3,822,119/60,000,000 输入，包含历史运行。

plain roundtrip 字节和 oracle 通过仍只有提交，验收待定；plain delivery 的 final 字节正确，但缺原始确认回执，完整 oracle 为 MISSING。plain AMEND 确已提交，随后无新 left 修订快照，保留转换未完成的结果。

multi AMEND 的原件和目标字节通过，但最后一批在凭据读取前被原模型预算守卫拒绝，新增调用为 0；已有验收是 `SUPERSEDED`，当前左右任务均未完成验收，右任务目标修订 2、已应用修订 1，不能写成交付。两格确认交付以前的 show 为 RUNNING，随后原始 ack 为 CONFIRMED、封存状态为 DELIVERED；补充记录解释时间顺序，没有改写原结果。

两种 rejected_rework 都在捕获不合格初稿的真实性/最终验收要求处先提问，没有原始负面审核或纠正，也没有实际尝试提交守卫；场景未触发，不能据此断言产品缺陷。两种 exhausted_rework 有开放问题，但缺原始失败/阻塞审核快照；提问不证明返工耗尽，完整 oracle 的 FAIL 保留。

私有证据索引（本机，不随仓库分发）：`/mnt/d/codex-workspace/wls861-g2-ready-20261117-9_tfsmtv/g3-b1-analysis/`，`aggregate-results.json` SHA-256 `bf1e485b821b3b5d60776f2f0bb6b0c0131bb62cd1335b8fe11d5f4cdaad4914`。汇总关联逐格结果与时间/守卫补充；`analysis-completion-receipt.json` 记录十格副本及捕获校验，根目录 `budget-after-g3-b1.json` 记录累计额度。原始日志、数据库、凭据和私有控制器均不入 Git。

**G3-C1（#154 已合入）：** 显式 `initial_plan.mode: fixed-single-task-v1` 只省首次 Dispatcher 规划；产品增删 94 行、测试 171 行，同一有效文件任务总调用 7→6、管理 5→4。排除一次实际文件写入后的固定开销为 6→5，该切片原 6→4 目标未达的记录保留。原始失败/修正证据在私有 `g3-c-evidence/result.md`；PR154 核心、源码及 D-40 25/25 已通过，精确输入与托管计时见[当前交接](../reviews/current-handoff.md#验证与证据)。

**G3-C2（#155 已合入，基于 main `2213c27`，未增加真实调用）：** 新增显式 `initial_work_order.mode: fixed-initial-work-order-v1`，必须同时选择 C1。合法初始 TeamLead claim 按冻结正文、完整原约束及当前 actor/binding/CAS 调用已有 `issue_instruction`，仍须 Worker 确认；任务/计划/模板/claim 不匹配时阻塞，AMEND、返工及已有说明保留原路径。产品增删 **114 行**，C1+C2 **208/250**；测试 **173 行**，另有 CTest/CI 注册维护 11 行，合计仍低于 C2 产品两倍。下面是同一 `result.txt` 有效文件任务、同一 brief/工单语义和最终字节的本机 FakeModel 对照：

| 配置 | 总调用 | 管理调用 | Worker 调用 | 固定开销（排除一次写入） | 宿主 context 字节 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 原角色隔离流程 | 7 | 5 | 2 | 6 | 348,146 |
| 仅冻结初始计划 | 6 | 4 | 2 | 5 | 305,374 |
| 冻结初始计划＋初始工单 | 5 | 3 | 2 | 4 | 259,588 |

Worker 始终各执行一次 `fs_write` 和 `submit`；TeamLead 检查、Dispatcher 验收、验收后独立经验报告仍分别调用，候选阶段无经验 job。固定开销合成实测 **6→4** 采用了独立初始工单替代方案，原 F-29“检查与经验同响应”机制未实施。字节为各次原始持久请求 envelope 中 `context` 的 canonical JSON UTF-8 大小之和，不是 provider wire bytes、真实 token、费用或性能结论。冻结六格式、默认值、Store 410、SQL 和工具 Schema 不变；没有 B2、真实模型或 G3 出口验收。

配置、五个 acceptance 分组、38 项契约和源码检查已通过；初始化分组按 D-20 独立注册，实际 96 项 CTest、26 组受影响 ASan 选择器通过一致性核对。C2 最终 head 核心 288/288、源码及 D-40 26/26 已核验通过；实际输入与计时见[当前交接](../reviews/current-handoff.md#验证与证据)。完整命令、初次段错误和两次负例夹具失败、修正及最终结果只在私有 `g3-c2-evidence/result.md` 和相邻原始日志维护；下一步继续 E-9，再做 B2。

**G3-E9 第一切片候选（未增加真实调用）：** 新增 `FileBatchHistoryContext`，要求原请求/config SHA、Worker、structured-v3 与 fixed-role-file-batch-v1；校验原 attempt/response/feedback 和双调用的身份、顺序、operation/result/哈希关系，再为各工具投影自己的完整结果。原始 native payload/ID/nullable usage 不改写；UNKNOWN、replay、缺观察不能成为成功或可折叠回合，已知部分失败仍为 FAILED。生产调用保持关闭的默认上下文，新配置值及派发入口未开放。表示层不独立认证外部自签哈希数据，下一切片必须从原冻结 Store 配置接入和独立重算。

产品增删 **191 行**，测试 **301 行≤382**，两 PR 的 600 行总限余 409 行；128 KiB response/512 KiB aggregate 仅为表示上限，执行前实际容量证明仍未完成。本机八套 Release、最终协议 67/67、38 契约通过；唯一修改的 C++ 测试已在 26 组选择器内。当前提交的核心/源码/D-40 与一次有界复核仍待完成，精确输入和原失败保留见[交接](../reviews/current-handoff.md#验证与证据)。没有 B2 或新的真实效果/性能结论。

E9 表示层 [#157](https://github.com/wmqfl861/wls861/pull/157) 已合入 main `8018622`。最终 head 的[核心](https://github.com/wmqfl861/wls861/actions/runs/44177730029) 294/294、[受影响 ASan](https://github.com/wmqfl861/wls861/actions/runs/44177804373) 26/26、[源码](https://github.com/wmqfl861/wls861/actions/runs/44177729924)及有界源码复核通过；ASan 最高时限占比 32.13%。生产批量配置/派发仍关闭，真实批量效果和 B2 未执行。下一候选包含相同执行边界的取消 token 贯通及其完整回归，全部增删计入剩余 409 行产品预算；保留测试两倍上限，未通过削减断言或填充产品行满足比例。

**G3-E9 执行切片（本机完成，托管门禁另记）：** `fixed-role-file-batch-v1` 仅对新冻结的 Worker/structured-v3 请求开放两个不同规范相对目标的顺序 `fs_write`；供应方 profile 必须显式允许双工具，单调用沿用原路径。批次不包含 mkdir/patch、相同目标依赖或 `/work` 别名。原 config SHA/actor/binding 贯通 full/window/append/fold，Store 预约独立重算；默认值、六个旧格式取值、Store410及发布SQL不变。

任一 prepare 前验证全组参数、权限、路径与创建/CAS条件、合计工作区/清单容量、原响应和完整反馈/历史的实际序列化上界。只读 Store 前检也要求当前至少剩两次工具额度及两个操作槽，保持事件/结算预留；之后仍逐项执行原授权、CAS和首次派发。只有本批新的、已接受 SUCCEEDED 才推进；第二 CAS 仅随本批 fresh prepare递增。取消、期限、并发变动、部分失败、UNKNOWN与存储故障保留原前缀和明确尾项；一个不可变feedback，重放不派发。零派发的超限拒绝保留完整原应答；不裁剪原件，不把缺反馈记成成功。

实现 `736dfd0` 的产品增删383行（含独立取消提交16行），测试561≤766；CTest注册6行、CI维护8行另列，即使都计测试也为575≤766。PR1+PR2产品合计574/600，计入注册维护则580/600。最终21/21 Release通过，172.11秒，含完整live_workflow55.88秒；337项源/资源输入与提交一致。取消前置保留精确旧生产基线的token红例及同条件绿例；中间编译或故障夹具SQL错误、旧输入验证与最终结果分开保留。

D-20将新增后27.55秒的append-fold整套分为原4例及五个批次组，所有原/新用例、断言和90秒Release基准不变，Make无参仍跑全套；六组Release为7.07/5.11/6.78/4.05/8.23/3.59秒。实际105项CTest注册，D-40选择34个完整套件，覆盖修改的5个C++测试翻译单元及所有六组；这是覆盖核对，最终ASan结果尚需实跑。私有原件在 `g3-e9-runtime-evidence/`（`final-input-03.json`、`final-tests-03.json`、`ctest-mapping-04.json`）及 `g4d-cancel-evidence/`。有界非作者复核和当前提交CI随PR记录；B2与真实批量效果/性能未执行，G3未出口。
