#include "event.h"
#include "config_registration.h"
#include "credential.h"
#include "messages.h"
#include "output.h"
#include "service_control.h"
#include "service_constants.h"
#include "service_commands.h"
#include "service_enumeration.h"
#include "service_operations.h"
#include "status.h"
#include "status_query.h"
#include "string_utils.h"
#include "usage.h"
#include "windows_limits.h"

#include <cwchar>
#include <conio.h>
#include <string>
#include <vector>

namespace {

constexpr unsigned long kReloadResultTimeoutMs = 120000;

bool managed_service(const std::wstring &name) {
  std::wstring path;
  unsigned long error = ERROR_SUCCESS;
  return read_config_path(name, &path, &error);
}

void print_control_wait_failure(const wchar_t *service_name,
                                unsigned long control,
                                const SERVICE_STATUS &status,
                                int response,
                                unsigned long query_error) {
  if (response == -2) {
    fwprintf(stderr, L"%ls: QueryServiceStatus: %ls\n", service_name,
              error_string(query_error));
    return;
  }
  print_message(stderr, SVP_MESSAGE_BAD_CONTROL_RESPONSE, service_name,
                service_status_text(status.dwCurrentState),
                service_control_text(control));
}

bool wait_for_reload_result(const std::wstring &name,
                            unsigned long previous_request,
                            unsigned long *result) {
  const unsigned long long deadline =
      GetTickCount64() + kReloadResultTimeoutMs;
  unsigned long request = 0;
  while (GetTickCount64() < deadline) {
    service_status_query_t status;
    if (! query_service_runtime_status(name, &status)) {
      if (result) *result = status.windows_error;
      return false;
    }
    if (! request && status.reload_requested != previous_request) {
      request = status.reload_requested;
    }
    if (request && status.reload_completed >= request) {
      if (result) *result = status.reload_result;
      return status.reload_result == ERROR_SUCCESS;
    }
    if (status.state != SERVICE_RUNNING) {
      if (result) *result = ERROR_SERVICE_NOT_ACTIVE;
      return false;
    }
    Sleep(50);
  }
  if (result) *result = ERROR_SERVICE_REQUEST_TIMEOUT;
  return false;
}

}  // namespace

int list_services(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.help || arguments.quiet ||
      ! arguments.positional.empty() ||
      (arguments.json && arguments.verbose)) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: svp list [-a|--all] [-v|--verbose|-o json|--output=json]");
    return static_cast<int>(cli_exit_code::usage);
  }
  const bool including_native = arguments.all;
  std::vector<service_enumeration_entry_t> entries;
  unsigned long error = ERROR_SUCCESS;
  if (! enumerate_windows_services(including_native, &entries, &error)) {
    if (arguments.json) {
      print_cli_error(true, L"system",
                      L"Unable to enumerate Windows services", error);
    }
    else {
      print_message(stderr, SVP_MESSAGE_ENUMSERVICESSTATUS_FAILED,
                    error_string(error));
    }
    return static_cast<int>(cli_exit_code::system);
  }
  if (arguments.json) {
    fputws(L"{\"ok\":true,\"operation\":\"list\",\"services\":[", stdout);
    for (size_t index = 0; index < entries.size(); ++index) {
      if (index) fputwc(L',', stdout);
      const auto &entry = entries[index];
      fputws(L"{\"name\":", stdout);
      write_json_string(stdout, entry.name);
      fputws(L",\"displayName\":", stdout);
      write_json_string(stdout, entry.display_name);
      fputws(L",\"state\":", stdout);
      write_json_string(stdout, service_state_text(entry.state));
      fwprintf(stdout, L",\"pid\":%lu}", entry.process_id);
    }
    fputws(L"]}\n", stdout);
    return static_cast<int>(cli_exit_code::success);
  }
  if (arguments.verbose) {
    fwprintf(stdout, L"%-16ls %8ls %ls\n", L"STATE", L"PID", L"NAME");
    for (const auto &entry : entries) {
      fwprintf(stdout, L"%-16ls %8lu %ls\n",
               service_state_text(entry.state), entry.process_id,
               entry.name.c_str());
    }
    return static_cast<int>(cli_exit_code::success);
  }
  for (const auto &entry : entries) wprintf(L"%ls\n", entry.name.c_str());
  return static_cast<int>(cli_exit_code::success);
}

int service_process_tree(int argc, wchar_t **argv) {
  if (argc < 1) return usage(1);
  int errors = 0;
  for (int i = 0; i < argc; ++i) {
    std::vector<service_process_entry_t> entries;
    unsigned long error = ERROR_SUCCESS;
    if (! query_service_process_tree(argv[i], &entries, &error)) {
      fwprintf(stderr, L"%ls: %ls\n", argv[i], error_string(error));
      ++errors;
      continue;
    }
    for (const auto &entry : entries) {
      std::wstring indentation(entry.depth, L' ');
      wprintf(L"% 8lu %ls%ls\n", entry.pid, indentation.c_str(),
              entry.executable.c_str());
    }
  }

  return errors;
}

int pre_remove_service(int argc, wchar_t **argv) {
  if (argc != 2 || ! argv[0] || ! argv[0][0] ||
      ! strings_equal_ignore_case(argv[1], L"confirm")) {
    print_message(stderr, SVP_MESSAGE_PRE_REMOVE_SERVICE);
    return 100;
  }
  if (wcslen(argv[0]) >= SERVICE_NAME_LENGTH) return usage(1);
  unsigned long error = ERROR_SUCCESS;
  const service_removal_status_t status =
      remove_managed_service(argv[0], &error);
  if (status == service_removal_status_t::unmanaged) {
    fwprintf(stderr,
              L"%ls: refusing to remove a service not managed by SvcPilot: %ls\n",
              argv[0], error_string(error));
    return 4;
  }
  if (status == service_removal_status_t::system_error) {
    fwprintf(stderr, L"%ls: DeleteService: %ls\n", argv[0],
              error_string(error));
    return 5;
  }
  print_message(stdout, SVP_MESSAGE_SERVICE_REMOVED, argv[0]);
  return 0;
}

int control_service(unsigned long control, int argc, wchar_t **argv,
                    bool return_status) {
  if (argc != 1) return usage(1);
  service_control_result_t result;
  if (! execute_service_control(control, argv[0], &result)) {
    if (result.wait_response) {
      print_control_wait_failure(result.service_name.c_str(), control,
                                 result.status, result.wait_response,
                                 result.windows_error);
    }
    else {
      fwprintf(stderr, L"%ls: %ls: %ls", result.service_name.c_str(),
                service_control_text(control),
                error_string(result.windows_error));
    }
    return return_status ? 0 : 1;
  }
  if (control == SERVICE_CONTROL_INTERROGATE) {
    wprintf(L"%ls\n", service_status_text(result.status.dwCurrentState));
  }
  else {
    wprintf(L"%ls: %ls: %ls", result.service_name.c_str(),
             service_control_text(control), error_string(ERROR_SUCCESS));
  }
  return return_status ? result.status.dwCurrentState : 0;
}

int control_service(unsigned long control, int argc, wchar_t **argv) {
  return control_service(control, argc, argv, false);
}

int control_services_command(unsigned long control, bool restart, int argc,
                             wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.json || arguments.verbose || arguments.all ||
      arguments.help || arguments.positional.empty()) {
    print_cli_error(false, L"usage",
                    L"Usage: svp start|stop|restart <service> [<service> ...]");
    return static_cast<int>(cli_exit_code::usage);
  }
  int failures = 0;
  for (wchar_t *name : arguments.positional) {
    int result = 0;
    if (restart) {
      result = control_service(SERVICE_CONTROL_STOP, 1, &name);
      if (! result) result = control_service(kServiceControlStart, 1, &name);
    }
    else result = control_service(control, 1, &name);
    if (result) ++failures;
  }
  return failures ? static_cast<int>(cli_exit_code::system) : 0;
}

int reload_services_command(bool restart_if_unsupported, int argc,
                            wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.json || arguments.verbose || arguments.all ||
      arguments.quiet || arguments.help || arguments.positional.empty()) {
    print_cli_error(false, L"usage",
                    restart_if_unsupported
                        ? L"Usage: svp reload-or-restart <service> [<service> ...]"
                        : L"Usage: svp reload <service> [<service> ...]");
    return static_cast<int>(cli_exit_code::usage);
  }
  int failures = 0;
  for (wchar_t *name : arguments.positional) {
    service_status_query_t before;
    if (! query_service_runtime_status(name, &before)) {
      fwprintf(stderr, L"%ls: reload: %ls\n", name,
               error_string(before.windows_error));
      ++failures;
      continue;
    }
    service_control_result_t result;
    if (execute_service_control(kServiceControlReload, name, &result)) {
      unsigned long reload_result = ERROR_SUCCESS;
      if (wait_for_reload_result(name, before.reload_requested,
                                 &reload_result)) {
        wprintf(L"%ls: reload: %ls", name, error_string(ERROR_SUCCESS));
        continue;
      }
      if (restart_if_unsupported &&
          (reload_result == ERROR_CALL_NOT_IMPLEMENTED ||
           reload_result == ERROR_INVALID_SERVICE_CONTROL ||
           reload_result == ERROR_SERVICE_NOT_ACTIVE)) {
        int status = control_service(SERVICE_CONTROL_STOP, 1, &name);
        if (! status) status = control_service(kServiceControlStart, 1, &name);
        if (status) ++failures;
        continue;
      }
      fwprintf(stderr, L"%ls: reload failed: %ls\n", name,
               error_string(reload_result));
      ++failures;
      continue;
    }
    if (restart_if_unsupported &&
        (result.windows_error == ERROR_CALL_NOT_IMPLEMENTED ||
         result.windows_error == ERROR_INVALID_SERVICE_CONTROL ||
         result.windows_error == ERROR_SERVICE_NOT_ACTIVE)) {
      int status = control_service(SERVICE_CONTROL_STOP, 1, &name);
      if (! status) status = control_service(kServiceControlStart, 1, &name);
      if (status) ++failures;
      continue;
    }
    fwprintf(stderr, L"%ls: reload: %ls\n", name,
             error_string(result.windows_error));
    ++failures;
  }
  return failures ? static_cast<int>(cli_exit_code::system) : 0;
}

int is_active_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.json || arguments.verbose || arguments.all ||
      arguments.help || arguments.positional.empty()) {
    print_cli_error(false, L"usage",
                    L"Usage: svp is-active [--quiet] <service> [<service> ...]");
    return static_cast<int>(cli_exit_code::usage);
  }
  bool all_active = true;
  for (wchar_t *name : arguments.positional) {
    service_status_query_t status;
    const bool active = query_service_runtime_status(name, &status) &&
                        status.state == SERVICE_RUNNING;
    all_active = all_active && active;
    if (! arguments.quiet) wprintf(L"%ls\n", active ? L"active" : L"inactive");
  }
  return all_active ? 0 : 3;
}

int is_failed_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.json || arguments.verbose || arguments.all ||
      arguments.help || arguments.positional.empty()) {
    print_cli_error(false, L"usage",
                    L"Usage: svp is-failed [--quiet] <service> [<service> ...]");
    return static_cast<int>(cli_exit_code::usage);
  }
  bool any_failed = false;
  bool query_failed = false;
  for (wchar_t *name : arguments.positional) {
    service_status_query_t status;
    const bool queried = query_service_runtime_status(name, &status);
    query_failed = query_failed || ! queried;
    any_failed = any_failed || (queried && status.failed);
    if (! arguments.quiet) {
      wprintf(L"%ls\n", queried && status.failed ? L"failed" : L"inactive");
    }
  }
  if (query_failed) return static_cast<int>(cli_exit_code::system);
  return any_failed ? 0 : 1;
}

int reset_failed_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.json || arguments.verbose || arguments.all ||
      arguments.quiet || arguments.help || arguments.positional.empty()) {
    print_cli_error(false, L"usage",
                    L"Usage: svp reset-failed <service> [<service> ...]");
    return static_cast<int>(cli_exit_code::usage);
  }
  int failures = 0;
  for (wchar_t *name : arguments.positional) {
    if (! managed_service(name)) {
      fwprintf(stderr, L"%ls: not managed by SvcPilot\n", name);
      ++failures;
      continue;
    }
    unsigned long error = ERROR_SUCCESS;
    if (! clear_persistent_failure(name, &error)) {
      fwprintf(stderr, L"%ls: unable to reset failure state: %ls\n", name,
               error_string(error));
      ++failures;
    }
  }
  return failures ? static_cast<int>(cli_exit_code::system) : 0;
}

int is_enabled_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.json || arguments.verbose || arguments.all ||
      arguments.help || arguments.positional.empty()) {
    print_cli_error(false, L"usage",
                    L"Usage: svp is-enabled [--quiet] <service> [<service> ...]");
    return static_cast<int>(cli_exit_code::usage);
  }
  bool all_enabled = true;
  for (wchar_t *name : arguments.positional) {
    unsigned long start_type = SERVICE_DEMAND_START;
    unsigned long error = ERROR_SUCCESS;
    const bool queried = query_service_start_type(name, &start_type, &error);
    const bool enabled = queried && start_type == SERVICE_AUTO_START;
    all_enabled = all_enabled && enabled;
    if (! arguments.quiet) {
      const wchar_t *state = ! queried ? L"not-found" :
          start_type == SERVICE_AUTO_START ? L"enabled" :
          start_type == SERVICE_DISABLED ? L"masked" : L"disabled";
      wprintf(L"%ls\n", state);
    }
  }
  return all_enabled ? 0 : 1;
}

int set_service_startup_command(unsigned long start_type, int argc,
                                wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.json || arguments.verbose || arguments.all ||
      arguments.quiet || arguments.help || arguments.positional.empty()) {
    print_cli_error(false, L"usage",
                    L"Usage: svp enable|disable|mask|unmask <service> [<service> ...]");
    return static_cast<int>(cli_exit_code::usage);
  }
  int failures = 0;
  for (wchar_t *name : arguments.positional) {
    if (! managed_service(name)) {
      fwprintf(stderr, L"%ls: not managed by SvcPilot\n", name);
      ++failures;
      continue;
    }
    unsigned long error = ERROR_SUCCESS;
    if (! set_service_start_type(name, start_type, &error)) {
      fwprintf(stderr, L"%ls: unable to change startup mode: %ls\n", name,
               error_string(error));
      ++failures;
    }
  }
  return failures ? static_cast<int>(cli_exit_code::system) : 0;
}

int show_service_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.invalid || arguments.verbose || arguments.all || arguments.help ||
      arguments.quiet || arguments.positional.size() != 1) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: svp show [--output=json] <service>");
    return static_cast<int>(cli_exit_code::usage);
  }
  wchar_t *name = arguments.positional.front();
  service_status_query_t status;
  if (! query_service_runtime_status(name, &status)) {
    print_cli_error(arguments.json, L"system", L"Unable to query the service",
                    status.windows_error);
    return static_cast<int>(cli_exit_code::system);
  }
  const wchar_t *active = status.state == SERVICE_RUNNING
                              ? L"active"
                              : status.failed ? L"failed" : L"inactive";
  if (arguments.json) {
    fputws(L"{\"ok\":true,\"operation\":\"show\",\"service\":", stdout);
    write_json_string(stdout, name);
    fputws(L",\"activeState\":", stdout);
    write_json_string(stdout, active);
    fwprintf(stdout, L",\"wrapperPid\":%lu,\"mainPid\":%lu,\"uptimeMs\":%llu",
             status.wrapper_pid, status.child_pid, status.uptime_ms);
    fputws(L",\"health\":", stdout);
    write_json_string(stdout, runtime_health_text(status.health));
    fputws(L",\"failureKind\":", stdout);
    write_json_string(stdout, failure_kind_text(status.failure.kind));
    fputws(L",\"failurePhase\":", stdout);
    write_json_string(stdout, status.failure.phase);
    fputws(L",\"failureMessage\":", stdout);
    write_json_string(stdout, status.failure.message);
    fwprintf(stdout, L",\"cpuPercent\":%lu,\"memoryCurrent\":%llu,\"tasksCurrent\":%lu,\"nRestarts\":%lu,\"execMainStatus\":%lu,\"configDrift\":%ls,\"restartRequired\":%ls,\"failed\":%ls,\"failureWindowsError\":%lu,\"failureTime\":%llu,\"failureRestartAttempts\":%lu,\"runtimeAvailable\":%ls}\n",
             status.cpu_percent, status.memory_bytes, status.process_count,
             status.restart_count, status.failure.exit_code,
             status.config_drift ? L"true" : L"false",
             status.restart_required ? L"true" : L"false",
             status.failed ? L"true" : L"false",
             status.failure.windows_error,
             status.failure.timestamp_100ns,
             status.failure.restart_attempts,
             status.runtime_available ? L"true" : L"false");
  }
  else {
    fwprintf(stdout, L"Id=%ls\nActiveState=%ls\nSubState=%ls\nMainPID=%lu\n",
             name, active, service_state_text(status.state), status.child_pid);
    fwprintf(stdout, L"ExecMainPID=%lu\nUptimeMSec=%llu\nHealth=%ls\n",
             status.wrapper_pid, status.uptime_ms,
             runtime_health_text(status.health));
    fwprintf(stdout, L"CPUPercent=%lu\nMemoryCurrent=%llu\nTasksCurrent=%lu\nNRestarts=%lu\n",
             status.cpu_percent, status.memory_bytes, status.process_count,
             status.restart_count);
    fwprintf(stdout, L"ExecMainStatus=%lu\nFailed=%ls\n", status.failure.exit_code,
             status.failed ? L"yes" : L"no");
    fwprintf(stdout, L"ConfigDrift=%ls\nRestartRequired=%ls\n",
             status.config_drift ? L"yes" : L"no",
             status.restart_required ? L"yes" : L"no");
    if (status.failed) {
      fwprintf(stdout,
               L"FailureKind=%ls\nFailurePhase=%ls\nFailureWindowsError=%lu\nFailureTime=%llu\nFailureRestartAttempts=%lu\nFailureMessage=%ls\n",
               failure_kind_text(status.failure.kind),
               status.failure.phase.c_str(), status.failure.windows_error,
               status.failure.timestamp_100ns,
               status.failure.restart_attempts,
               status.failure.message.c_str());
    }
  }
  return 0;
}

int credential_command(int argc, wchar_t **argv) {
  if (argc < 2 || ! argv[0] || ! argv[1]) {
    fwprintf(stderr, L"Usage: svp credential set|delete <service> [account]\n");
    return static_cast<int>(cli_exit_code::usage);
  }
  const std::wstring action = argv[0];
  const std::wstring service = argv[1];
  unsigned long error = ERROR_SUCCESS;
  if (action == L"delete" && argc == 2) {
    if (! delete_service_credential(service, &error)) {
      fwprintf(stderr, L"%ls: unable to delete credential: %ls\n",
               service.c_str(), error_string(error));
      return static_cast<int>(cli_exit_code::system);
    }
    return 0;
  }
  if (action != L"set" || argc != 3 || ! argv[2][0]) {
    fwprintf(stderr, L"Usage: svp credential set <service> <account>\n");
    return static_cast<int>(cli_exit_code::usage);
  }
  fwprintf(stderr, L"Password for %ls: ", argv[2]);
  std::wstring password;
  while (true) {
    const wint_t character = _getwch();
    if (character == L'\r' || character == L'\n') break;
    if (character == L'\b') {
      if (! password.empty()) password.pop_back();
      continue;
    }
    if (character == 0 || character == 0xE0) { _getwch(); continue; }
    password.push_back(static_cast<wchar_t>(character));
  }
  fputwc(L'\n', stderr);
  const bool stored = ! password.empty() &&
      store_service_credential(service, argv[2], password, &error);
  if (! password.empty()) SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
  if (! stored) {
    fwprintf(stderr, L"%ls: unable to store credential: %ls\n",
             service.c_str(), error_string(error ? error : ERROR_INVALID_PASSWORD));
    return static_cast<int>(cli_exit_code::system);
  }
  wprintf(L"Stored credential for %ls as %ls\n", service.c_str(), argv[2]);
  return 0;
}
