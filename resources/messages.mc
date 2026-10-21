LanguageNames =
(
English=0x0409:MSG00409
Chinese=0x0804:MSG00804
)

MessageId = 501
SymbolicName = SVP_MESSAGE_USAGE
Severity = Informational
Language = English
SvcPilot: Windows service supervisor
Version %ls %ls, %ls
Usage: svp <option> [<args> ...]

To validate or apply a declarative TOML configuration:

        svp validate <config.toml>

        svp plan [--output=json] <config.toml>

        svp apply [--output=json] <config.toml> [...]

        svp export [--output=json] <servicename> <destination.toml>

To remove a service with explicit confirmation:

        svp remove <servicename> confirm

To manage a service:

        svp start <servicename>

        svp stop <servicename>

        svp restart <servicename>

        svp reload|reload-or-restart <servicename> [...]

        svp continue <servicename>

        svp status [-v|--verbose] [-o json|--output=json] <servicename>

        svp show [--output=json] <servicename>

        svp is-active [-q|--quiet] <servicename> [...]

        svp is-failed [-q|--quiet] <servicename> [...]

        svp reset-failed <servicename> [...]

        svp is-enabled [-q|--quiet] <servicename> [...]

        svp enable|disable|mask|unmask <servicename> [...]

        svp rotate <servicename>

        svp processes <servicename>

        svp logs [-n N|--lines N] [-f|--follow] <servicename>

        svp events [-n N|--lines N] [-o json|--output=json] <servicename>

        svp list|list-units [-a|--all] [-v|--verbose|-o json|--output=json]
.
Language = Chinese
SvcPilot：Windows 服务监督器
版本 %ls %ls, %ls
用法：svp <选项> [<参数> ...]

常用命令：
  验证配置：svp validate <配置文件.toml>
  预览变更：svp plan [--output=json] <配置文件.toml>
  安装或更新：svp apply [--output=json] <配置文件.toml> [...]
  原子导出：svp export [--output=json] <服务名> <目标文件.toml>

  移除（显式确认）: svp remove <服务名> confirm

  管理：
    svp start|stop|restart|continue <服务名>
    svp reload|reload-or-restart <服务名> [...]
    svp status [-v|--verbose] [-o json|--output=json] <服务名>
    svp show [--output=json] <服务名>
    svp is-active [-q|--quiet] <服务名> [...]
    svp is-failed [-q|--quiet] <服务名> [...]
    svp reset-failed <服务名> [...]
    svp is-enabled [-q|--quiet] <服务名> [...]
    svp enable|disable|mask|unmask <服务名> [...]
    svp rotate|processes <服务名>
    svp logs [-n N|--lines N] [-f|--follow] <服务名>
    svp events [-n N|--lines N] [-o json|--output=json] <服务名>
    svp list|list-units [-a|--all] [-v|--verbose|-o json|--output=json]
.

MessageId = +1
SymbolicName = SVP_MESSAGE_NOT_ADMINISTRATOR_CANNOT_INSTALL
Severity = Informational
Language = English
Administrator access is needed to install a service.
.
Language = Chinese
安装服务需要管理员权限。
.

MessageId = +1
SymbolicName = SVP_MESSAGE_NOT_ADMINISTRATOR_CANNOT_REMOVE
Severity = Informational
Language = English
Administrator access is needed to remove a service.
.
Language = Chinese
移除服务需要管理员权限。
.

MessageId = +1
SymbolicName = SVP_MESSAGE_PRE_REMOVE_SERVICE
Severity = Informational
Language = English
To remove a service, confirm explicitly: svp remove <servicename> confirm
.
Language = Chinese
移除服务需要显式确认：svp remove <服务名> confirm
.

MessageId = +1
SymbolicName = SVP_MESSAGE_ENUMSERVICESSTATUS_FAILED
Severity = Informational
Language = English
Can't open service!
EnumServicesStatus(): %ls
.
Language = Chinese
无法打开服务！
EnumServicesStatus()：%ls
.

MessageId = +1
SymbolicName = SVP_MESSAGE_SERVICE_REMOVED
Severity = Informational
Language = English
Service "%ls" removed successfully!
.
Language = Chinese
服务"%ls"移除成功！
.

MessageId = +1
SymbolicName = SVP_MESSAGE_BAD_CONTROL_RESPONSE
Severity = Informational
Language = English
%ls: Unexpected status %ls in response to %ls control.
.
Language = Chinese
%ls：收到对 %ls 控制的异常状态 %ls。
.

MessageId = 1001
SymbolicName = SVP_EVENT_DISPATCHER_FAILED
Severity = Error
Language = English
StartServiceCtrlDispatcher() failed:
%1
.
Language = Chinese
StartServiceCtrlDispatcher() 失败：
%1
.

MessageId = +1
SymbolicName = SVP_EVENT_OUT_OF_MEMORY
Severity = Error
Language = English
Out of memory for %1 in %2!
.
Language = Chinese
在 %2 中为 %1 分配内存失败！
.

MessageId = +1
SymbolicName = SVP_EVENT_REGISTERSERVICECTRLHANDER_FAILED
Severity = Error
Language = English
RegisterServiceCtrlHandlerEx() failed:
%1
.
Language = Chinese
RegisterServiceCtrlHandlerEx() 失败：
%1
.

MessageId = +1
SymbolicName = SVP_EVENT_START_SERVICE_FAILED
Severity = Error
Language = English
Can't start %1 for service %2.
Error code: %3.
.
Language = Chinese
无法为服务 %2 启动 %1。
错误码：%3。
.

MessageId = +1
SymbolicName = SVP_EVENT_RESTART_SERVICE_FAILED
Severity = Warning
Language = English
Failed to restart %1 for service %2.
Sleeping...
.
Language = Chinese
为服务 %2 重启 %1 失败。
等待中……
.

MessageId = +1
SymbolicName = SVP_EVENT_STARTED_SERVICE
Severity = Informational
Language = English
Started %1 %2 for service %3 in %4.
.
Language = Chinese
已为服务 %3 启动 %1 %2，用时 %4。
.

MessageId = +1
SymbolicName = SVP_EVENT_REGISTERWAITFORSINGLEOBJECT_FAILED
Severity = Warning
Language = English
Service %1 may claim to be still running when %2 exits.
RegisterWaitForSingleObject() failed:
%3
.
Language = Chinese
当 %2 退出时，服务 %1 可能仍声称在运行。
RegisterWaitForSingleObject() 失败：
%3
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATEPROCESS_FAILED
Severity = Error
Language = English
Failed to start service %1.  Program %2 couldn't be launched.
CreateProcess() failed:
%3
.
Language = Chinese
启动服务 %1 失败。无法启动程序 %2。
CreateProcess() 失败：
%3
.

MessageId = +1
SymbolicName = SVP_EVENT_TERMINATEPROCESS
Severity = Informational
Language = English
Killing process %2 because service %1 is stopping.
.
Language = Chinese
由于服务 %1 正在停止，正在终止进程 %2。
.

MessageId = +1
SymbolicName = SVP_EVENT_PROCESS_ALREADY_STOPPED
Severity = Informational
Language = English
Requested stop of service %1.  No action is required as program %2 is not running.
.
Language = Chinese
已请求停止服务 %1。由于程序 %2 未在运行，无需执行操作。
.

MessageId = +1
SymbolicName = SVP_EVENT_ENDED_SERVICE
Severity = Informational
Language = English
Program %1 for service %2 exited with return code %3.
.
Language = Chinese
服务 %2 的程序 %1 已退出，返回码 %3。
.

MessageId = +1
SymbolicName = SVP_EVENT_EXIT_RESTART
Severity = Informational
Language = English
Service %1 action for exit code %2 is %3.
Attempting to restart %4.
.
Language = Chinese
服务 %1 的退出码 %2 对应动作为 %3。
正在尝试重启 %4。
.

MessageId = +1
SymbolicName = SVP_EVENT_OPENKEY_FAILED
Severity = Error
Language = English
Failed to open registry key HKLM\%1:
%2
.
Language = Chinese
打开注册表键 HKLM\%1 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_SETVALUE_FAILED
Severity = Error
Language = English
Failed to write registry value %1:
%2
.
Language = Chinese
写入注册表值 %1 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_KILLING
Severity = Informational
Language = English
Killing process tree of process %2 for service %1 with exit code %3
.
Language = Chinese
正在终止服务 %1 的进程树（根 PID %2），退出码 %3
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATETOOLHELP32SNAPSHOT_PROCESS_FAILED
Severity = Error
Language = English
Failed to create snapshot of running processes when terminating service %1:
%2
.
Language = Chinese
终止服务 %1 时创建进程快照失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_PROCESS_ENUMERATE_FAILED
Severity = Error
Language = English
Failed to enumerate running processes when terminating service %1:
%2
.
Language = Chinese
终止服务 %1 时枚举进程失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_OPENPROCESS_FAILED
Severity = Error
Language = English
Failed to open process handle for process with PID %1 when terminating service %2:
%3
.
Language = Chinese
终止服务 %2 时，打开 PID %1 的进程句柄失败：
%3
.

MessageId = +1
SymbolicName = SVP_EVENT_KILL_PROCESS_TREE
Severity = Informational
Language = English
Killing PID %1 in process tree of PID %2 because service %3 is stopping.
.
Language = Chinese
由于服务 %3 正在停止，正在终止进程树 PID %2 下的 PID %1。
.

MessageId = +1
SymbolicName = SVP_EVENT_TERMINATEPROCESS_FAILED
Severity = Error
Language = English
Failed to terminate process with PID %1 for service %2:
%3
.
Language = Chinese
终止服务 %2 的 PID %1 失败：
%3
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATETOOLHELP32SNAPSHOT_THREAD_FAILED
Severity = Error
Language = English
Failed to create snapshot of running threads when terminating service %1:
%2
.
Language = Chinese
终止服务 %1 时创建线程快照失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_THREAD_ENUMERATE_FAILED
Severity = Error
Language = English
Failed to enumerate running threads when terminating service %1:
%2
.
Language = Chinese
终止服务 %1 时枚举线程失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_RESET_THROTTLE
Severity = Informational
Language = English
Request to resume service %1.  Throttling of restart attempts will be reset.
.
Language = Chinese
收到恢复服务 %1 的请求。将重置重启节流。
.

MessageId = +1
SymbolicName = SVP_EVENT_SERVICE_CONTROL_HANDLED
Severity = Informational
Language = English
Service %1 received %2 control, which will be handled.
.
Language = Chinese
服务 %1 收到控制码 %2，将予以处理。
.

MessageId = +1
SymbolicName = SVP_EVENT_SERVICE_CONTROL_NOT_HANDLED
Severity = Informational
Language = English
Service %1 received unsupported %2 control, which will not be handled.
.
Language = Chinese
服务 %1 收到不支持的控制码 %2，将不会处理。
.

MessageId = +1
SymbolicName = SVP_EVENT_SERVICE_CONTROL_UNKNOWN
Severity = Informational
Language = English
Service %1 received unknown service control message %2, which will be ignored.
.
Language = Chinese
服务 %1 收到未知服务控制消息 %2，将被忽略。
.

MessageId = +1
SymbolicName = SVP_EVENT_SERVICE_CONFIG_FAILURE_ACTIONS_FAILED
Severity = Informational
Language = English
Error configuring service failure actions for service %1.  The service will not be subject to recovery actions if it exits gracefully with a non-zero exit code.
ChangeServiceConfig2() failed:
%2
.
Language = Chinese
为服务 %1 配置失败动作出错。
如果服务以非零退出码"正常"退出，将不再触发恢复策略。
ChangeServiceConfig2() 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_GETPROCESSTIMES_FAILED
Severity = Error
Language = English
GetProcessTimes() failed:
%1
.
Language = Chinese
GetProcessTimes() 失败：
%1
.

MessageId = +1
SymbolicName = SVP_EVENT_GETEXITCODEPROCESS_FAILED
Severity = Error
Language = English
Failed to read the exit code for service %1.
GetExitCodeProcess() failed:
%2
.
Language = Chinese
无法读取服务 %1 的退出码。
GetExitCodeProcess() 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_UNREGISTERWAIT_FAILED
Severity = Error
Language = English
Failed to unregister the process wait for service %1:
%2
.
Language = Chinese
无法注销服务 %1 的进程等待：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_ATTACHCONSOLE_FAILED
Severity = Error
Language = English
Error attaching to console for service %1.
AttachConsole() failed:
%2
.
Language = Chinese
为服务 %1 附加控制台失败。
AttachConsole() 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_SETCONSOLECTRLHANDLER_FAILED
Severity = Error
Language = English
Error setting null handler for Control-C events sent to service %1.
SetConsoleCtrlHandler() failed:
%2
.
Language = Chinese
为服务 %1 设置 Control-C 事件空处理程序失败。
SetConsoleCtrlHandler() 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_GENERATECONSOLECTRLEVENT_FAILED
Severity = Error
Language = English
Error generating Control-C event for service %1.
GenerateConsoleCtrlEvent() failed:
%2
.
Language = Chinese
为服务 %1 生成 Control-C 事件失败。
GenerateConsoleCtrlEvent() 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_FREECONSOLE_FAILED
Severity = Warning
Language = English
Error detaching from console for service %1.
FreeConsole() failed:
%2
.
Language = Chinese
为服务 %1 释放控制台失败。
FreeConsole() 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATEFILE_FAILED
Severity = Error
Language = English
CreateFile() failed to open %1:
%2
.
Language = Chinese
CreateFile() 打开 %1 失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_DUPLICATEHANDLE_FAILED
Severity = Error
Language = English
Error duplicating the filehandle previously opened for %1 as %2.
DuplicateHandle() failed:
%3
.
Language = Chinese
将已为 %1 打开的文件句柄复制为 %2 失败。
DuplicateHandle() 失败：
%3
.

MessageId = +1
SymbolicName = SVP_EVENT_GET_OUTPUT_HANDLES_FAILED
Severity = Error
Language = English
Error setting up one or more I/O filehandles.  Service %1 will not be started.
.
Language = Chinese
配置一个或多个 I/O 文件句柄失败。服务 %1 将不会启动。
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATETHREAD_FAILED
Severity = Error
Language = English
CreateThread() failed:
%1
.
Language = Chinese
CreateThread() 失败：
%1
.

MessageId = +1
SymbolicName = SVP_EVENT_ROTATE_FILE_FAILED
Severity = Error
Language = English
Failed to rotate output file %2 for service %1.
%3 failed for file %4:
%5
.
Language = Chinese
为服务 %1 轮转输出文件 %2 失败。
%3 失败（文件 %4）：
%5
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATE_JOB_FAILED
Severity = Error
Language = English
Failed to create or configure a Job Object for service %1:
%2
.
Language = Chinese
为服务 %1 创建或配置作业对象失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_ASSIGN_JOB_FAILED
Severity = Error
Language = English
Failed to assign the application process to the Job Object for service %1:
%2
.
Language = Chinese
无法将应用程序进程加入服务 %1 的作业对象：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_TERMINATE_JOB_FAILED
Severity = Warning
Language = English
Failed to terminate the Job Object for service %1:
%2
.
Language = Chinese
终止服务 %1 的作业对象失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_RESUME_PROCESS_FAILED
Severity = Error
Language = English
Failed to resume the application process for service %1:
%2
.
Language = Chinese
恢复服务 %1 的应用程序进程失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_CONFIG_LOAD_FAILED
Severity = Error
Language = English
Failed to load the declarative configuration for service %1:
%2
.
Language = Chinese
加载服务 %1 的声明式配置失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_RECOVERY_STOPPED
Severity = Warning
Language = English
Service %1 will not be restarted: %2.
.
Language = Chinese
服务 %1 将不再重启：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_HEALTH_MONITOR_START_FAILED
Severity = Error
Language = English
Failed to start the health monitor for service %1: %2.
.
Language = Chinese
无法启动服务 %1 的健康监控：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_HEALTH_FAILED
Severity = Warning
Language = English
Health check for service %1 failed: %2 (consecutive failures: %3).
.
Language = Chinese
服务 %1 的健康检查失败：%2（连续失败次数：%3）。
.

MessageId = +1
SymbolicName = SVP_EVENT_HEALTH_RECOVERED
Severity = Informational
Language = English
Health check for service %1 recovered: %2.
.
Language = Chinese
服务 %1 的健康检查已恢复：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_HEALTH_WATCHDOG
Severity = Error
Language = English
Health watchdog is restarting service %1 after reaching the failure threshold: %2.
.
Language = Chinese
服务 %1 达到健康检查失败阈值，Watchdog 将重启该服务：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_RESOURCE_MONITOR_START_FAILED
Severity = Error
Language = English
Failed to start the resource monitor for service %1: %2.
.
Language = Chinese
无法启动服务 %1 的资源监控：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_READINESS_READY
Severity = Informational
Language = English
Service %1 is ready: %2.
.
Language = Chinese
服务 %1 已就绪：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_READINESS_FAILED
Severity = Error
Language = English
Service %1 failed its readiness check: %2.
.
Language = Chinese
服务 %1 未通过就绪检查：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_STATUS_SNAPSHOT_FAILED
Severity = Warning
Language = English
Failed to create the runtime status snapshot for service %1: %2.
.
Language = Chinese
无法为服务 %1 创建运行时状态快照：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_LIFECYCLE_COMMAND_FAILED
Severity = Error
Language = English
Service %1 lifecycle phase %2 failed: %3.
.
Language = Chinese
服务 %1 的生命周期阶段 %2 失败：%3。
.

MessageId = +1
SymbolicName = SVP_EVENT_PERSIST_FAILURE_FAILED
Severity = Warning
Language = English
Failed to persist failure state for service %1: %2.
.
Language = Chinese
无法持久记录服务 %1 的失败状态：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_CONFIGURE_JOB_FAILED
Severity = Error
Language = English
Failed to configure Job Object resource limits for service %1: %2.
.
Language = Chinese
无法为服务 %1 配置 Job Object 资源限制：%2。
.

MessageId = +1
SymbolicName = SVP_EVENT_RESOURCE_EXCEEDED
Severity = Error
Language = English
Service %1 exceeded its %2: %3.
.
Language = Chinese
服务 %1 超出%2：%3。
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATEPIPE_FAILED
Severity = Error
Language = English
Failed to set up a pipe to read output from service %1.
Rotation of log file %2 will not be possible while the service is running.
CreatePipe(): %3
.
Language = Chinese
为读取服务 %1 的输出创建管道失败。
在服务运行期间将无法轮转日志文件 %2。
CreatePipe()：%3
.

MessageId = +1
SymbolicName = SVP_EVENT_READFILE_FAILED
Severity = Error
Language = English
Failed to read output for service %1.
If the error persists, no more data will be written to %2.
ReadFile(): %3
.
Language = Chinese
读取服务 %1 的输出失败。
如果错误持续发生，将不再向 %2 写入更多数据。
ReadFile()：%3
.

MessageId = +1
SymbolicName = SVP_EVENT_WRITEFILE_FAILED
Severity = Error
Language = English
Failed to write output for service %1 to file %2.
If the error persists, some data may be lost.
WriteFile(): %3
.
Language = Chinese
将服务 %1 的输出写入文件 %2 失败。
如果错误持续发生，可能会丢失部分数据。
WriteFile()：%3
.

MessageId = +1
SymbolicName = SVP_EVENT_SOMEBODY_SET_UP_US_THE_BOM
Severity = Warning
Language = English
Output from service %1 was detected as being in UTF-16 format but an attempt to write an appropriate byte order marker failed.
It is likely that subsequent attempts to write data to %2 will fail.  If they succeed, the file may not be recognised as being
in UTF-16 format by applications which attempt to read it.
WriteFile(): %3
.
Language = Chinese
检测到服务 %1 的输出为 UTF-16，但写入相应的字节序标记（BOM）失败。
后续向 %2 写入数据的尝试很可能失败；即使成功，该文件也可能不会被读取它的应用识别为 UTF-16 格式。
WriteFile()：%3
.

MessageId = +1
SymbolicName = SVP_EVENT_ROTATED
Severity = Informational
Language = English
Rotated output file %2 for service %1 to %3.
.
Language = Chinese
已将服务 %1 的输出文件 %2 轮转为 %3。
.

MessageId = +1
SymbolicName = SVP_EVENT_AWAITING_SINGLE_HANDLE
Severity = Informational
Language = English
%1 has waited %3 of %5 milliseconds for the %2 handle.
Next update in %4 milliseconds.
.
Language = Chinese
%1 已等待 %3/%5 毫秒以等待句柄 %2。
下一次更新在 %4 毫秒后。
.

MessageId = +1
SymbolicName = SVP_EVENT_CREATEEVENT_FAILED
Severity = Error
Language = English
Failed to create the stop event for service %1:
%2
.
Language = Chinese
为服务 %1 创建停止事件失败：
%2
.

MessageId = +1
SymbolicName = SVP_EVENT_SETSERVICESTATUS_FAILED
Severity = Error
Language = English
Failed to report service %1 status while %2:
%3
.
Language = Chinese
在%2时上报服务 %1 状态失败：
%3
.
