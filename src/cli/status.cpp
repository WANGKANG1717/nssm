#include "status.h"

#include "output.h"
#include "status_snapshot.h"
#include "win32_handle.h"

#include <windows.h>

#include <string>

namespace {

const wchar_t *service_state_text(unsigned long state) {
  switch (state) {
    case SERVICE_STOPPED: return L"stopped";
    case SERVICE_START_PENDING: return L"startPending";
    case SERVICE_STOP_PENDING: return L"stopPending";
    case SERVICE_RUNNING: return L"running";
    case SERVICE_CONTINUE_PENDING: return L"continuePending";
    case SERVICE_PAUSE_PENDING: return L"pausePending";
    case SERVICE_PAUSED: return L"paused";
    default: return L"unknown";
  }
}

}  // namespace

int inspect_service_status_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.positional.size() != 1 || ! arguments.positional[0]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: nssm status [--verbose] [--json] <service>");
    return static_cast<int>(cli_exit_code::usage);
  }
  std::wstring name = arguments.positional[0];
  unique_service_handle manager(OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE,
                                                SC_MANAGER_CONNECT));
  if (! manager) {
    unsigned long error = GetLastError();
    print_cli_error(arguments.json,
                    error == ERROR_ACCESS_DENIED ? L"permission" : L"system",
                    L"Unable to open the Service Control Manager", error);
    return static_cast<int>(error == ERROR_ACCESS_DENIED
                                ? cli_exit_code::permission
                                : cli_exit_code::system);
  }
  unique_service_handle service(
      OpenServiceW(manager.get(), name.c_str(), SERVICE_QUERY_STATUS));
  if (! service) {
    unsigned long error = GetLastError();
    print_cli_error(arguments.json,
                    error == ERROR_SERVICE_DOES_NOT_EXIST ? L"notFound"
                                                          : L"system",
                    L"Unable to open the service", error);
    return static_cast<int>(error == ERROR_SERVICE_DOES_NOT_EXIST
                                ? cli_exit_code::conflict
                                : cli_exit_code::system);
  }
  SERVICE_STATUS_PROCESS status = {};
  unsigned long needed = 0;
  if (! QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<unsigned char *>(&status),
                             sizeof(status), &needed)) {
    unsigned long error = GetLastError();
    print_cli_error(arguments.json, L"system",
                    L"Unable to query service status", error);
    return static_cast<int>(cli_exit_code::system);
  }
  status_snapshot_t snapshot = {};
  unsigned long snapshot_error = ERROR_SUCCESS;
  bool has_snapshot = read_status_snapshot(name, &snapshot, &snapshot_error);
  if (has_snapshot &&
      (! status.dwProcessId || snapshot.wrapper_pid != status.dwProcessId)) {
    has_snapshot = false;
    snapshot_error = ERROR_INVALID_DATA;
  }
  unsigned long long uptime_ms =
      has_snapshot && snapshot.child_pid && snapshot.process_started_ms
          ? GetTickCount64() - snapshot.process_started_ms
          : 0;
  const wchar_t *state = service_state_text(status.dwCurrentState);
  const wchar_t *health =
      has_snapshot
          ? runtime_health_text(static_cast<runtime_health_t>(snapshot.health))
          : L"unknown";

  if (! arguments.json) {
    fwprintf(stdout, L"Service: %ls\nState: %ls\nWrapper PID: %lu\n",
             name.c_str(), state, status.dwProcessId);
    fwprintf(stdout, L"Child PID: %lu\nUptime: %llu ms\nHealth: %ls\n",
             has_snapshot ? snapshot.child_pid : 0, uptime_ms, health);
    fwprintf(stdout,
             L"Restarts: %lu\nCPU: %lu%%\nMemory: %llu bytes\nProcesses: %lu\n",
             has_snapshot ? snapshot.restart_count : 0,
             has_snapshot ? snapshot.cpu_percent : 0,
             has_snapshot ? snapshot.memory_bytes : 0,
             has_snapshot ? snapshot.process_count : 0);
    if (! has_snapshot && status.dwCurrentState != SERVICE_STOPPED) {
      fwprintf(stdout, L"Runtime snapshot: unavailable (Windows error %lu)\n",
               snapshot_error);
    }
  }
  else {
    fputws(L"{\"ok\":true,\"operation\":\"status\",\"service\":", stdout);
    write_json_string(stdout, name);
    fputws(L",\"state\":", stdout);
    write_json_string(stdout, state);
    fwprintf(stdout,
             L",\"wrapperPid\":%lu,\"childPid\":%lu,\"uptimeMs\":%llu",
             status.dwProcessId, has_snapshot ? snapshot.child_pid : 0,
             uptime_ms);
    fputws(L",\"health\":", stdout);
    write_json_string(stdout, health);
    fwprintf(stdout,
             L",\"restartCount\":%lu,\"cpuPercent\":%lu,\"memoryBytes\":%llu,\"processCount\":%lu,\"runtimeAvailable\":%ls}\n",
             has_snapshot ? snapshot.restart_count : 0,
             has_snapshot ? snapshot.cpu_percent : 0,
             has_snapshot ? snapshot.memory_bytes : 0,
             has_snapshot ? snapshot.process_count : 0,
             has_snapshot ? L"true" : L"false");
  }
  return static_cast<int>(cli_exit_code::success);
}
