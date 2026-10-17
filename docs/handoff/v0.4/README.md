# 无量山（Wuliangshan）

个人用的 C++ 多 Agent 系统：Root → Dispatcher → TeamLead + Worker，Linux 优先。

## 当前状态

**S0（规格定稿）已完成，还没有代码。** 下一步是 S1 最小闭环，见 `docs/07_S1任务卡.md`。

## 阅读顺序

| 文件 | 主要读者 | 内容 |
| --- | --- | --- |
| docs/00_决策记录.md | 你 | 所有已定、默认和延后的决定 |
| docs/01_实施规格_v0.4.md | 你、AI | 唯一权威规格 |
| docs/02_角色行为规格.md | AI | 四个角色的唤醒时机、工具、产出 |
| docs/03_记忆系统v1规格.md | 你、AI | 记忆 v1（S3 实现） |
| docs/04_验收计划.md | 你、AI | 各切片的验收用例 |
| docs/05_需求追踪.md | 你 | 需求 → 规格 → 用例；v0.1 / v0.3 旧编号对照 |
| docs/06_AI协作开发流程.md | 你 | Claude 与 GPT 怎么配合开发 |
| docs/07_S1任务卡.md | 你、AI | S1 的 14 张任务卡 |
| docs/history/ | 你 | v0.3 蓝图审核报告 |

GPT 生成的 v0.1–v0.3 交接包作为历史来源保存在你那里，不放进代码仓库，避免 AI 依据旧文档开发。

## 目录

| 路径 | 内容 |
| --- | --- |
| AGENTS.md、CLAUDE.md | AI 编程助手的工作约定（CLAUDE.md 导入 AGENTS.md） |
| schemas/ | 交接消息的 JSON Schema 和一套示例 |
| db/schema.sql | SQLite 表结构 |
| prompts/ | 各角色的系统提示词 |
| config/examples/ | system.yaml 与功能模板 general 的样例 |
| packaging/systemd/ | wlsd 的 systemd 用户服务 |

## 延后的决定

| 编号 | 事项 | 何时决定 |
| --- | --- | --- |
| P-01 | 首个模型供应商与 API Key | S1-13 开工前 |
| P-02 | 首个正式功能 Agent | S2 开工前 |
| P-03 | 嵌入模型 | S5 前 |
| P-04 | 代码托管位置 | S1-01 开工前 |
| P-05 | 每请求预算数值 | S1-13 开工前 |
