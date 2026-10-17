# AGENTS.md — 无量山（Wuliangshan, WLS）

本文件写给参与开发的 AI 编程助手（Claude、GPT / Codex 等）。项目所有者是唯一的人类成员，负责分配任务、裁决和合并。

## 项目是什么

用 C++20 实现的个人用多 Agent 系统，Linux 优先。四种角色：Root（对外入口，纯代码）→ Dispatcher（每个请求一个）→ TeamLead + Worker（一对一团队，只有 Worker 执行命令和改文件）。单进程守护程序 wlsd：Agent 是协程，Worker 的工具在 bubblewrap 沙箱里执行；命令行客户端 wls。

## 开工前必读（按顺序）

1. `docs/01_实施规格_v0.4.md` —— 唯一权威规格
2. `docs/07_S1任务卡.md` 中分配给你的那张卡
3. 卡片引用的章节，以及 `schemas/`、`db/schema.sql`
4. 涉及角色行为时读 `docs/02_角色行为规格.md`；涉及记忆时读 `docs/03_记忆系统v1规格.md`

不要依据 `docs/history/` 或 v0.1–v0.3 的旧文档开发。

## 当前阶段

S1（最小闭环）。只做任务卡范围内的事；看到卡片之外的问题，写进交接记录，不要顺手改。

## 构建与测试

S1-01 完成后在这里填写准确命令。约定的入口：

```sh
scripts/ci.sh                                   # configure + build + 全部测试
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

`tests/live/` 需要真实 API Key，默认不运行，由项目所有者手动执行。

## 硬性规则

1. 规格优先。发现规格有错或做不到：停下，在 `docs/00_决策记录.md` 的"提议"区追加一条，并在 PR 中说明；不要自行偏离。
2. 字段名、状态名、表名、枚举值与 `schemas/`、`db/schema.sql` 完全一致。
3. 不在数据库事务里等待模型调用、工具执行或网络。
4. 所有状态写入都用 state_version 条件更新；Agent 返回的结果先核对代次和版本再生效。
5. 沙箱子进程用显式环境变量启动，绝不继承 wlsd 的环境（里面有 API Key）。
6. 文件工具一律经过 openat2 路径约束；删除只按团队资源清单，不按路径前缀。
7. 不连真实模型的测试一律用 FakeModel；真实模型测试只放在 `tests/live/`。
8. 不提交密钥、个人路径、数据库文件、构建产物。
9. 新增第三方依赖要写进 `vcpkg.json`，并在 PR 中说明理由。
10. 供应商没有返回的用量记 NULL，不记 0。

## 代码约定

- C++20；异步用 Boost.Asio 协程（awaitable）；JSON 用 nlohmann/json；日志用 spdlog。
- 预期内的失败（校验失败、版本冲突、工具失败）返回 `Result<T>`；程序错误抛异常。
- 命名：类型 PascalCase，函数和变量 snake_case，常量 kPascalCase，文件名 snake_case。
- 代码注释和标识符用英文；面向用户的 CLI 文案用中文。
- 格式以仓库根目录的 `.clang-format` 为准；目录结构见主规格第 16 节。

## 完成一张卡时

- `scripts/ci.sh` 全部通过；
- 任务卡列出的验收用例都有对应测试；
- PR 描述按 `docs/06_AI协作开发流程.md` 第 5 节写交接记录。

## 审查别人的代码时

按 `docs/06_AI协作开发流程.md` 第 4 节的格式输出审查报告；正确性和规格一致性优先，风格问题放进"建议修改"。
