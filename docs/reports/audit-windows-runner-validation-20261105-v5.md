# Windows 监督器夹具入口与结果传递修复验证（v5）

generated_at_utc: 2026-11-05T19:03:13.230978+00:00
scope: 仅 E002 夹具入口/跨 cwd 证据根/N6 结果路径/N9 异常注记四项 harness 修正（FINAL v6，三个属主仓库文件，生产语义改动 0）。本报告只记录到生成时刻为止实际发生的本地验证；后续真实 Windows CI、独立硬审、发布与合并均未发生。

## 1. 基线

- 分支 `fix/audit-windows-evidence-20261105-adp35vg4`，基线 HEAD `6c04265a8a7863fc976a8e7da2631c23bada74b0`，tree `d7cb3cba269b2ec98c0c9775f5be7c89f1e9b6a3`。
- 监督器测试基线 SHA-256 `55b5a6b6e1440da0bfa2bd2ef8a3075fb5a9860afd76cbc2edeb54038a1679be`；生产 `windows_process.py` 全程逐字节保持 `a1fa6e7a4477bc819b53c82edfc0c62133c04f52485a5e5a23eba100187e2c02`（本批后实测一致）。
- 既有 v1–v4 报告、E003、失败 run 原件、全部历史收据未改动。

## 2. 6c04265 两次真实 Windows CI 的既成事实（只读引用）

| run | 事件 / job / attempt | API head | 实际 checkout | 结论 |
| --- | --- | --- | --- | --- |
| 42076583585 | push / 127418273395 / 1 | 6c04265a8a7863fc976a8e7da2631c23bada74b0 | 6c04265a8a7863fc976a8e7da2631c23bada74b0（同 head） | failure |
| 42076594588 | pull_request / 127418314096 / 1 | 6c04265a8a7863fc976a8e7da2631c23bada74b0 | eb7b68ee37a40219b422943e9551b307386985e0（merge） | failure |

两 run tree 均为 `d7cb3cba269b2ec98c0c9775f5be7c89f1e9b6a3`，windows-2022，attempt 1。steps 1–6 成功（capture、runner/supervisor synthetic），step7 native failure，step8/9 skipped，step10 上传成功且每 run artifact_count=1（artifact 13437704765 SHA `c5b56b130e6592adfe78384d0e0559609ab99ca1d3da1cf49ad56b6d08ed54d8`；13438606566 SHA `be0dc88e98cde44ab2041f46b209609e27770463c1033fa98869c739ba7770dc`）。native summary 共 11 项：`startup-abi-nesting` 与 `n7-assign-failure-injection` PASS，N1–N6/N8–N10 九项 FAIL。

真实属性边界正证据（两 startup 原件一致）：三个自有真实可继承句柄、24 字节负载、旧键 0x00020000 返回 FALSE 且即时错误 24，SDK 键 0x00020002 返回 TRUE；真实 CreateProcessW 提交的列表地址等于成功 Update 的列表地址，三 stdio 一致；startup-probe COMPLETED/exit 0/树退出证明/清理 PROVEN_ZERO。该证据只覆盖这两次运行中的此边界，不表示九项失败已闭合、产品 5 套件 34 方法通过或 Win11 验收完成。N7 的 case PASS 不可扩大为其原件中的 LAUNCH_ERROR/树未证明/清理 UNPROVEN 限制。

## 3. 九项失败的共同入口（已观察）与四项发现

九个失败 case 各有独立 313 字节 stderr 原件（SHA 全部 `1e5e281a7a0e105dc8cf78e5f81791b7781257caad75b6e651f6b2e7d3ee2823`）：argparse 要求 `--mode, --evidence-dir`。N6/N10 为 controller exit 2；N9 原件为 `RuntimeError('ctest failed with exit 2')`，其 process 原件显示实际 argv 为 `--fixture hang --ready native-process-native\native-n9\ctest-ready --seconds 45`、cwd 为 work 目录——即相对 ready 将在子进程 cwd 下再解释。本次只把该九项失败归因到夹具 harness 的四项缺陷，不宣称九个生产监督器缺陷，也不预言修复后 native 全绿。

| 发现 | 证据分层 | 修复（仅 harness） |
| --- | --- | --- |
| F6-ENTRY | 两 run 实测 + 静态确认 | 有效既有 fixture 免普通必填旗标分派；unknown/空名为副作用前的参数错误；普通入口仍严格；fixture 优先、parse_known_args 宽松剩余、help exit0、返回语义保持 |
| F6-PATH | 静态确认 + 已观察 argv/cwd 矛盾（解锁后未原生复现） | 证据根按建立时 cwd 一次锚定；不 resolve、不归一化、不折叠 `..`；已存在目录拒绝与绝对输入语义保持 |
| F6-N6-OUTCOME | 静态确认（被入口失败遮蔽） | runner 独占 `supervised-runner-outcome.json`；controller 写独立 `controller-outcome.json` 并嵌入 runner 内容与 sha256；native N6 从 runner 记录验证原三项中断/树字段；未用 `--budget 18` 与既有 timeout/wait 不动 |
| F6-N9-NOTE | 静态确认（被入口失败遮蔽） | router 对原异常已携带的 dict receipt 附同一注记后 bare raise 同一异常；不造 receipt、不改生产；6s/300s 路由参数不动 |

## 4. 本轮本地实施与验证（真实收据）

同文件双重身份如实记录：RED/GREEN 整文件 SHA 必然不同；四个 oracle 符号与 20 项注册表的原字节在 RED-2/GREEN-2 逐字节一致（20 项注册）；生产每态仍为 a1fa。身份规则为符号名 + 原字节段 SHA（基线行号仅辅助）。

- 基线测试文件：`55b5a6b6e1440da0bfa2bd2ef8a3075fb5a9860afd76cbc2edeb54038a1679be`。
- attempt-1 RED：`1e6cc828c9381e619f0d0eb6adddfa09e047fce3386d5aeae1b554b31e57f24b`，17 PASS / 3 FAIL，但 PATH 子检查在基线下以 sentinel 写入 FileNotFoundError 崩溃、未落 facts——oracle 缺陷。该轮全部源码/diff/符号报告/命令收据/case 原件在私有证据 red/ 原样保全，绝不删除或重绑。
- attempt-2 RED-2：`278cb2c298314245469dadb526bfff52fecab3ea62ded45b6c2ddd52e65850c3`，被测 13 符号 13/13 原字节不变；17 PASS / 3 FAIL，四条独立红互不遮蔽：ENTRY（leaf 探针 exit2 且八名全部解析被拒）、PATH（根未锚定、原根无 marker、子 cwd 实际写失败 `FileNotFoundError: [Errno 2] No such file or directory: 'evidence root with space/sentinel-started.txt'`，绝对路径正例通过）、N6（路径相同、内层字节被覆盖）、N9（同对象同 receipt 但无注记）。
- GREEN-2：`367af8d18c19fd95b40c784397b21f6b723c16a35fdd84a65e4f895db51d6bd4`（仓库当前实测 `367af8d18c19fd95b40c784397b21f6b723c16a35fdd84a65e4f895db51d6bd4`）；恰五个计划内符号变更（Recorder, _fixture_ctrlc_controller, _integration_router, native_n6_isolated_console_ctrl_c, parse_args），其余八个（含 main、native_n9 与全部消费 fixture）保持基线字节；oracle 与注册表冻结。
- GREEN 实测四组：监督器 synthetic **20/20 PASS**；runner synthetic `Ran 31 tests in 29.173s` OK；image provenance `Ran 17 tests in 0.493s` OK；bundle capture `Ran 6 tests in 1.997s` OK。四组 exit 全 0。20 为工程工具 case 数，不计入 Linux 95 注册或 native 产品 5 套件 34 方法。
- 每条命令收据含展开 argv/cwd/解释器路径版本 SHA/关键 env/UTC 起止/exit/stdout/stderr 原字节与哈希/五个源文件当时哈希/前后完整 HEAD 与未经 strip 的 porcelain 原字节（base64+SHA+returncode+stderr），读取一律 `git --no-optional-locks`。本轮对 D04-6 的改进即在于此，不回填历史。
- 期限全部未动：synthetic 本地 55s/step 60s、N1–N10 每 25s 与内部 280s、native step 300s/job 900s、build600/ctest300、suite 30/30/30/60/30s、N6/N9/N10 内部等待值；15.0 常量仍仅作耗时记录，未新增比较门。

## 5. 真实 Windows 验证（未发生，PENDING）

修复后的原生结论只能来自下一次自然执行的原生 CI（capture 仍为第一道相关步骤，工作流相对 evidence-dir 传参保持，从而真实跨 cwd 行为得到验证）：startup 属性对照与列表观察继续 PASS；startup+N1–N10 共 11 项须有最终 SHA 原件，N6 须有分离的 controller/runner 两报告及真实事件/原异常/树证明，N9 须实际 TimeoutExpired（非参数 exit2）并带异常 receipt 注记，N10 须真实 KeyboardInterrupt 且清理先于 after-snapshot。6c 两 run 的旧通过不得冒充新最终 SHA 整套通过；本报告无原生执行，不使用 Linux/Wine 代填。后续 native 再失败时保全原件并返回同作者最小诊断。

## 6. 继承披露（未闭合）

H6-1（result 聚合为 finalization 前投影）、H6-2（旧 12:20 非实证写时，原写时 UNKNOWN）、H6-4（README 真实 SHA 以实测值为准）；D03-1..4（系统 tmp 中转、环境首版丢失、错误 manifest、chmod 元数据）不因本轮规范执行而闭合；D04-1..7（失败夹具首轮与迁移映射、手抄预检、空目录/临时副本、未验证 v4 17:08、非 raw porcelain、freeze 范围）全部继承，本轮 attempt-1 的 oracle 缺陷及其保全同样如实披露；hard-review-04 的逐收据常量-only 说明未回填。

## 7. 发布状态（分离）

IMPLEMENTATION_COMPLETE（本三文件与本地证据范围）：是。LOCAL_VALIDATION_COMPLETE（仅本地合成与三组回归）：是。PUSH_COMPLETE：否。REMOTE_CI_COMPLETE：PENDING（未来最终 SHA run）。PR_REVIEW_COMPLETE：PENDING（须另一全新独立硬审）。MERGED：否。Win11 普通用户 NTFS NOT_RUN；历史 image 冲突 UNRESOLVED；私有原件托管 BLOCKED；full_native_runtime NOT_IMPLEMENTED；历史 Clang/live_workflow 保持 OPEN。无真实 WLS/付费产品调用；工程模型请求按各阶段真实收据计数，未知记 unknown。

## 8. 局限

本报告由实施方生成，其实施者自评不是独立审核。本地三态与四组回归不构成原生 Windows 证据；20 工具 case 不构成产品注册；私有证据目录（validation-fixture-entry-01，本地数据盘，未发布）中的收据是上述哈希与结论的原始依据。
