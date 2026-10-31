# 99c9063f 独立审核附件与本地 Agent 接手入口

这是用户要求上传到 GitHub 的三个原始会话附件，内容逐字节保留。
审核对象是 `99c9063f3e29cd6d69c56fb15815715d499a374a`，不是本次附件发布提交。
附件分支：`docs/review-attachments-99c9063f`。

## 阅读顺序

1. [详细整改提示词](wls861_next_fix_prompt_99c9063f.md)：T0–T5 的授权范围、修复约束、验证和交付要求。
2. [完整独立审核报告](wls861_review_99c9063f.md)：两项 P1 发现、实际复现及证据限制。
3. [原始证据 ZIP](wls861_review_99c9063f_evidence.zip)：32 个正文成员与 `REVIEW_MANIFEST.json`，共 33 个普通文本/复现源码成员。
4. [SHA256SUMS](SHA256SUMS)：三个附件原件的 SHA-256。进入本目录后运行 `sha256sum -c SHA256SUMS`。

证据包内首先阅读 `REVIEW.md`、`NEXT_AGENT_PROMPT.md`、`independent_async_repros.cpp` 和 `independent-reproduction.json`。
前两份正文分别与本目录审核报告、详细提示词一致。内层清单记录其余 32 个成员的字节数和 SHA-256；清单本身由外层 ZIP 摘要固定。
上传前已核验成员集合、摘要、普通文件及非可执行模式、UTF-8、无 NUL、无路径穿越/链接，并扫描常见凭据模式。

## 开发基线与资料分离

先在数据盘核验仓库 remote、工作区和最新 PR #10 head/base。
通过 fetch 取得附件分支，再用独立只读 worktree 或 `git archive` 将本目录取到数据盘任务目录；不要覆盖现有开发工作树。
在全新目录中列出并检查 ZIP 成员，然后解压、核验 `REVIEW_MANIFEST.json`。
不要自动执行归档代码来审计其自身；阅读后再按已授权的 T0，在审核基线自己的 headers、生产库和资源上构建复现。
原命令中的 `/mnt/data/...` 是审核容器路径，必须映射为实际数据盘路径，不照搬。

未改动基线 `99c9063f` 的两个反例应命中各自目标断言，退出码 1；构建失败和外层超时不算红例。
修复候选必须执行同一业务条件的绿例，并保留原 18 个异步子例、资源 FIFO、审核预留、Store 首次 claim 和结算守卫。
如果远端有新成果，先审差异和祖先关系，不回退、不覆盖、不强推。附件原件保持不变，新诊断或修正另写文件。

## 审核状态

两项优先修复：R-A3-01（AMEND 精确重放停止新调用）、R-A4-01（跨池资源 FIFO 循环等待）。
[PR #10](https://github.com/wmqfl861/wls861/pull/10) 上的[独立审核留言](https://github.com/wmqfl861/wls861/pull/10#pullrequestreview-5723348769)为 `CHANGES_REQUIRED`。
附件上传不是生产代码修复、不是新一轮产品测试，也不是正式批准或合并授权。
不修改 main，不移动既有 PR 分支，不调用真实模型或付费 API。
