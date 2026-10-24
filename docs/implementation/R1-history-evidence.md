# R1.1：原始历史的有界读取契约

实现版本：0.7.3-r1.1；蓝图v0.4.1；ADR：[067](../adr/067-paged-history.md)。
这是宿主读取层，不是模型上下文窗口，也不是供应商适配器。现有Worker仍调用旧全历史接口。

## 两种读取

`Store::attempt_history_page(actor, after_sequence, through_sequence, limit)`：

- actor由受信宿主构造，限定request、role、principal、task和team，验证当前dispatcher/execution代次。
  支持Dispatcher、TeamLead、Worker；Root不作为该接口的跨任务读取角色。
- `after_sequence`默认0且排他；`through_sequence`为0时捕获该scope当前最大插入序号。
  非零游标必须属于同一scope。`limit`为1..64，默认32。
- `items`包含标识、原绑定、结算状态、反馈指纹/缺失标记，不含完整模型输入、原响应或结果正文。
  `accepted`只表示响应曾满足当时绑定，不表示业务验收通过。
- `has_more`为true时，使用返回的`next_after_sequence`和不变的`through_sequence`继续。
  序号允许间隙；另一个任务的记录不填入间隙。遍历开始后的新增记录不进入原高水位。
- 游标是当前数据库的插入位置，不是永久业务ID；外部重建、VACUUM或另库恢复后应从首页重新枚举。
  持久证据引用使用attempt_id和record_sha256，不依赖rowid。运行时本身不删除或整理这些行。
- `snapshot_kind=insertion_membership_only`：固定成员范围，不冻结状态。原来缺失的反馈可能稍后
  由首次合法写入出现；重复读取同一范围可以观察到它。空范围返回EOF，不用through=0维持一个空快照。

`Store::read_history_feedback(actor, attempt_id, expected_record_sha256, section, offset, max_bytes)`：

- `section`只能是`response`或`result`；`offset`非负且不超过section字节数；`max_bytes`为1..16384。
- 先验证调用归属，再读取已记录反馈。缺失反馈返回`not_ready`，不会重读工作区或再次运行工具。
- 重新计算完整反馈事实指纹，并核对原绑定、原响应与`call_attempts`对应不可变事实一致。
  预期指纹不同返回`conflict`；存储事实损坏返回`corrupt`。仅重算反馈哈希不能伪造原调用绑定。
- 输出是原section的规范化JSON UTF-8字节，不是HTTP线上请求字节，也不是其中某个文件字段的裸字节。
  每片以`content_base64`返回，另带`section_sha256`、`size_bytes`、`original_binding`、`eof`和`next_offset`。
  允许切在UTF-8字符中间；应先按顺序拼接全部原字节、核对完整SHA，再解码JSON。
- offset等于size时返回0字节和EOF；超出size拒绝。单片最多16KiB；内部仍核验一个完整反馈记录，
  受既有记录解析上限及section的4MiB上限约束，不是数据库底层的流式大对象实现。

这两个接口不修改事件、预算、等待、UNKNOWN、任务版本或执行代次；也不发起Takeover。
读取不会给调用方新的业务执行授权。当前暂不注册为模型工具：后续必须单独定义冻结工具Schema、
原生tool-call ID、控制预算、窗口遗漏指针及最终授权，才能接入Worker模型循环。

## CLI

在已有请求和任务上，以相同本地宿主身份查询（变量填已有ID）：

```sh
./build/wls history-page --db "$DB" --request "$REQ" --role worker --task "$TASK" --limit 16
# 后续页沿用首页返回的through_sequence：
./build/wls history-page --db "$DB" --request "$REQ" --role worker --task "$TASK" \
  --after "$NEXT" --through "$THROUGH" --limit 16
./build/wls history-read --db "$DB" --request "$REQ" --role worker --task "$TASK" \
  --attempt "$ATTEMPT" --record-sha256 "$RECORD_SHA" --section result --offset 0 --max-bytes 8192
```

Dispatcher不传task；TeamLead/Worker必须指定本请求下的task。CLI通过只读Store导出当前scope，
不从用户参数接收任意principal或伪造epoch。错误范围、字段或指纹不会打开写者。
这些本地宿主命令不构成远程认证服务；不得直接暴露给无认证网络客户端。

## 兼容及验收

不新增或修改SQL迁移，不重写历史行，不改变旧`history`和`attempt_history`的完整或失败语义。
已发布的UNKNOWN、AMEND、等待、交付及权限门禁保持不变。

新增C++ 10项、实际CLI 7项，覆盖270次尝试、交错任务和新增记录的高水位、跨角色/任务/请求拒绝、
61,100字节以上的历史结果、中文/emoji/NUL与二进制内容、读取后修改当前文件、缺失/损坏反馈、
自洽假指纹、重开原绑定、等待与UNKNOWN仍阻断新业务、全部数据库表及预算不变。
事实损坏只在临时测试数据库注入，并在读取前恢复原不可变触发器。执行证据见
[验证报告](../reports/r1-history-evidence-validation.md)。
