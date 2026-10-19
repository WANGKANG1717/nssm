# SvcPilot

SvcPilot 是面向现代 Windows 的轻量级服务监督器：它把普通应用程序作为原生
Windows 服务运行，并通过声明式配置、健康探测、故障恢复、安全审计和机器可读
接口管理完整生命周期。

项目源自 NSSM 的服务包装思路和部分代码，但已经采用新的产品边界与工程基线。
SvcPilot 不是原版 NSSM 的兼容发行版，也不以恢复旧命令、旧配置或旧 GUI 为目标。

## 项目现状

SvcPilot 当前处于积极开发、尚未投入生产的阶段，适合继续进行集成测试和接口收敛，
暂不建议作为稳定版本部署到关键业务。

已经具备：

- 原生 Windows 服务安装、更新、删除与生命周期控制。
- 支持注释的严格 TOML 声明式配置，以及 `validate`、`plan`、`apply`、`export` 工作流。
- Readiness、Health Check、Watchdog 式进程监督和可配置恢复策略。
- Job Object 资源治理、进程树跟踪及 CPU、内存、进程数统计。
- 标准输出/错误重定向、日志轮转和运行时在线轮转。
- 配置与运行路径 ACL 审计、Service SID、虚拟账户和安全默认值。
- 稳定的 JSON 输出和退出码，供脚本、CI/CD 及未来 Web 管理端调用。

当前 Debug 和 Release 构建各有 6 项 CTest 测试通过，GCC `-fanalyzer` 无诊断；
管理员权限下的 SCM 生命周期测试由 GitHub Actions 承担。当前仍有两个发布阻塞项：

1. 在线日志轮转状态在控制、日志和关闭线程之间尚未完全同步。
2. 磁盘满或配额错误后的日志重试等待不能被停止事件立即打断，可能延迟服务关闭。

在这两个问题修复并完成压力测试前，不应创建稳定版本标签。详细评估见
[`docs/repository-audit.md`](docs/repository-audit.md)。

## 产品原则

- **CLI 优先**：核心只提供命令行和 Windows 服务运行时，不内置 Win32 GUI。
- **声明式管理**：配置文件是期望状态，先校验和预览，再原子应用。
- **自动化友好**：JSON 输出、稳定退出码和无交互模式是公开接口的一部分。
- **安全默认**：不保存明文密码，不默认使用 LocalSystem，敏感路径必须通过 ACL 审计。
- **职责分离**：监督核心不依赖 UI；未来 Web 应用通过受控管理接口调用核心能力。
- **基线清晰**：只维护 Windows 10+、x64、Unicode 和 MinGW-w64 GCC 这一条路径。

## 支持范围

- Windows 10 或更高版本，仅 x64。
- Unicode 与显式 Win32 Wide API。
- C++20。
- MinGW-w64 GCC 14 或更高版本。
- CMake 3.25+、Ninja、`windmc` 和 `windres`。

不支持 Visual Studio/MSVC 工程、32 位构建、ANSI/MBCS、交互式服务、原版 NSSM
GUI，以及旧 `install/edit/get/set/reset/unset/dump` 命令。

## 命令名称

产品名称是 **SvcPilot**，可执行文件和命令名称是较短的 **`svp.exe`**。将其所在目录
加入 `PATH` 后，本文统一使用 `svp` 调用。项目没有生产用户，因此不提供 `nssm.exe`
别名或旧标识兼容层。

## 构建

使用 MSYS2 UCRT64 安装工具链：

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-binutils
```

在 PowerShell 或 UCRT64 终端中构建并测试：

```powershell
cmake --preset mingw-debug
cmake --build --preset mingw-debug
ctest --preset mingw-debug

cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-release
```

产物位于 `out\Debug\win64\svp.exe` 和 `out\Release\win64\svp.exe`。
MinGW 运行库静态链接，Windows 系统库仍由操作系统提供。Release 构建不嵌入时间戳，
便于可复现构建。

本地便携 MSYS2 也可以放在已忽略的 `.tools\msys64`：

```powershell
$toolBin = (Resolve-Path '.tools\msys64\ucrt64\bin').Path
$env:PATH = "$toolBin;$env:PATH"
```

GitHub Actions 同时构建和测试 Debug/Release，验证 x64 PE 与静态 MinGW 运行库，
并生成 ZIP、SHA-256 校验文件和 SPDX 2.3 SBOM。配置签名 secrets 后，Release
作业还会签名并验证 Windows 可执行文件。

## 快速开始

先验证并预览配置，再在管理员终端中应用：

```powershell
svp validate --output=json .\service.toml
svp plan --output=json .\service.toml
svp apply --output=json .\service.toml
svp credential set MyService '.\service-user'
```

查询和控制服务：

```powershell
svp start MyService
svp reload-or-restart MyService
svp status MyService
svp show --output=json MyService
svp is-active MyService
svp is-failed MyService
svp reset-failed MyService
svp logs --lines 200 --follow MyService
svp stop MyService
svp export --output=json MyService .\backup\MyService.toml
svp remove MyService confirm
```

主要命令：

- `validate <config>`：校验 TOML 结构和业务规则。
- `plan <config>`：只读比较期望配置与 SCM 当前状态。
- `apply <config>`：创建服务或原子更新服务配置。
- `export <service> <path>`：导出已登记的声明式配置。
- `start|stop|restart <service> [...]`：批量控制服务生命周期。
- `reload <service> [...]`：运行配置声明的 `ExecReload`，不重启主进程。
- `reload-or-restart <service> [...]`：支持重载时运行 `ExecReload`，否则回退为重启。
- `status <service>`：显示面向人的 SCM 和监督器运行时状态。
- `show [--output=json] <service>`：输出稳定的机器可读属性。
- `is-active [--quiet] <service> [...]`：查询运行状态，非活动时返回退出码 `3`。
- `is-failed [--quiet] <service> [...]`：查询最近一次未恢复的失败状态。
- `reset-failed <service> [...]`：清除持久失败状态，不启动或停止服务。
- `enable|disable|mask|unmask <service> [...]`：管理开机启动和手动启动权限。
- `is-enabled [--quiet] <service> [...]`：返回 `enabled`、`disabled` 或 `masked`。
- `rotate <service>`：触发在线日志轮转。
- `logs [--lines N] [--follow] <service>`：查看统一日志尾部并可持续跟随；持续跟随要求 stdout/stderr 使用同一路径。
- `list-units [--all] [--verbose|--output=json]`：列出服务；`--all` 包含全部 Windows 服务。
- `processes <service>`：列出受监督的进程树。
- `remove <service> confirm`：删除服务，必须显式确认。
- `version`：显示版本、构建信息和架构。

## 声明式配置

最小示例位于 [`tests/fixtures/minimal-service.toml`](tests/fixtures/minimal-service.toml)。
当前 schema 严格固定为 `1`，不会猜测或自动迁移其他版本。

```toml
Schema = 1

[Unit]
Name = "example-service"
Description = "Example Service"

[Service]
ExecStart = 'C:\apps\example\example.exe'
Arguments = ["--serve"]
WorkingDirectory = 'C:\apps\example'
Environment = ["MODE=production"]
Restart = "on-failure"
RestartSec = "2s"
TimeoutStartSec = "90s"
TimeoutStopSec = "30s"
Type = "notify"
WatchdogSec = "30s"
ExecStartPre = [
  { Path = 'C:\apps\example\prepare.exe', Arguments = ["--check"] }
]
ExecStartPost = [{ Path = 'C:\apps\example\announce.exe' }]
ExecStop = [{ Path = 'C:\apps\example\shutdown.exe' }]
ExecStopPost = [{ Path = 'C:\apps\example\cleanup.exe' }]
ExecReload = [{ Path = 'C:\apps\example\reload.exe' }]
MemoryMax = 268435456
TasksMax = 16

# Windows SCM 特有设置集中在此，不伪装成 Linux 语义。
[Windows]
Account = "virtual"
ServiceSid = "unrestricted"
```

配置文件使用 TOML；`--output=json` 仅表示 CLI 输出 JSON，便于脚本和自动化程序消费。
`[Unit]` 和 `[Service]` 使用 systemd 熟悉的指令命名；`[Windows]`
明确表达服务账户和 Service SID 等 SCM 特有能力。启动策略与运行配置分离，
由 `enable/disable/mask/unmask` 管理。时长接受
`500ms`、`10s`、`5min`、`1h` 等可读写法；Windows 路径建议使用 TOML
单引号字面量。

生命周期命令按数组顺序串行执行，使用独立的 kill-on-close Job Object，且不会经过
shell 解析。`ExecStartPre` 或 `ExecStartPost` 非零退出、超时会让启动失败；显式停止时
先运行 `ExecStop`，等待主进程自行退出，超过 `TimeoutStopSec` 后按控制台、窗口、线程、
进程树的顺序强制终止。`ExecStopPost` 在进程树清理后运行，其失败会记录但不会阻止服务
进入 stopped 状态。主进程意外退出时只运行 `ExecStopPost`，不会错误地调用
`ExecStop`。

`Type = "notify"` 会向应用注入 `SVP_NOTIFY_PIPE`。应用连接该本机命名管道并发送
UTF-8 消息 `READY=1` 后服务才进入 running；配置 `WatchdogSec` 后还须至少按该间隔
发送一次 `WATCHDOG=1`，超时会终止受监督的 Job Object 并进入恢复流程。管道拒绝远程
客户端，并以 SYSTEM、Administrators、服务进程所有者可访问的保护 ACL 创建。

声明式命令统一使用以下退出码：

| 退出码 | 含义 |
| ---: | --- |
| `0` | 成功 |
| `1` | 配置校验失败 |
| `2` | 参数错误 |
| `3` | 权限不足 |
| `4` | 配置冲突 |
| `5` | Windows/SCM 错误 |
| `6` | 路径安全审计失败 |

`--output=json` 模式不会弹出 UAC，便于 CI/CD 确定性处理失败。

## 安全模型

`apply` 和服务启动时会审计配置文件、应用程序、工作目录、生命周期命令、命令探针及日志目录的
Windows ACL。受信对象必须由 SYSTEM、Administrators 或 TrustedInstaller 控制，
且不能向其他主体授予修改权限。

服务默认建议使用独立虚拟账户和 Service SID，而不是 LocalSystem。配置不接受明文
密码；需要域身份时优先使用 gMSA。确需普通用户账户时，先用交互式
`svp credential set <service> <account>` 安全输入。凭据由 Windows DPAPI 机器作用域
加密，绑定服务名和账户名，且仅存于受保护的 SvcPilot 凭据注册项；不会进入 TOML、命令行或日志。
涉及 SCM、注册表、进程、令牌和路径的 Win32
调用集中在平台层，临时句柄使用轻量 RAII 封装。

## 运行时状态

`svp show --output=json <service>` 返回 SCM 状态、包装进程 PID、子进程 PID、运行时长、
Readiness、Health Check、恢复重启次数，以及 Job Object 的 CPU、内存和进程数。
运行时数据通过只读 ACL 的共享内存快照发布；服务停止或快照不可用时，
`runtimeAvailable` 为 `false`。
失败状态和最后退出码独立持久化在服务注册信息中，即使包装进程已经退出，
`show` 与 `is-failed` 仍可查询；成功启动不会隐式抹去诊断信息，需明确运行
`reset-failed`，与 systemd 的运维习惯保持一致。

## 架构

```text
src/
  app/          程序入口、命令分派和版本信息
  cli/          参数解析、控制台输出和 JSON 输出
  config/       配置模型、解析、校验、导出与应用
  supervisor/
    process/     子进程、Job Object 和进程树终止
    monitoring/  Readiness、Health Check、资源监控与快照
    runtime/     服务状态机、恢复和日志
  platform/     Windows API、SCM、注册表、安全、事件日志与编码
resources/      版本信息、消息目录和 manifest
tests/          单元测试、配置样例和 SCM 集成测试
cmake/          构建、资源编译、打包和验证脚本
```

目标依赖方向为 `app/cli -> config/supervisor -> platform -> Windows API`。核心模块
不依赖界面，CLI 也不直接操作注册表或 SCM。

## 测试与发布

CTest 覆盖版本/架构、Unicode 资源、声明式配置、命令行转义、环境隔离、Job Object、
探针、恢复和安全校验。管理员环境中的
[`tests/integration/service_lifecycle.ps1`](tests/integration/service_lifecycle.ps1)
执行完整 SCM 生命周期：`apply`、`list`、`start`、运行时 JSON 状态、原子 `export`、
`stop` 和 `remove`。

发布前至少需要满足：

- Debug、Release 和静态分析全部通过。
- 管理员 SCM 集成测试通过。
- 日志并发及可中断关闭两个 P1 问题关闭。
- 产物架构、静态运行库、SBOM、校验和与签名验证通过。

## 来源与许可

SvcPilot 基于 Iain Patterson 创建并贡献到 Public Domain 的 NSSM。项目保留必要的
来源说明和第三方许可，详见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
新的名称用于明确本项目已经形成独立产品，不暗示原作者对当前实现提供支持或背书。
