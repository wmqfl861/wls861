# Claude 审核修复接续：发布复验

日期：2026-10-23 America/Los_Angeles / 2026-10-24 UTC。实现 **0.7.1-s1.6**；蓝图 **v0.4.1**。
本报告记录接续半成品后的复验，不覆盖[首次修复报告](claude-review-fixes-validation.md)。
原始建议、逐项取舍和未完成范围见[采纳记录](../reviews/claude-disposition.md)。

## 接续来源与实际阻塞

接手时 main 为 `fcdbdd0eff537fa1d157e7207d5803830e8ef0ec`，已有修复保存在
`work/claude-audit-fixes` 的 `1d7467d6599d012517c902dea71a973468bcb0e2`，不是从聊天重建。
恢复自 Actions source artifact **12480053305**，源码 tar SHA-256 为
`ba60f5e642d80aca99be4c43f7503d44d1179ff6583721dea4a70e7925fef973`；恢复的 Git tree
精确等于 `6c46144abe1611e54ccd5bf61803d5fe025ef0d1`。收到的审核文件与仓库归档逐字节一致。

[前次 CI](https://github.com/wmqfl861/wls861/actions/runs/39905526154) 的五种 CMake 构建及
各自33组CTest已经通过，整体失败来自附加的 GCC13 `make asan`：Valijson继续实例化
libstdc++ `<regex>`，在 `-O1` 加 ASan 时触发 `-Wmaybe-uninitialized` 并被 `-Werror` 拒绝。
仅移除应用中的两处regex不足以解决该构建入口的问题。

接续修复提交：`807b0605e8de7fd93538b005bc2c6dc39fab47a0`，源码树
`dde89352a7ab89e0bde5f360b3ee3bc9d78ffe47`。Make ASan 默认 `-O0`，与 CMake Debug 一致；
保留 `-Werror`、地址/泄漏检测和全部测试。优化级别可通过 `ASAN_OPTIMIZATION` 显式设置，
但不宣称GCC13的优化版regex诊断已修复。优化Release、UBSan和Clang检查继续执行。
Make-ASan独立成一个CI作业，避免两个完整sanitizer入口共用一次超时额度。

## 同一代码提交的验证

[CI run 40044150928](https://github.com/wmqfl861/wls861/actions/runs/40044150928) 已观察到全部六项成功：

| 作业 | 结果 |
| --- | --- |
| GCC Release / umask 0022 | 成功 |
| GCC Release / umask 0002 | 成功 |
| GCC UBSan / umask 0002 | 成功 |
| GCC CMake Debug ASan / umask 0002 | 成功 |
| GCC Make ASan / umask 0002 / 命令行CPPFLAGS | 完整目标成功 |
| Clang18 Release / umask 0002 | 成功 |

五个CMake作业各运行全部33组CTest；Make作业独立执行原Make完整测试入口。
测试均要求非root；只有安装开发依赖使用sudo。前次失败记录保留，没有删用例、关闭检查或改vendor文件。
这六项是同一组回归的不同执行方式，不累加为六倍用例或产品验收。

本轮本地另外以 **oai/uid1000、GCC14.2、Python3.13.5/jsonschema4.26.0** 执行：

| 本地检查 | 实测结果 |
| --- | --- |
| CMake Release，umask0002，verbose | 33/33组，退出0 |
| 同一Release产物，umask0022 | 33/33组，退出0 |
| Make ASan，umask0002，保留命令行CPPFLAGS | 完整目标退出0，无地址/泄漏错误 |

本地与托管环境的编译器、Python依赖分别记录，不用GCC14冒充GCC13。计数为
**S1 C++ 310、S1 CLI 78、M1 C++ 52及演示3、参考契约36**。本轮接续的构建修正没有新增产品用例；
相对原始S1.6，已有审核补丁共新增6项S1 C++和9项实际CLI回归。

```sh
# 在源码根目录，以普通用户执行；Python路径按本地已安装依赖指定。
cmake -S . -B build-resume-release -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/opt/pyvenv/bin/python
cmake --build build-resume-release --parallel 2
(umask 0002; ctest --test-dir build-resume-release --output-on-failure -V)
(umask 0022; ctest --test-dir build-resume-release --output-on-failure)
(umask 0002; make -j2 asan BUILD_DIR=build-resume-make \
  CPPFLAGS=-DWLS_REVIEW_USER_CPPFLAGS=1 PYTHON=/opt/pyvenv/bin/python)
```

190个执行输入在复验后仍与冻结清单一致；清单包含本轮Make/CI改动，区别于首次报告的历史156项清单。
新清单SHA-256为 `a0f63cc7b1546e32c44c3391087f73a9b70f46b8f6a695eb60c60e1257831066`。
完整原始日志、环境、结果JSON、清单及开放问题复现脚本在会话附件
`wls861-claude-review-resume-logs.zip` 中。关键原字节指纹如下：

| 文件 | SHA-256 |
| --- | --- |
| `release-tests.log` | `2d7d585664b619e63db15c459cb850abdc78e87d4237e0a3481064ad1b8e97c3` |
| `release-0022.log` | `341bab54520652e120017c0a780ede588a47246513882966973a9eb28306542e` |
| `make-asan.log` | `9365a632a485184c29a98a6c8374776f37f14c4cc49736ff945a3dc37d02bbad` |
| `open-p1-reproduction.json` | `af564259bdbca85a188cd0dc1a696a06fb114e9992fd0369369daa82df0c8a3b` |
| `reproduce-open-findings.py` | `8c3a7ee99f5ede396381a990e184611f77fcc7fd01171641a4c9c51cda589c6e` |

## 开放问题的重新复现

以下是本轮实际复现，不是原报告成绩的转述，也不计为通过的产品能力：

- **#1 大结果历史**：61100字节文本、16次脚本fs_read，上调测试token预算以避免预算先耗尽。
  首次pump记录9次总调用（Dispatcher、TeamLead和7次Worker），下次Worker因
  `quota: JSON record exceeds byte limit` 被阻断；重新pump仍为相同quota，只多一次管理调用。
  报错文字与原报告不同，但历史增长后无法继续的缺口仍在；更正错误类别不是压缩实现。
- **#3 否定验收**：将固定验收判断改为false，任务变为BLOCKED；再pump为0次调用，仍报
  `rework_blocked`。受控Dispatcher后继仍未实现。
- 实际 `config check` 为 valid=true、all_configured_controls_enforced=false、real_calls_ready=false；
  列出16项validated-only设置。披露不意味着墙钟、告警、并发或压缩已开始执行。

#2事件生命周期上限/全库恢复、#4 PREPARED分配恢复仍由源码和原审核支持，未在本轮新增完整实测。
#12 mkdir、#13交付逻辑路径和#14未执行控制继续按路线图推进。本报告不把这些项目改成完成。

## 采纳与交付边界

保持R0可靠性修正、R1结构化模型输入/可恢复上下文、R2长任务与拒绝出口、R2a分配/目录工具、
R3单协议受控真实闭环、R4单Agent对照/并行的顺序；协议回放可与前置修复并行。
私有行purge和完整服务化后置。保留旧内核回归和强隔离要求，不通过弱回退取得通过结果。

本轮仅使用FakeModel和一次性夹具，未调用付费模型或操作生产数据；复核来自同一实施者，
不宣称独立第二模型评审。这里完成的是审核后的首批可靠性修正及优先级优化，不是全部14项修复。
本报告的提交仅增加文档，不改变上述已验证执行输入；最终分支及对应SHA的CI以GitHub记录为准。
