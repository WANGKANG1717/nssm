# NSSM (Non-Sucking Service Manager)

NSSM 是一个将普通应用程序作为原生 Windows 服务运行的轻量监督器。本分支以原版 NSSM 为基础进行全新开发，提供声明式 JSON 配置、机器可读输出、进程监督、健康检查、恢复策略、资源治理和日志轮转。

项目仅提供 CLI 和 Windows 服务运行时，不包含 Win32 GUI。未来的 Web 或桌面管理端应调用稳定的 JSON CLI，不将界面逻辑并入服务监督核心。

## 平台与工具链

- Windows 10 或更高版本。
- 仅支持 x64 和 Unicode。
- C++20。
- 仅支持 MinGW-w64 GCC 14 或更高版本。
- CMake 3.25+、Ninja、`windmc` 和 `windres`。
- 不需要 Visual Studio，不支持 MSVC 工程或 32 位构建。
- 不支持交互式服务（`SERVICE_INTERACTIVE_PROCESS`）。

本项目基于 Iain Patterson 的 NSSM，遵循 Public Domain 许可。本分支不保留原版 GUI、旧工程和注册表式配置命令的兼容性。

## 构建

使用 MSYS2 UCRT64 可安装所需工具：

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

产物位于 `out\Debug\win64\nssm.exe` 和 `out\Release\win64\nssm.exe`。MinGW 运行库静态链接，Windows 系统库仍使用系统 DLL。Release 构建不嵌入时间戳，便于可复现构建。

本地便携 MSYS2 也可放在已忽略的 `.tools\msys64` 中：

```powershell
$toolBin = (Resolve-Path '.tools\msys64\ucrt64\bin').Path
$env:PATH = "$toolBin;$env:PATH"
```

GitHub Actions 会构建并测试 Debug/Release，验证 x64 PE 和静态 MinGW 运行库，并生成 ZIP、SHA-256 校验文件和 SPDX 2.3 SBOM。配置签名 secrets 后，Release 作业还会签名并验证 `nssm.exe`。

## 快速使用

先验证并预览配置，再以管理员权限应用：

```powershell
nssm validate --json .\service.json
nssm plan --json .\service.json
nssm apply --json .\service.json
```

管理服务：

```powershell
nssm start MyService
nssm status --json MyService
nssm stop MyService
nssm export --json MyService .\backup\MyService.json
nssm remove MyService confirm
```

常用命令：

- `validate <config>`：校验 JSON 和业务规则。
- `plan <config>`：只读比较期望配置与 SCM 状态。
- `apply <config>`：创建或原子更新服务配置。
- `export <service> <path>`：导出已登记的声明式配置。
- `start|stop|restart|pause|continue <service>`：控制服务。
- `status [--verbose|--json] <service>`：查询 SCM 与运行时状态。
- `rotate <service>`：触发在线日志轮转。
- `list [all]`：列出 NSSM 管理的服务，或列出全部 Windows 服务。
- `processes <service>`：列出包装进程树。
- `remove <service> confirm`：删除服务，必须显式确认。
- `version`：显示版本和架构。

旧 `install/edit/get/set/reset/unset/dump` 命令已删除。请修改 JSON 并重新执行 `apply`。

## 声明式配置

最小示例位于 `tests/fixtures/minimal-service.json`。当前 schema 严格固定为 `1`，不猜测或迁移旧 schema。

```json
{
  "schema": 1,
  "service": {
    "name": "example-service",
    "displayName": "Example Service",
    "startup": "automatic",
    "account": { "type": "virtual" }
  },
  "application": {
    "path": "C:\\apps\\example\\example.exe",
    "arguments": ["--serve"],
    "workingDirectory": "C:\\apps\\example"
  },
  "security": { "serviceSid": "unrestricted" }
}
```

声明式命令的统一退出码为：`0` 成功、`1` 配置校验失败、`2` 参数错误、`3` 权限不足、`4` 配置冲突、`5` Windows/SCM 错误、`6` 路径安全审计失败。`--json` 模式不弹出 UAC，方便 CI/CD 确定性处理。

`apply` 和服务启动时会审计配置文件、应用程序、工作目录、命令探针与日志目录的 Windows ACL。对象必须由 SYSTEM、Administrators 或 TrustedInstaller 控制，且不能向其他主体授予修改权限。

服务默认建议使用独立虚拟账户和 Service SID，而非 LocalSystem。配置不接受明文密码；需要域身份时优先使用 gMSA。

## 运行时状态

`nssm status --json <service>` 除了返回 SCM 状态和包装进程 PID，还会返回子进程 PID、运行时长、Readiness、Health Check、恢复重启次数，以及 Job Object 的 CPU、内存和进程数。运行时数据通过只读 ACL 的共享内存快照发布；服务停止或快照不可用时，`runtimeAvailable` 为 `false`。

## 仓库结构

```text
src/
  app/          程序入口、命令分派和版本信息
  cli/          参数、控制台与 JSON 输出
  config/       JSON 配置模型、校验、导出与 SCM 应用
  supervisor/
    process/     子进程、Job Object 和进程树终止
    monitoring/  Readiness、Health Check、资源监控与快照
    runtime/     服务状态机、恢复、Hooks 和日志
  platform/     Windows API、注册表、安全、事件日志与编码
resources/      版本信息、消息目录、图标和 manifest
tests/          单元测试、样例配置和 SCM 集成测试
cmake/          构建、资源编译和验证脚本
```

## 测试

CTest 覆盖版本/架构、Unicode 资源、声明式配置、命令行转义、环境隔离、Job Object、探针、恢复与安全校验。`tests/integration/service_lifecycle.ps1` 在管理员环境中执行完整 SCM 生命周期：`apply`、`list`、`start`、运行时 JSON 状态、原子 `export`、`stop` 和 `remove`。
