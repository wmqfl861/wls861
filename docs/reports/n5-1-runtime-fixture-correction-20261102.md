# N5-1：运行时恢复夹具的源码复核修正

时间范围：2026-11-02。本项仅为 **source-review fixture correction**；没有运行 C++ 编译或测试，不是已复现的产品失败。
原源码检查点 `ead72f103b2f0c134dd8f9ce8b7c79a19eac9079` 和原报告
`n5-1-persistent-event-runtime-source-checkpoint-20261102.md` 保留其原时间范围及输入身份。

复核原 `Store::instruction` 实现确认：它返回 `exposed(impl_->current_instruction(t))`。
`current_instruction` 根据原恢复事件和原 instruction body hash 提供合法的当前 epoch 投影；
数据库内原 `instructions` 行保持不可变。新的 empty-S 测试此前将该公开投影跨恢复整对象恒等作为断言，
会把合法 epoch 投影误判为历史修改。

本小改动仅修改 `tests/persistent_event_runtime_tests.cpp`：

- 使用只读 SQLite 查询记录临时 fixture 的完整原 `instructions` 行，包括每列原 SQLite 类型及文本字节。
- 恢复后逐行比较原表字节；分别检查公开投影 body、任务/plan/instruction revision、team 和 requirements 不变。
- 明确检查公开投影的 dispatcher/execution epoch 各推进一次，并与当前 Store binding 一致。
- 空 S 拒绝之后再核原持久行和该合法投影均未发生额外变化。

修改不放宽产品 guard，不删除断言，不把原投影冻结为旧 epoch，也不改 5 秒 barrier 或 180 秒拟议 suite 期限。
方法总数仍为 **13 methods / 1 suite**；独立 legacy capability probe 的源码不变。

修正后的测试源 SHA-256：
`f0354309b330c74f39fa33b9e37fa07819c536b0cab0599283dc77c6237897ff`。
本次 `git diff --check` exit 0。新增源码冻结清单保存在任务私有证据
`n5-1-runtime-source-freeze-02/receipt.json`；后继提交核验在同目录保存，原 freeze-01 不覆盖。

当前构建、测试、sanitizer、远端 CI 及完整实现验收状态均仍未完成。
