#include "status.h"

#include "output.h"
#include "status_query.h"

#include <string>

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

int inspect_service_status_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.all || arguments.help ||
      arguments.positional.size() != 1 || ! arguments.positional[0]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: svp status [--verbose] [--output=json] <service>");
    return static_cast<int>(cli_exit_code::usage);
  }
  std::wstring name = arguments.positional[0];
  service_status_query_t status;
  if (! query_service_runtime_status(name, &status)) {
    const unsigned long error = status.windows_error;
    const bool not_found = error == ERROR_SERVICE_DOES_NOT_EXIST;
    const bool permission = error == ERROR_ACCESS_DENIED;
    print_cli_error(arguments.json,
                    not_found ? L"notFound"
                              : permission ? L"permission" : L"system",
                    L"Unable to query the service", error);
    return static_cast<int>(not_found
                                ? cli_exit_code::conflict
                                : permission ? cli_exit_code::permission
                                             : cli_exit_code::system);
  }
  const wchar_t *state = service_state_text(status.state);
  const wchar_t *health = runtime_health_text(status.health);

  if (! arguments.json && ! arguments.quiet) {
    fwprintf(stdout, L"Service: %ls\nState: %ls\nWrapper PID: %lu\n",
             name.c_str(), state, status.wrapper_pid);
    fwprintf(stdout, L"Child PID: %lu\nUptime: %llu ms\nHealth: %ls\n",
             status.child_pid, status.uptime_ms, health);
    fwprintf(stdout,
             L"Restarts: %lu\nCPU: %lu%%\nMemory: %llu bytes\nProcesses: %lu\n",
             status.restart_count, status.cpu_percent, status.memory_bytes,
             status.process_count);
    if (! status.runtime_available && status.state != SERVICE_STOPPED) {
      fwprintf(stdout, L"Runtime snapshot: unavailable (Windows error %lu)\n",
               status.windows_error);
    }
    if (status.failed) {
      fwprintf(stdout,
               L"Failure: %ls (phase=%ls, exit=%lu, win32=%lu, restarts=%lu)\n",
               failure_kind_text(status.failure.kind),
               status.failure.phase.empty() ? L"-" : status.failure.phase.c_str(),
               status.failure.exit_code, status.failure.windows_error,
               status.failure.restart_attempts);
      if (! status.failure.message.empty()) {
        fwprintf(stdout, L"Failure detail: %ls\n", status.failure.message.c_str());
      }
    }
  }
  else {
    fputws(L"{\"ok\":true,\"operation\":\"status\",\"service\":", stdout);
    write_json_string(stdout, name);
    fputws(L",\"state\":", stdout);
    write_json_string(stdout, state);
    fwprintf(stdout,
             L",\"wrapperPid\":%lu,\"childPid\":%lu,\"uptimeMs\":%llu",
             status.wrapper_pid, status.child_pid, status.uptime_ms);
    fputws(L",\"health\":", stdout);
    write_json_string(stdout, health);
    fputws(L",\"failureKind\":", stdout);
    write_json_string(stdout, failure_kind_text(status.failure.kind));
    fputws(L",\"failurePhase\":", stdout);
    write_json_string(stdout, status.failure.phase);
    fputws(L",\"failureMessage\":", stdout);
    write_json_string(stdout, status.failure.message);
    fwprintf(stdout,
             L",\"restartCount\":%lu,\"cpuPercent\":%lu,\"memoryBytes\":%llu,\"processCount\":%lu,\"failed\":%ls,\"failureExitCode\":%lu,\"failureWindowsError\":%lu,\"failureTime\":%llu,\"failureRestartAttempts\":%lu,\"runtimeAvailable\":%ls}\n",
             status.restart_count, status.cpu_percent, status.memory_bytes,
             status.process_count,
             status.failed ? L"true" : L"false",
             status.failure.exit_code, status.failure.windows_error,
             status.failure.timestamp_100ns,
             status.failure.restart_attempts,
             status.runtime_available ? L"true" : L"false");
  }
  return static_cast<int>(cli_exit_code::success);
}
