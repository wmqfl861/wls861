# GCC ASan 验证配置对齐记录

范围：从 `1a5ca4f3549e1531066a159b0e410e69b1a57573` 派生独立修复候选，
统一本地与 CI 的 GCC 13.3 Debug ASan 入口。没有修改 src/include/SQL、旧测试、
CMakeLists、历史报告或运行版本；没有合并 PR #1/#2/#3。

## 已确认依据

原 `.github/workflows/ci.yml` 的 CMake ASan 使用 GCC、Debug；原精确提交的
push run `41044026799`，ASan job `124023534030` 日志记录 GCC 13.3.0、
ASAN_OPTIONS 的地址与泄漏检查和完整 89/89。原本地报告记录 Clang 18、Debug
`-O2 -g`、39 项选择中的 29 过/10 超时，并保留 live_workflow 内部 FAIL。

两者配置不同是已知事实；其因果关系尚未证明。不能用 GCC 的成功推断 Clang
必然有问题，也不能把环境差异直接登记为已确认根因。
原始范围见 [边界验证](r2-4-boundary-coverage-validation-20261030.md)。

## 本次修改

- `scripts/run_gcc_asan.py`：全新目录、普通用户、固定 GCC 13.3.0/Debug、隔离继承环境，
  核编译 flags、完整注册/JUnit、非零退出及前后身份；不重试、不改 timeout、不清旧证据。
- `tests/test_run_gcc_asan.py`：验证 runner 的配置与失败处理，包含明确标注的模拟命令流程。
- CI：原 CMake ASan 矩阵项复用同一 runner，保留其原 55 分钟 job 上限和全部产品测试期限；
  另外五路、Make-ASan 原命令及 CPPFLAGS 检查不变，添加失败也上传的文本证据。
- [操作契约](../implementation/gcc-asan-validation.md)：数据盘要求、运行指令和结论边界。

## 本次真实执行及限制

执行位置是本会话的隔离 Linux 容器，**不是用户 Ubuntu 服务器**。
环境：Debian 13、GCC 14.2.0、CMake 3.31.6、Python 3.13.5。
普通用户自检为 UID/GID 1000；附加组 1001。此环境没有 GCC 13.3，未冒充目标构建。

| 实际检查 | 结果 | 证明范围 |
| --- | --- | --- |
| runner 自检，普通用户 | 20/20，exit 0 | 配置隔离、拒绝 root/错误版本、日志和失败保存、注册/JUnit 校验、身份改变拒绝；含模拟流程，不是产品测试 |
| 独立真实 CTest 格式夹具 | 1/1，exit 0 | 当前 CTest 输出的 JSON/JUnit 可被入口解析；不是 wls 或 sanitizer 测试 |
| 实际 GCC 14.2 误用探针 | runner exit 1，NOT_COMPLETE | 仅执行版本探测后拒绝；未编译、未运行产品测试 |
| CI 结构/基线核对 | 通过 | 原 CI blob 为 a325601aa4e210cdcefc029c4dab6e269d0858da；六项矩阵、job 期限及 Make 命令保持 |
| 用户服务器 GCC 13.3 全量 89 项 | NOT_RUN | 当前远程执行通道不可用，不能声称服务器已更新或已通过 |
| 此候选精确提交的远端 CI | PENDING_AT_REPORT_FREEZE | 提交后的实际运行另行记录，不能继承旧提交绿灯 |

源码自检不是第二模型独立审查。当前只完成验证入口与配置层修订；运行时超时根因、
用户服务器 Clang 失败和原本地完整验收状态均未关闭。原故障记录保持不变。

本报告冻结于提交前，最终 SHA、分支和新 CI 状态由发布回执或 PR 记录。
没有真实模型调用、付费 API、生产数据或服务操作；无磁盘挂载、强推或主分支写入。
