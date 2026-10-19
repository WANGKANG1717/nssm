#include "service_operations.h"

#include "config_registration.h"
#include "service_constants.h"
#include "process.h"
#include "security.h"
#include "win32_handle.h"
#include "windows_limits.h"

#include <new>
#include <utility>

namespace {

int service_control_response(unsigned long control, unsigned long status) {
  switch (control) {
    case kServiceControlStart:
      if (status == SERVICE_START_PENDING) return 1;
      return status == SERVICE_RUNNING ? 0 : -1;
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      if (status == SERVICE_RUNNING || status == SERVICE_STOP_PENDING) return 1;
      return status == SERVICE_STOPPED ? 0 : -1;
    case SERVICE_CONTROL_PAUSE:
      if (status == SERVICE_PAUSE_PENDING) return 1;
      return status == SERVICE_PAUSED ? 0 : -1;
    case SERVICE_CONTROL_CONTINUE:
      if (status == SERVICE_CONTINUE_PENDING) return 1;
      return status == SERVICE_RUNNING ? 0 : -1;
    case SERVICE_CONTROL_INTERROGATE:
    case kServiceControlRotate:
    case kServiceControlReload:
    default:
      return 0;
  }
}

int await_service_control_response(unsigned long control, SC_HANDLE service,
                                   SERVICE_STATUS *status,
                                   unsigned long initial_status,
                                   unsigned long *error) {
  unsigned long long last_progress = GetTickCount64();
  unsigned long checkpoint = 0;
  bool initial_state_observed = false;
  bool transition_observed = false;
  while (true) {
    if (! QueryServiceStatus(service, status)) {
      if (error) *error = GetLastError();
      return -2;
    }
    const int response =
        service_control_response(control, status->dwCurrentState);
    if (! response) return 0;
    if (response < 0) {
      const bool reported_failure = status->dwWin32ExitCode != NO_ERROR;
      if (reported_failure || status->dwCurrentState != initial_status ||
          transition_observed || initial_state_observed) {
        if (error) {
          *error = reported_failure
                       ? status->dwWin32ExitCode
                       : control == kServiceControlStart
                             ? ERROR_SERVICE_NOT_ACTIVE
                             : ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
        }
        return response;
      }
      initial_state_observed = true;
    }
    else if (status->dwCurrentState != initial_status) {
      transition_observed = true;
    }

    const unsigned long long now = GetTickCount64();
    if (status->dwCheckPoint != checkpoint) {
      checkpoint = status->dwCheckPoint;
      last_progress = now;
    }
    const unsigned long wait_hint =
        status->dwWaitHint ? status->dwWaitHint : kServiceStatusDeadlineMs;
    if (now - last_progress >
        static_cast<unsigned long long>(wait_hint) +
            kServiceWaitHintMarginMs) {
      if (error) *error = ERROR_SERVICE_REQUEST_TIMEOUT;
      return response;
    }
    unsigned long interval = wait_hint / 10;
    if (interval < 100) interval = 100;
    if (interval > 1000) interval = 1000;
    Sleep(interval);
  }
}

unsigned long service_control_access(unsigned long control) {
  unsigned long access = SERVICE_QUERY_STATUS;
  switch (control) {
    case kServiceControlStart: return access | SERVICE_START;
    case SERVICE_CONTROL_CONTINUE:
    case SERVICE_CONTROL_PAUSE: return access | SERVICE_PAUSE_CONTINUE;
    case SERVICE_CONTROL_STOP: return access | SERVICE_STOP;
    case kServiceControlRotate:
    case kServiceControlReload:
      return access | SERVICE_USER_DEFINED_CONTROL;
    default: return access;
  }
}

struct process_collection_t {
  std::vector<service_process_entry_t> *entries = nullptr;
  unsigned long error = ERROR_SUCCESS;
};

int collect_process(nssm_service_t *, kill_t *process) {
  auto *collection = static_cast<process_collection_t *>(process->user_data);
  if (! collection || ! collection->entries) return 1;
  if (collection->error) return 1;
  try {
    service_process_entry_t entry;
    entry.pid = process->pid;
    entry.depth = process->depth;
    entry.executable.assign(PATH_LENGTH, L'\0');
    unsigned long length = static_cast<unsigned long>(entry.executable.size());
    if (QueryFullProcessImageNameW(process->process_handle, 0,
                                   entry.executable.data(), &length)) {
      entry.executable.resize(length);
    }
    else {
      entry.executable = L"???";
    }
    collection->entries->push_back(std::move(entry));
  }
  catch (const std::bad_alloc &) {
    collection->error = ERROR_NOT_ENOUGH_MEMORY;
  }
  return 1;
}

}  // namespace

bool execute_service_control(unsigned long control,
                             const std::wstring &service_name,
                             service_control_result_t *result) {
  if (! result || service_name.empty()) {
    if (result) result->windows_error = ERROR_INVALID_PARAMETER;
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  *result = {};
  result->service_name = service_name;

  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
  if (! manager) {
    result->windows_error = GetLastError();
    return false;
  }
  unique_service_handle service(OpenServiceW(
      manager.get(), service_name.c_str(), service_control_access(control)));
  if (! service) {
    result->windows_error = GetLastError();
    return false;
  }

  BOOL accepted = FALSE;
  unsigned long initial_status = SERVICE_STOPPED;
  if (control == kServiceControlStart) {
    accepted = StartServiceW(service.get(), 0, nullptr);
  }
  else if (control == SERVICE_CONTROL_INTERROGATE) {
    accepted = QueryServiceStatus(service.get(), &result->status);
    if (! accepted) result->windows_error = GetLastError();
    return accepted != FALSE;
  }
  else {
    accepted = ControlService(service.get(), control, &result->status);
    initial_status = result->status.dwCurrentState;
    const unsigned long control_error =
        accepted ? ERROR_SUCCESS : GetLastError();
    if (! accepted && control_error == ERROR_IO_PENDING) accepted = TRUE;
    if (! accepted) result->windows_error = control_error;
  }

  if (! accepted) {
    if (control == kServiceControlStart) {
      result->windows_error = GetLastError();
    }
    if (result->windows_error == ERROR_SERVICE_NOT_ACTIVE &&
        (control == SERVICE_CONTROL_SHUTDOWN ||
         control == SERVICE_CONTROL_STOP)) {
      result->status.dwCurrentState = SERVICE_STOPPED;
      result->windows_error = ERROR_SUCCESS;
      return true;
    }
    return false;
  }

  result->wait_response = await_service_control_response(
      control, service.get(), &result->status, initial_status,
      &result->windows_error);
  return result->wait_response == 0;
}

bool query_service_process_tree(
    const std::wstring &service_name,
    std::vector<service_process_entry_t> *entries, unsigned long *error) {
  if (! entries || service_name.empty()) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  entries->clear();
  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
  if (! manager) {
    if (error) *error = GetLastError();
    return false;
  }
  unique_service_handle service(OpenServiceW(
      manager.get(), service_name.c_str(), SERVICE_QUERY_STATUS));
  if (! service) {
    if (error) *error = GetLastError();
    return false;
  }
  SERVICE_STATUS_PROCESS status = {};
  unsigned long needed = 0;
  if (! QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<unsigned char *>(&status),
                             sizeof(status), &needed)) {
    if (error) *error = GetLastError();
    return false;
  }
  if (! status.dwProcessId) {
    if (error) *error = ERROR_SUCCESS;
    return true;
  }

  debug_privilege_guard debug_privilege;
  unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    status.dwProcessId));
  if (! process) {
    if (error) *error = GetLastError();
    return false;
  }
  kill_t context = {};
  context.name = service_name.c_str();
  context.pid = status.dwProcessId;
  context.process_handle = process.get();
  if (get_process_creation_time(process.get(), &context.creation_time)) {
    if (error) *error = GetLastError();
    return false;
  }
  GetSystemTimeAsFileTime(&context.exit_time);
  process_collection_t collection = {entries, ERROR_SUCCESS};
  context.user_data = &collection;
  walk_process_tree(nullptr, collect_process, &context, context.pid);
  const unsigned long traversal_error = collection.error
                                            ? collection.error
                                            : context.traversal_error;
  if (traversal_error) {
    entries->clear();
    if (error) *error = traversal_error;
    return false;
  }
  if (error) *error = ERROR_SUCCESS;
  return true;
}

service_removal_status_t remove_managed_service(
    const std::wstring &service_name, unsigned long *error) {
  if (service_name.empty()) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return service_removal_status_t::system_error;
  }
  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
  if (! manager) {
    if (error) *error = GetLastError();
    return service_removal_status_t::system_error;
  }
  unique_service_handle service(
      OpenServiceW(manager.get(), service_name.c_str(), DELETE));
  if (! service) {
    if (error) *error = GetLastError();
    return service_removal_status_t::system_error;
  }

  std::wstring config_path;
  unsigned long registration_error = ERROR_SUCCESS;
  if (! registered_service_config_path(service_name.c_str(), &config_path,
                                        &registration_error)) {
    if (error) *error = registration_error;
    if (registration_error == ERROR_FILE_NOT_FOUND ||
        registration_error == ERROR_PATH_NOT_FOUND ||
        registration_error == ERROR_INVALID_DATA) {
      return service_removal_status_t::unmanaged;
    }
    return service_removal_status_t::system_error;
  }
  if (! DeleteService(service.get())) {
    if (error) *error = GetLastError();
    return service_removal_status_t::system_error;
  }
  if (error) *error = ERROR_SUCCESS;
  return service_removal_status_t::success;
}

bool set_service_start_type(const std::wstring &service_name,
                            unsigned long start_type,
                            unsigned long *error) {
  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
  if (! manager) {
    if (error) *error = GetLastError();
    return false;
  }
  unique_service_handle service(OpenServiceW(manager.get(), service_name.c_str(),
                                              SERVICE_CHANGE_CONFIG));
  if (! service || ! ChangeServiceConfigW(
                        service.get(), SERVICE_NO_CHANGE, start_type,
                        SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr, nullptr)) {
    if (error) *error = GetLastError();
    return false;
  }
  if (error) *error = ERROR_SUCCESS;
  return true;
}

bool query_service_start_type(const std::wstring &service_name,
                              unsigned long *start_type,
                              unsigned long *error) {
  if (! start_type) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
  if (! manager) {
    if (error) *error = GetLastError();
    return false;
  }
  unique_service_handle service(OpenServiceW(manager.get(), service_name.c_str(),
                                              SERVICE_QUERY_CONFIG));
  if (! service) {
    if (error) *error = GetLastError();
    return false;
  }
  unsigned long needed = 0;
  QueryServiceConfigW(service.get(), nullptr, 0, &needed);
  if (! needed || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    if (error) *error = GetLastError();
    return false;
  }
  std::vector<unsigned char> buffer(needed);
  auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
  if (! QueryServiceConfigW(service.get(), config, needed, &needed)) {
    if (error) *error = GetLastError();
    return false;
  }
  *start_type = config->dwStartType;
  if (error) *error = ERROR_SUCCESS;
  return true;
}
