# SvcPilot

SvcPilot（命令名 `svp`）是面向 Windows 10+ 的轻量级服务监督器：把普通应用程序作为原生 Windows 服务运行，并提供接近 systemd 使用习惯的声明式 TOML 配置和 CLI。

> 定位：比 Servy 更轻量、比 NSSM 更可靠、比传统 Windows 服务工具更接近 systemd 的声明式 CLI 管理器。

项目借鉴 NSSM 的服务包装思路和部分代码，但已经形成独立产品，不兼容原版 NSSM 的命令、配置或 GUI。

## 当前成熟度

项目尚未投入生产。普通构建、单元测试、GitHub Actions 管理员 SCM 集成测试和 Windows 虚拟机 13 步验收均已通过；异常注入、长期运行和压力测试仍待在隔离虚拟机执行。在此之前，不建议用于关键生产服务。

技术基线：Windows 10+、x64、Unicode、C++20、CMake 3.25+、Ninja、MinGW-w64 GCC 14+、`windmc`、`windres`。仅提供 CLI，无 GUI、MSVC 和 32 位版本。

## 功能地图

为便于后续取舍，现有能力分为三层。“可选”表示服务不配置它也能运行，并非尚未实现。

### 核心日常能力

| 类别 | 已实现功能 | 可选 | 用途 |
| --- | --- | :---: | --- |
| 服务包装 | 将可执行文件及参数注册为原生 Windows 服务 | 否 | 让普通程序随系统运行 |
| 声明式配置 | 严格 TOML Schema 1、注释、未知字段拒绝、UTF-8 | 否 | 配置可读、可审查、可版本化 |
| 配置工作流 | `validate`、`plan`、`apply`、`export` | 否 | 校验、预览、应用和备份 |
| 配置漂移 | `plan`、`status`、`show` 检查 TOML、SCM 和运行实例 | 否 | 提示重新 apply 或 restart |
| 生命周期 | `start`、`stop`、`restart`，支持批量操作 | 否 | 日常启停 |
| 启动策略 | `enable`、`disable`、`mask`、`unmask` | 否 | 管理自动、手动和禁用启动 |
| 状态查询 | `status`、`show`、`is-active`、`is-enabled`、`list`（`list-units`） | 否 | 排障和脚本检查 |
| 进程监督 | Job Object 管理完整子进程树 | 否 | 防止孤儿进程和不完整停止 |
| 基础恢复 | 永不重启、失败重启、始终重启 | 是 | 崩溃后自动恢复 |
| 基础日志 | stdout/stderr 重定向、可选时间戳 | 是 | 保存应用输出 |
| Windows 集成 | 显示名、描述、服务依赖、工作目录、环境变量 | 是 | 表达常见 SCM 配置 |

### 生产可靠性能力

| 类别 | 已实现功能 | 可选 | 用途 |
| --- | --- | :---: | --- |
| 优雅停止 | 停止命令、超时等待、分阶段终止进程树 | 是 | 保护数据完整性 |
| 生命周期钩子 | `ExecStartPre/Post`、`ExecStop/Post` | 是 | 启动检查、注册、排空和清理 |
| 就绪探测 | process、TCP、HTTP、command、file | 是 | 真正可用后才报告 running |
| 健康检查 | 周期、延迟、超时及成功/失败阈值 | 是 | 发现卡死而非仅检查存活 |
| 主动通知 | `READY=1`、`WATCHDOG=1` 命名管道协议 | 是 | 应用主动报告就绪和存活 |
| 故障恢复 | 指数退避、抖动、频率限制、稳定后重置 | 是 | 避免崩溃循环 |
| 失败状态 | 持久记录故障类型、阶段、退出码、Win32 错误、详情和恢复次数 | 否 | 包装进程退出后仍可定位根因 |
| 资源治理 | 内存、进程数和 CPU 使用率限制 | 是 | 限制失控服务影响整机 |
| 运行时观测 | PID、运行时长、健康、重启数和资源统计 | 否 | 查看服务实际行为 |
| 安全审计 | 配置、程序、钩子、探针和日志路径 ACL 检查 | 否 | 防止低权限篡改高权限服务 |
| 身份隔离 | 虚拟账户、Service SID | 是 | 按服务隔离权限 |
| 安全凭据 | 隐藏输入、DPAPI 加密、账户绑定、受限 ACL | 是 | 安全使用普通用户账户 |

### 高级与自动化能力

| 类别 | 已实现功能 | 可选 | 用途 |
| --- | --- | :---: | --- |
| 热重载 | `reload`、`reload-or-restart`、`ExecReload` | 是 | 不停止主进程地重载 |
| 日志轮转 | 按大小轮转和 `rotate` 在线触发 | 是 | 控制日志体积 |
| 日志查看 | `logs -n N`、`-f`、Ctrl+C 停止 | 是 | 直接查看服务日志 |
| 事件查询 | `events -n N [-o json]` | 否 | 按服务关联 Windows Application 事件 |
| 进程树查询 | `processes` | 是 | 查看全部受管子进程 |
| JSON 接口 | 稳定 JSON 输出和固定退出码 | 是 | PowerShell、CI/CD、未来 Web 端 |
| SCM 全景列表 | `list -a` | 是 | 查看非 SvcPilot 服务 |
| 多账户类型 | 系统账户、虚拟账户、gMSA、普通用户 | 是 | 适配不同 Windows 身份 |

## 快速开始

最小配置只需要服务名和启动命令：

```toml
Schema = 1

[Unit]
Name = "example-service"
Description = "Example service managed by SvcPilot"

[Service]
ExecStart = 'C:\apps\example\example.exe'
Arguments = ["--serve"]
WorkingDirectory = 'C:\apps\example'
```

在管理员终端校验、预览、应用并启动：

```powershell
svp validate .\service.toml
svp plan .\service.toml
svp apply .\service.toml
svp start example-service
svp status example-service
```

新服务默认手动启动。使用 `svp enable example-service` 改为自动启动。删除仅限 SvcPilot 管理的服务，并要求 `svp remove example-service confirm` 显式确认。

## 命令清单

### 配置与生命周期

| 命令 | 作用 |
| --- | --- |
| `validate [--output=json] <config.toml>` | 只读校验 TOML 和业务规则 |
| `plan [--output=json] <config.toml>` | 比较期望配置与当前状态，不写入系统 |
| `apply [--output=json] <config.toml> [...]` | 创建/原子更新服务；多文件按依赖排序、先整体预检，失败时移除本批次已创建服务 |
| `export [--output=json] <service> <file.toml>` | 导出声明式配置 |
| `remove <service> confirm` | 删除 SvcPilot 管理的服务 |
| `start\|stop\|restart <service> [...]` | 批量启动、停止或重启 |
| `reload <service> [...]` | 执行 `ExecReload` |
| `reload-or-restart <service> [...]` | 支持重载时重载，否则重启 |
| `continue <service>` | 继续暂停的 Windows 服务 |
| `enable\|disable\|mask\|unmask <service> [...]` | 设置自动、手动或禁用启动 |
| `reset-failed <service> [...]` | 清除失败状态，不改变运行状态 |

`enable` 对应 SCM automatic；`disable`、`unmask` 对应 manual；`mask` 对应 disabled。

### 查询、日志与凭据

| 命令 | 作用 |
| --- | --- |
| `status [-v\|--verbose] [-o json\|--output=json] <service>` | 显示服务与监督状态 |
| `show [--output=json] <service>` | 输出稳定的详细属性 |
| `is-active\|is-enabled\|is-failed [-q\|--quiet] <service> [...]` | 供人工或脚本判断状态 |
| `list`（`list-units`）`[-a\|--all] [-v\|--verbose\|-o json\|--output=json]` | 列出服务；详细模式显示状态和 PID |
| `processes <service>` | 列出监督的进程树 |
| `logs [-n N\|--lines N] [-f\|--follow] <service>` | 查看并跟随日志 |
| `events [-n N\|--lines N] [-o json\|--output=json] <service>` | 查询该服务的 SvcPilot Windows 事件 |
| `rotate <service>` | 请求在线日志轮转 |
| `credential set <service> <account>` | 隐藏输入并加密保存密码 |
| `credential delete <service>` | 删除保存的凭据 |
| `version` | 显示版本、构建类型和架构 |

日常使用推荐较短的 `list`；`list-units` 作为 systemd 风格的完整别名保留。默认只列 SvcPilot 服务，`-a`/`--all` 包含全部 Windows 服务。通用短参数为 `-v`（详细）、`-o json`（JSON）、`-q`（静默）、`-n`（日志行数）和 `-f`（持续跟随）。

## TOML 配置参考

Schema 1 会拒绝未知 section 和字段。时长支持 `500ms`、`10s`、`5min`、`1h`；Windows 路径建议使用 TOML 单引号。

### `[Unit]`

| 字段 | 必需 | 说明 |
| --- | :---: | --- |
| `Name` | 是 | Windows 服务名 |
| `DisplayName` | 否 | SCM 显示名称，默认等于 `Name` |
| `Description` | 否 | 服务说明 |
| `Requires` | 否 | Windows 服务依赖名称数组 |

### `[Service]`

| 字段 | 必需 | 说明 |
| --- | :---: | --- |
| `ExecStart` | 是 | 主程序绝对路径 |
| `Arguments` | 否 | 参数数组，按 Windows 规则安全序列化 |
| `WorkingDirectory` | 否 | 工作目录，默认主程序目录 |
| `Environment` / `InheritEnvironment` | 否 | `NAME=value` 数组及是否继承环境 |
| `ExecStartPre/Post` | 否 | 主进程启动前后的命令数组 |
| `ExecStop/Post` | 否 | 停止和清理命令数组 |
| `ExecReload` | 否 | 热重载命令数组 |
| `TimeoutStartSec` / `TimeoutStopSec` | 否 | 启停截止时间 |
| `Type` / `WatchdogSec` | 否 | `simple` 或 `notify`；notify 心跳期限 |
| `Restart` | 否 | `never`、`on-failure`、`always` |
| `RestartSec` / `RestartMaxDelaySec` | 否 | 初始和最大重启延迟 |
| `RestartSteps` / `RestartJitterPercent` | 否 | 退避倍数和抖动百分比 |
| `StartLimitIntervalSec` / `StartLimitBurst` | 否 | 窗口内允许的重启次数 |
| `RestartResetSec` | 否 | 稳定运行多久后重置计数 |
| `MemoryMax` / `TasksMax` / `CPUQuota` | 否 | 内存字节、进程数和 CPU 百分比上限 |

钩子格式为 `{ Path = 'C:\path\tool.exe', Arguments = ["..."] }`。钩子按顺序执行，不经过 shell，并在独立 kill-on-close Job Object 中运行。

### `[Windows]`

| 字段 | 必需 | 说明 |
| --- | :---: | --- |
| `Account` | 否 | `system`、`localService`、`networkService`、`virtual`、`gmsa`、`user`；默认 `virtual` |
| `AccountName` | 条件 | gMSA、普通用户必需；虚拟账户可显式指定同名账户 |
| `ServiceSid` | 否 | `none`、`unrestricted`、`restricted`；默认 `unrestricted` |
| `CPUMonitorInterval` | 否 | CPU 采样间隔 |
| `CPUViolationThreshold` | 否 | 连续超限多少次后判定违规 |

启动策略不写入 TOML，由 `enable/disable/mask/unmask` 管理，避免部署配置意外改变开机启动策略。

### `[Readiness]` 与 `[Health]`

两者均支持五类探测：

- `process`：主进程存活。
- `tcp`：`TCP.Host`、`TCP.Port`。
- `http`：`HTTP.URL`，可设置 `MinimumStatus`、`MaximumStatus`。
- `command`：`Command.ExecStart`、`Command.Arguments`。
- `file`：`File.Path`，可设置 `MaximumAgeSec`。

Readiness 支持 `InitialDelaySec`、`IntervalSec`、`TimeoutSec`、`StartupTimeoutSec`、`SuccessThreshold`；成功前保持 start-pending。Health 还支持 `FailureThreshold`，持续失败会终止进程树并进入恢复。

`Type = "notify"` 与 `[Readiness]` 二选一。notify 模式注入 `SVP_NOTIFY_PIPE`，应用发送 UTF-8 `READY=1` 和定期 `WATCHDOG=1`。管道拒绝远程连接，ACL 仅允许 SYSTEM、Administrators 和实际服务账户。

### `[Logging]`

| 字段 | 说明 |
| --- | --- |
| `Timestamp` | 为输出行添加时间戳 |
| `[Logging.Stdout].Path` | stdout 绝对路径 |
| `[Logging.Stderr].Path` | stderr 绝对路径 |
| `[Logging.Rotation].MaximumBytes` | 达到指定字节数后轮转 |

配置 `[Logging]` 时至少提供一个输出路径。`logs --follow` 当前要求 stdout/stderr 使用同一路径。

## 运行与安全机制

- 主进程及后代进入同一 Job Object；停止和恢复覆盖完整进程树。
- 启动钩子失败会使启动失败；显式停止先执行 `ExecStop` 并等待，超时后分阶段强制终止；意外退出只执行 `ExecStopPost`。
- SCM 状态持续更新 checkpoint 和 wait hint。
- 只读共享内存发布 PID、运行时长、就绪、健康、重启数和资源统计。
- 失败状态会持久化故障类型、生命周期阶段、退出码、Win32 错误、详情和恢复次数；服务停止后仍可通过 `status`/`show` 查询，使用 `reset-failed` 清除。
- `apply` 和启动时审计配置、程序、目录、钩子、探针及日志路径 ACL，拒绝可被非受信主体修改的高权限路径。
- 推荐虚拟账户和 Service SID。TOML 与普通命令参数不接受密码；`credential set` 仅从隐藏控制台输入读取，普通账户密码经 DPAPI machine scope 加密并绑定服务名和账户名。凭据键创建时即使用仅 SYSTEM/Administrators 可访问的保护 ACL，移除服务会一并删除凭据。
- SvcPilot 是本机单用户/管理员工具，不提供多租户权限或远程控制面。

## JSON 与退出码

`--output=json` 服务于脚本、CI/CD 和未来 Web 端，且不会弹出 UAC。声明式配置命令使用：`0` 成功、`1` 校验失败、`2` 参数错误、`3` 权限不足、`4` 配置冲突、`5` Windows/SCM 错误、`6` 路径安全审计失败。状态判断命令还会用非零码表示 inactive 或 failed，自动化应同时参考 JSON。

## 当前限制

- 管理员 SCM 自动化和虚拟机 13 步验收已通过；异常注入、长期运行和压力测试尚未完成。
- 正式 `vMAJOR.MINOR.PATCH` tag 发布强制要求 Authenticode 证书、时间戳和签名校验；普通 CI 构建仍可能未签名，不应当作正式发布包分发。
- `reload` 会等待 `ExecReload` 完成，并按真实成功、失败或超时返回退出码；`reload-or-restart` 仅在服务不支持 reload 时回退到重启。
- `logs --follow` 要求 stdout/stderr 共用路径；MoveFile 轮转后可能继续读取旧句柄，需要重启命令。
- 不提供集中远程管理、Web UI、集群编排和多用户授权。

## 明确不支持

- Windows 10 以前版本、x86、ANSI/MBCS。
- Visual Studio/MSVC、VS2008 工程和 `/MT` 配置。
- NSSM GUI、交互式服务、旧命令和配置兼容层。
- 直接兼容 Linux systemd unit 文件；仅借鉴其命名和工作流。

## 构建与测试

### 虚拟机一键验收

在全新的 Windows 10/11 x64 虚拟机中，以管理员身份打开 PowerShell，在发布包目录执行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Test-SvcPilot.ps1
```

从源码目录测试指定产物：

```powershell
.\scripts\Test-SvcPilot.ps1 -Executable .\out\Release\win64\svp.exe
```

脚本退出码为 `0` 且最后显示 `PASS`，才表示全部通过。任何异常、断言失败或非预期退出码都会返回 `1`。测试使用随机 GUID 服务名，只创建自己的受管服务和一个用于验证删除保护的临时外部服务；`finally` 清理只针对这两个精确名称及其独立 `%ProgramData%\SvcPilotTest-*` 目录，不会停止、修改或删除既有服务。

验收覆盖配置校验和安全 ACL、plan/apply 幂等性、服务创建与删除、启动策略、启停重启、Readiness/Health、生命周期钩子、运行时状态、进程树、日志与轮转、reload、export、快速失败、持久失败状态及清理。

### 隔离虚拟机可靠性测试

压力、反复 SCM 操作和长期运行测试不会作为开发机常规测试执行。只在可随时还原快照的管理员 Windows 虚拟机中运行：

```powershell
.\Test-SvcPilot-Reliability.ps1 `
  -IUnderstandThisMustRunInDisposableVM `
  -Iterations 25 `
  -SoakHours 8
```

脚本反复执行完整的随机服务名验收流程，每轮均验证无 SCM 和临时目录残留，并生成 `SvcPilot-Reliability-Report.json`。缺少显式 VM 确认参数时脚本拒绝运行。

在 MSYS2 UCRT64 安装工具链：

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-binutils
```

构建并运行不修改 SCM 的测试：

```powershell
cmake --preset mingw-debug
cmake --build --preset mingw-debug
ctest --preset mingw-debug

cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-release
```

产物位于 `out\Debug\win64\svp.exe` 和 `out\Release\win64\svp.exe`。Release 支持可复现构建。GitHub Actions 构建和测试 Debug/Release，并生成 ZIP、SHA-256、SPDX 2.3 SBOM及可选代码签名。

管理员 SCM 集成脚本位于 `tests/integration/`，目前由维护者择机在隔离环境执行，不应把普通 CI 视为这部分验证已经完成。代码结构与审计结果见 [`docs/repository-audit.md`](docs/repository-audit.md)。

## 来源与许可

SvcPilot 基于 Iain Patterson 创建并贡献到 Public Domain 的 NSSM。第三方许可见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。独立名称不暗示原作者对当前实现提供支持或背书。
