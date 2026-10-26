# ADR-078：有界 Responses SSE 离线回放

日期：2026-10-26。状态：**决定已固定，实施中；实测结果另记**。节点 **R3.2**，
依赖[ADR-077](077-responses-offline-codec.md)的外部 profile、结构化请求和终态解码。

## 范围与协议

将显式本地字节块增量解析为 SSE，再按 Responses 的文本/function_call 事件子集校验。
处理 CR、LF、CRLF 跨块、多行 data、comment、UTF-8 跨块及初始 BOM；明确限制总输入、
块数、行长、单事件、事件数、输出项目、文本和工具参数。未知或尚不支持的协议项目明确拒绝。
不以忽略未知事件、丢失 reasoning 或多模态项目换取“完成”。

事件必须保持同一 response 身份、合法的 sequence、output/content 索引和 item/call ID。
完整终态与此前已观察的文本、工具名和参数必须一致。终态后新的 data 或第二终态拒绝；
未完成参数、输出 item 的 done 或零散文本都不等于 response completed。
只有完整终态解析与公共 HTTP 解码均成功才产出完成响应，工具执行仍须后续现有宿主门禁。

## 可确定性中断与证据

每个回放块带从起点起算的非递减 elapsed_ms，可显式携带 cancel_before。
回放不 sleep、不创建线程；截止由原请求 timeout 和这些时间输入判定，不伪称测量了网络延迟。
stop token、显式取消、达到 deadline、EOF 缺终态均保留原宿主 tuple，返回中断且不释放部分工具。
协议不一致返回错误，缺失 usage 保持 null；取消不能被解释为供应商退款或已确认未发生费用。

保留有界 raw stream、哈希、已观察事件/部分结果和终态 JSON，输出明确 offline replay、
network_dispatched=false、零工具派发与零重试。不会生成或更新 Store attempt/wire 记录，
不会把输入夹具的状态写成真实运行事实。

## 接口与验证

库接受 profile、原 ModelRequest、有界 chunks、limits 与 stop token；CLI 可从原始 SSE
文件按指定块大小喂入，也可读取显式时间/取消事件的 replay JSON，输入模式必须互斥。
读文件和输出同样有界，含 NUL/错误 UTF-8/重复键/超深结构不能成为有效终态。

验证覆盖每个拆分点、多事件单块、CRLF/多行/中文多字节、交错工具、重复/乱序/错ID、
delta 与终态冲突、断流/取消/deadline边界、终态后迟到、工具参数JSON及各类预算。
不模拟第三方服务通过率，不开启真实调用；真实传输与供应商现场能力另行验收。
官方事件依据为[Responses streaming events](https://developers.openai.com/api/reference/resources/responses/streaming-events)，
核验日期与支持子集见 ADR-077；没有声称支持文档列出的全部事件。
