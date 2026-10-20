# 无量山 · wls861

C++ 通用多 Agent 系统，Linux 优先。**当前增量：0.5.0-s1.4；当前蓝图：v0.4.1。** 原 `wls861` 继续提供 0.1.1-m1 合成内核，`wls` 提供持久化 FakeModel 流程和受控 Worker 工具。

本批沿 S1-03/07/08/09/10/12 新增 **Worker 持久提问、逐级回答和上报用户、不可变真实文件提交快照、TeamLead 检查与有界返工**。`pump-fake` 在原持久事件和独立任务基础上推进问答/提交流程，旧问题或旧响应不能为新版本任务授权。TeamLead 检查通过产生候选就绪待办；完整强沙箱正向、真实模型、守护进程、业务验收/交付和可恢复清理继续按任务卡实施。

## 蓝图、计划与审核入口

| 文件 | 用途 |
| --- | --- |
| [实施蓝图 v0.4.1](docs/spec/v0.4.1/01_实施规格_v0.4.1.md) | 当前目标架构、版本关系、运行行为与各切片边界 |
| [实施路线](docs/ROADMAP.md) | 已有 M1 与 S0–S5 的承接、优先顺序和完成条件 |
| [S1 任务卡](docs/spec/v0.4.1/07_S1任务卡.md) | 基于现有代码的增量任务，不从空项目重建 |
| [本次审核修订说明](docs/REVISION-v0.4.1.md) | 每项审核发现如何进入文档、契约或内核修复 |
| [决策记录](docs/spec/v0.4.1/00_决策记录.md) | 用户明确决定、工程默认值与实际接入参数 |
| [S1 基础实现契约](docs/implementation/S1-foundation.md) | 新 C++ Store、配置、模型、Schema、CLI 与明确未实现的部分 |
| [S1.2 Worker 工具契约](docs/implementation/S1-worker-tools.md) | 文件边界、强进程条件、派发/结果/控制、403 迁移与当前限制 |
| [S1.3 事件运行器契约](docs/implementation/S1-event-runtime.md) | 持久待办、404迁移、原子多任务、原反馈、等待与CLI边界 |
| [S1.4 问答与提交契约](docs/implementation/S1-workflow-runtime.md) | 405迁移、原问题/答案、文件快照、团队检查与返工、CLI边界 |
| [本批验证报告](docs/reports/s1-workflow-runtime-validation.md) | S1.4实际构建、Store/运行器/CLI分层证据与限制 |
| [S1.3 历史验证](docs/reports/s1-event-runtime-validation.md) | 持久事件/独立多任务交付时的实际证据 |
| [S1.2 历史验证](docs/reports/s1-worker-tools-validation.md) | 文件/进程/SQLite/CLI交付时的实际证据 |
| [S1.1 历史验证](docs/reports/s1-foundation-validation.md) | 基础层交付时的实际构建与回归 |
| [M1 实现契约](docs/implementation/M1-contract.md) | 原 Journal 内核的 API、持久化、身份和合成验收 |
| [v0.4.1 修订验证](docs/reports/v0.4.1-validation.md) | 前一批审核修正的历史证据 |

原 v0.3 的 33 个文件和本次 v0.4 审核包的 49 个文件分别保存在 [docs/handoff/v0.3](docs/handoff/v0.3) 与 [docs/handoff/v0.4](docs/handoff/v0.4)，有独立 SHA-256 校验。原件里的历史状态、提示词和 AGENTS 文件只作来源档案；当前开发入口是本页、根 AGENTS 和 v0.4.1 蓝图。

## 当前可以运行什么

`wls` 已有 `config check/explain`、`schema check`、`doctor`、`init/submit`、`show/events/budget/config-snapshot`、`run-fake/pump-fake`、`inbox/history/wait/wake`、`questions/submissions/answer`、`tool-query`、`amend/cancel/pending`。只读命令不会改变执行代次；写者重开会先围栏旧执行，保留未决模型预算与 UNKNOWN。

| S1 模块 | 当前实现 |
| --- | --- |
| SQLite Store | 真正的 C++ WAL/FULL、外键、CAS/事件、独占写者；非空402→403→404→405迁移，原tuple结果/输出/完整反馈与提交字节不可变；独立计划及初始团队原子分配 |
| 配置 | LibYAML 严格解析、完整依赖快照、逐字段直接继承冻结 Root、哈希恢复、来源解释；环境 check 保持为数据 |
| JSON / Schema | 严格 JSON、SHA-256、typed ULID；Valijson Draft-07、本地引用注册表、完整记录与模型输入投影 |
| 模型和上下文 | 有界脚本 FakeModel；实际前缀字节及依赖哈希；完整原文、AMEND、任务和说明；不截断约束 |
| 尝试与预算 | 完整调用输入先持久化，响应保留原 tuple；每次尝试独立计数，错误／迟到也记账，未知用量保留预留 |
| Worker 文件工具 | dirfd/openat2 限定工作目录；分页读取、字面搜索、创建、完整 SHA 覆盖/唯一匹配 patch、原子写入及 UNKNOWN |
| 进程与控制 | 显式 argv/env、固定运行文件 SHA、强 bwrap/cgroup 入口；缺条件拒绝；ack/cancel 精确重放与诊断不被业务门禁锁死 |
| 离线角色流程 | 原单任务run-fake保留；pump-fake按持久待办推进独立多任务，逐项确认，AMEND遗漏保持暂停，后补keep使用同一计划 |
| 等待与历史 | 宿主wait/wake持久化，Store阻断等待中的新调用和业务；原响应/完整反馈/绑定跨运行恢复，缺失明确标记，容量超限先阻止预约 |
| 问答 | Worker发起，TeamLead/Dispatcher逐级回答或附注转发，Root接收用户ANSWER；原文与原绑定不改，精确对象版本和等待代次防旧答唤醒 |
| 提交与团队检查 | 实际完整文件字节/哈希同事务保存；TeamLead看原快照、逐项检查，有界返工推进任务/指令并重ack；CANDIDATE_READY不等于业务验收 |

`wls861 demo` 创建两个独立请求，各自使用 `Root → Dispatcher → TeamLead → Worker`，并行完成合成工作、独立审核、交付回执与资源状态收尾，输出 JSON 证据。演示使用固定字节和符号模型，模型调用数为零。

| 模块 | 已实现范围 |
| --- | --- |
| TaskEngine | 请求与命令分别去重；NEW / AMEND / REPLACE / CANCEL；角色与团队归属检查；条件状态提交 |
| 版本与恢复 | 独立需求、计划、应用、状态、代次与团队指令版本；启动自动隔离旧代次；未知操作必须核对 |
| 控制与执行 | 变更确认和操作取消使用专门控制门禁；确认新说明不解除 UNKNOWN 对新业务的阻断；取消运行操作保留 Unknown |
| DurableJournal | 状态与事件同帧写入，成功前同步；断尾恢复；完整损坏拒绝；独占写者锁 |
| Scheduler | 固定线程池、有界队列；同资源键有序；独立键并行；审核和交接常态预留准入及运行容量 |
| 配置与权限 | 逐字段直接继承请求冻结的 Root 快照；能力不支持时明确拒绝；Worker 与允许列表交集 |
| 候选与交付 | 不复用成果版本；全部指定审核团队通过；受理的生产说明更新使当前候选失效，当前负面审核可撤回批准；匹配回执后才完成 |
| 资源状态 | 业务完成与资源关闭独立；未核对操作阻止停止；清理失败可重试 |
| v0.4.1 契约参考 | JSON Schema、消息示例、SQLite 表结构及 Python 事务参考；用于检验未来模块能否按同一契约连接 |

当前 `TaskEngine` 仍使用 Journal；新的 `wls_s1` 库使用显式 SQLite Store 和配置快照，两者分别验证。Python SQLite／Schema 帮助程序继续作为契约参考；它们的测试通过不代表 C++ 已有完整记忆、审核或清理运行器。新数据库也不把原 Journal 当作 SQLite 导入。

## 构建与验证

需要 Linux、支持 C++20 的 GCC/Clang、Make、Python 3.9+；CMake/CTest 是另一入口。S1 使用 **Boost 1.83+、SQLite 3.37+、OpenSSL 3+ 和 LibYAML 0.2.5 运行库**。Valijson 和 LibYAML 公共头已固定导入，见 [依赖来源与许可证](third_party/README.md)。契约测试需要 `requirements-test.txt` 中的 `jsonschema`，以及 Python 所链接的 SQLite 3.37+。

```sh
git clone https://github.com/wmqfl861/wls861.git
cd wls861
# Ubuntu 24.04 开发依赖；不安装或启动服务。
sudo apt-get install build-essential cmake libboost1.83-dev libsqlite3-dev libssl-dev libyaml-dev python3-venv
python3 -m venv .local/test-venv
.local/test-venv/bin/python -m pip install -r requirements-test.txt
make -j2 test PYTHON=.local/test-venv/bin/python
./build/wls861 demo
make -j2 ubsan PYTHON=.local/test-venv/bin/python
```

如果所用 Python 已有测试依赖，可直接指定它。本批使用独立环境 `.local/test-venv/bin/python` 安装仓库固定的测试依赖。构建不会联网获取 C++ 源码；固定导入的上游文件有逐文件哈希清单。

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$PWD/.local/test-venv/bin/python"
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

仅构建 C++ 程序可使用 `make -j2`，或 CMake 的 `-DBUILD_TESTING=OFF`。只核对契约用 `make verify-contracts PYTHON=...`；原件核对用 `make verify-handoff PYTHON=...`。

完整测试入口包含 C++ 模块、CLI 演示、两个原始包校验及跨模块契约回归。严格编译启用主要警告和 `-Werror`，GitHub Actions 对 Release 与 UBSan 两种构建运行相同检查。原 51 项产品验收仍保留历史“未执行”，每批实际证据独立记录。

## 运行一次持久化离线流程

在仓库根目录执行；`--db` 指向你选定的运行数据路径，以下使用被 Git 忽略的开发目录：

```sh
mkdir -p .local/demo
./build/wls config check
./build/wls config explain --role worker
./build/wls submit --db .local/demo/request.db --key s1-example \
  --text-file tests/fixtures/s1_request.txt \
  --constraints-file tests/fixtures/s1_constraints.json
```

将 JSON 输出中的 `request.request_id` 作为后续 `--request` 参数：

```sh
./build/wls run-fake --db .local/demo/request.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s1_fake_initial.json
./build/wls amend --db .local/demo/request.db --request req_YOUR_RETURNED_ID \
  --text-file tests/fixtures/s1_amendment.txt \
  --constraints-file tests/fixtures/s1_amended_constraints.json
./build/wls run-fake --db .local/demo/request.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s1_fake_amended.json --disposition update
./build/wls show --db .local/demo/request.db --request req_YOUR_RETURNED_ID
./build/wls events --db .local/demo/request.db --request req_YOUR_RETURNED_ID
```

`req_YOUR_RETURNED_ID` 是待替换占位符。脚本中的 token 和费用是明确的合成用量；实际供应商调用数为零。note 是数据库中的操作记录，成功不等于业务成果已验收或交付。初始 `--constraints-file` 省略时目录为空，CLI 不会宣称已从自由文本抽取完整约束；AMEND 必须提供整份有效约束目录。运行目录不同时使用 `--source-root /path/to/wls861`。

## 运行真实文件工具

以下示例在一个独立开发目录中创建并修改 `result.txt`，固定模型脚本不访问外部服务。数据库放在工作目录之外；同一团队的目录身份和执行策略首次绑定后不自动改变。

```sh
mkdir -p .local/s12/work
./build/wls submit --db .local/s12/runtime.db --key s12-file-example \
  --text-file tests/fixtures/s12_request.txt \
  --constraints-file tests/fixtures/s12_constraints.json
./build/wls run-fake --db .local/s12/runtime.db --request req_YOUR_RETURNED_ID \
  --workspace .local/s12/work --worker-calls 3 \
  --script tests/fixtures/s12_fake_workspace.json
```

预期文件内容为 `verified from S1.2` 加换行；总共 5 次 Fake 调用，两个文件写操作和一次只读操作。覆盖已有文件需要正确旧 SHA，重复使用这个新建示例不会覆盖已经存在的结果。将输出中的 `task_id` 用于只读查询：

```sh
./build/wls tool-query --db .local/s12/runtime.db --request req_YOUR_RETURNED_ID \
  --task task_YOUR_RETURNED_ID --workspace .local/s12/work \
  --tool fs_read --input tests/fixtures/s12_read_query.json
./build/wls doctor
```

进程工具需要另行准备的强环境及 [manifest](config/examples/sandbox-manifest.json)。该样例故意没有 launcher SHA、可写 cgroup 委派和运行文件，doctor 会列出缺项，run 会拒绝启动。实际使用前填写 bwrap 和每个运行文件的 SHA-256、白名单目标及已委派路径，并在新请求的首次工作目录绑定前传入 `--sandbox-manifest`。程序不会安装 bwrap、修改 user namespace/AppArmor 或创建宿主委派。完整参数与边界见 [Worker 工具契约](docs/implementation/S1-worker-tools.md)。

## 运行持久事件与两个独立任务

以下使用新增的确定性脚本，各任务各有一个目录。调用次数达到上限时，后续工作仍在 inbox 中；示例输出不声明完成业务验收或交付。

```sh
mkdir -p .local/s13/alpha .local/s13/beta
python3 - <<'PYMAP'
import json
from pathlib import Path
root = Path('.local/s13').resolve()
(root / 'workspaces.json').write_text(json.dumps({
    'alpha': str(root / 'alpha'), 'beta': str(root / 'beta')
}))
PYMAP
./build/wls submit --db .local/s13/runtime.db --key s13-events \
  --text-file tests/fixtures/s13_request.txt \
  --constraints-file tests/fixtures/s13_constraints.json
./build/wls pump-fake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s13_fake_initial.json \
  --workspaces-file .local/s13/workspaces.json --worker-calls 2
```

第一次 pump 在 alpha/beta 目录各新建 `r1.txt` 并读取一次，共 7 次 Fake 调用。下面的变更只处置 alpha，beta 保持暂停；随后显式 keep beta，仍要求它取得新说明和 ack：

```sh
./build/wls amend --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --text-file tests/fixtures/s13_amendment.txt \
  --constraints-file tests/fixtures/s13_amended_constraints.json
./build/wls pump-fake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s13_fake_amended.json \
  --workspaces-file .local/s13/workspaces.json \
  --dispositions-file tests/fixtures/s13_update_alpha.json
./build/wls inbox --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID
```

补 beta 时，使用只包含 beta 后续步骤的脚本，避免再次要求 alpha 执行其文件新建动作。显式 keep 继续使用当前计划：

```sh
./build/wls pump-fake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s13_fake_keep_beta.json \
  --workspaces-file .local/s13/workspaces.json \
  --dispositions-file tests/fixtures/s13_keep_beta.json
```

只读历史和持久暂停的入口为：

```sh
./build/wls history --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --role worker --task tsk_YOUR_RETURNED_ID
./build/wls wait --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --task tsk_YOUR_RETURNED_ID --reason '等待宿主补充当前任务资料'
./build/wls show --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID
./build/wls wake --db .local/s13/runtime.db --request req_YOUR_RETURNED_ID \
  --task tsk_YOUR_RETURNED_ID --wait-generation 1
```

`--task` 与 `--wait-generation` 使用实际输出值。wake 不代替新的 AMEND 处置、说明确认或 UNKNOWN 核对，也不能绕过尚未回答的 blocking question。脚本分组、部分目录和原事件语义见 [S1.3 契约](docs/implementation/S1-event-runtime.md)，新的问答/提交行为见 [S1.4 契约](docs/implementation/S1-workflow-runtime.md)。

## 运行提交、检查和返工

下面的固定脚本在独立 alpha 目录写第一版文件，提交完整快照，接收 TeamLead 返工说明，再写第二版并重新提交检查。该示例没有外部模型调用。

```sh
mkdir -p .local/s14/alpha
python3 - <<'PYMAP'
import json
from pathlib import Path
root = Path('.local/s14').resolve()
(root / 'workspaces.json').write_text(json.dumps({'alpha': str(root / 'alpha')}))
PYMAP
./build/wls submit --db .local/s14/runtime.db --key s14-rework \
  --text-file tests/fixtures/s14_request.txt \
  --constraints-file tests/fixtures/s14_constraints.json
./build/wls pump-fake --db .local/s14/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s14_fake_rework.json \
  --workspaces-file .local/s14/workspaces.json --worker-calls 4
./build/wls show --db .local/s14/runtime.db --request req_YOUR_RETURNED_ID
./build/wls submissions --db .local/s14/runtime.db --request req_YOUR_RETURNED_ID \
  --task tsk_YOUR_RETURNED_ID
```

任务 ID 取实际输出。提交路径使用 `/work/...`；文件工具参数仍使用相对路径。每次最多 8 个文件、每个 64 KiB、合计 256 KiB，必须完整捕获，超限拒绝。旧提交字节和哈希不会随工作目录之后的修改而变化；TeamLead 通过只产生 `CANDIDATE_READY`，不表示业务已经验收或交付。

下面用另一个数据库和 alpha/beta 两个独立目录演示提问。alpha 的 Worker 等待答案时，beta 仍可写文件；脚本随后将 alpha 的原问题从 TeamLead 转给 Dispatcher，再转给用户：

```sh
mkdir -p .local/s14-questions/alpha .local/s14-questions/beta
python3 - <<'PY'
import json
from pathlib import Path
root = Path('.local/s14-questions').resolve()
(root / 'workspaces.json').write_text(json.dumps({
    'alpha': str(root / 'alpha'),
    'beta': str(root / 'beta')
}))
PY
./build/wls submit --db .local/s14-questions/runtime.db --key s14-question-route \
  --text-file tests/fixtures/s14_request.txt \
  --constraints-file tests/fixtures/s14_constraints.json
./build/wls pump-fake --db .local/s14-questions/runtime.db --request req_YOUR_RETURNED_ID \
  --script tests/fixtures/s14_fake_question_route.json \
  --workspaces-file .local/s14-questions/workspaces.json --worker-calls 1
./build/wls questions --db .local/s14-questions/runtime.db --request req_YOUR_RETURNED_ID
./build/wls answer --db .local/s14-questions/runtime.db --request req_YOUR_RETURNED_ID \
  --question q_YOUR_RETURNED_ID --expected-version QUESTION_STATE_VERSION \
  --text-file tests/fixtures/s14_answer.txt
```

`q_YOUR_RETURNED_ID` 和 `QUESTION_STATE_VERSION` 分别替换为查询得到的问题 ID 与 state_version；文本与 `--selected-option` 至少给一项。已答、过期、跨请求或未上报用户的问题不能被此入口重新授权，错误输入在打开写者前检查。回答只结束该问题的等待，恢复后的新说明/ack 和 UNKNOWN 检查继续生效。

## 后续实施边界

当前内核是受信宿主进程内 API，`Actor` 由宿主构造。本机 CLI 不提供远程认证；诊断速率限制在单进程内，写者持租约期间没有第二个 CLI 的 socket 取消通道。文件容量是代理的逻辑上限，不是任意进程磁盘配额；合作锁不防任意同 UID 宿主绕过。强进程正向测试、残留 cgroup 恢复及完整 SBX/SEC 验收仍需独立证据。

下一批将候选就绪待办接入当前版本业务验收、交付包、回执、经验与可恢复清理，再推进wlsd/API。跨功能独立审核、DAG和全局审核双容量按S2推进；S1-06/07强环境正向与残留恢复保持独立验收。真实模型协议在外部端点、凭据引用、能力和预算齐备后独立接入。配置样例保持 FakeModel、记忆关闭、真实调用关闭、付费重试为零。
