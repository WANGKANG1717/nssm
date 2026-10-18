#include "nssm.h"
#include "config.h"
#include "config_registration.h"
#include "event.h"
#include "messages.h"
#include "process.h"
#include "service.h"
#include "service_constants.h"
#include "service_commands.h"
#include "string_utils.h"
#include "usage.h"
#include "win32_handle.h"

#include <new>
#include <string>
#include <vector>

namespace {

int remove_service(nssm_service_t *service);

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
    default:
      return 0;
  }
}

int await_service_control_response(unsigned long control, SC_HANDLE service,
                                   SERVICE_STATUS *status,
                                   unsigned long initial_status,
                                   unsigned long *query_error) {
  if (query_error) *query_error = ERROR_SUCCESS;
  unsigned long long last_progress = GetTickCount64();
  unsigned long checkpoint = 0;
  while (true) {
    if (! QueryServiceStatus(service, status)) {
      if (query_error) *query_error = GetLastError();
      return -2;
    }
    int response = service_control_response(control, status->dwCurrentState);
    if (! response) return 0;
    if (response < 0 && status->dwCurrentState != initial_status) return response;

    const unsigned long long now = GetTickCount64();
    if (status->dwCheckPoint != checkpoint) {
      checkpoint = status->dwCheckPoint;
      last_progress = now;
    }
    const unsigned long wait_hint =
        status->dwWaitHint ? status->dwWaitHint : kServiceStatusDeadlineMs;
    if (now - last_progress >
        static_cast<unsigned long long>(wait_hint) + kServiceWaitHintMarginMs) {
      return response;
    }
    unsigned long interval = wait_hint / 10;
    if (interval < 100) interval = 100;
    if (interval > 1000) interval = 1000;
    Sleep(interval);
  }
}

void print_control_wait_failure(const TCHAR *service_name,
                                unsigned long control,
                                const SERVICE_STATUS &status,
                                int response,
                                unsigned long query_error) {
  if (response == -2) {
    _ftprintf(stderr, _T("%s: QueryServiceStatus: %s\n"), service_name,
              error_string(query_error));
    return;
  }
  print_message(stderr, NSSM_MESSAGE_BAD_CONTROL_RESPONSE, service_name,
                service_status_text(status.dwCurrentState),
                service_control_text(control));
}

}  // namespace

int list_nssm_services(int argc, TCHAR **argv) {
  const bool including_native =
      argc > 0 && strings_equal_ignore_case(argv[0], _T("all"));
  unique_service_handle manager(open_service_manager(
      SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
  if (! manager) {
    print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
    return 1;
  }

  unsigned long required = 0;
  unsigned long count = 0;
  unsigned long resume = 0;
  EnumServicesStatusEx(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                       SERVICE_STATE_ALL, nullptr, 0, &required, &count,
                       &resume, nullptr);
  unsigned long error = GetLastError();
  if (error != ERROR_MORE_DATA) {
    print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
                  error_string(error));
    return 2;
  }

  std::vector<unsigned char> buffer;
  try {
    buffer.resize(required);
  }
  catch (const std::bad_alloc &) {
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
                  _T("ENUM_SERVICE_STATUS_PROCESS"),
                  _T("list_nssm_services()"));
    return 3;
  }
  while (true) {
    const BOOL complete = EnumServicesStatusEx(
        manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        buffer.data(), static_cast<unsigned long>(buffer.size()), &required,
        &count, &resume, nullptr);
    if (! complete) {
      error = GetLastError();
      if (error != ERROR_MORE_DATA) {
        print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
                      error_string(error));
        return 4;
      }
    }

    auto *statuses = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESS *>(
        buffer.data());
    for (unsigned long i = 0; i < count; ++i) {
      std::wstring config_path;
      unsigned long config_error = ERROR_SUCCESS;
      const bool managed = registered_service_config_path(
          statuses[i].lpServiceName, &config_path, &config_error);
      if (including_native || managed) {
        _tprintf(_T("%s\n"), statuses[i].lpServiceName);
      }
    }

    if (complete) return 0;
  }
}

int service_process_tree(int argc, TCHAR **argv) {
  if (argc < 1) return usage(1);

  unique_service_handle manager(open_service_manager(SC_MANAGER_CONNECT));
  if (! manager) {
    print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
    return 1;
  }

  /* Failure is reported later if the process cannot be opened. */
  unique_handle debug_token(get_debug_token());
  int errors = 0;
  for (int i = 0; i < argc; ++i) {
    TCHAR canonical_name[SERVICE_NAME_LENGTH] = {};
    unique_service_handle service_handle(open_service(
        manager.get(), argv[i], SERVICE_QUERY_STATUS, canonical_name,
        _countof(canonical_name)));
    if (! service_handle) {
      ++errors;
      continue;
    }

    SERVICE_STATUS_PROCESS service_status = {};
    unsigned long size = 0;
    const BOOL queried = QueryServiceStatusEx(
        service_handle.get(), SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&service_status), sizeof(service_status),
        &size);
    if (! queried) {
      const unsigned long error = GetLastError();
      _ftprintf(stderr, _T("%s: %s\n"), canonical_name,
                error_string(error));
      ++errors;
      continue;
    }

    kill_t context = {};
    context.pid = service_status.dwProcessId;
    if (! context.pid) continue;

    unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                      FALSE, context.pid));
    if (! process) {
      _ftprintf(stderr, _T("%s: %lu: %s\n"), canonical_name, context.pid,
                error_string(GetLastError()));
      ++errors;
      continue;
    }
    context.process_handle = process.get();
    if (get_process_creation_time(context.process_handle,
                                  &context.creation_time)) {
      ++errors;
      continue;
    }
    GetSystemTimeAsFileTime(&context.exit_time);

    nssm_service_t *service = alloc_nssm_service();
    if (! service) {
      ++errors;
      continue;
    }
    _sntprintf_s(service->name, _countof(service->name), _TRUNCATE,
                 _T("%s"), canonical_name);
    context.name = service->name;
    walk_process_tree(service, print_process, &context, context.pid);
    cleanup_nssm_service(service);
  }

  return errors;
}

int pre_remove_service(int argc, TCHAR **argv) {
  if (argc != 2 || ! argv[0] || ! argv[0][0] ||
      ! strings_equal_ignore_case(argv[1], _T("confirm"))) {
    print_message(stderr, NSSM_MESSAGE_PRE_REMOVE_SERVICE);
    return 100;
  }
  if (_tcslen(argv[0]) >= SERVICE_NAME_LENGTH) return usage(1);

  nssm_service_t *service = alloc_nssm_service();
  if (! service) return 1;
  set_nssm_service_defaults(service);
  wmemcpy(service->name, argv[0], _tcslen(argv[0]) + 1);
  int result = remove_service(service);
  cleanup_nssm_service(service);
  return result;
}

int control_service(unsigned long control, int argc, TCHAR **argv,
                    bool return_status) {
  if (argc != 1) return usage(1);
  TCHAR *service_name = argv[0];
  TCHAR canonical_name[SERVICE_NAME_LENGTH] = {};

  unique_service_handle manager(open_service_manager(
      SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
  if (! manager) {
    print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
    return return_status ? 0 : 2;
  }

  unsigned long access = SERVICE_QUERY_STATUS;
  switch (control) {
    case kServiceControlStart:
      access |= SERVICE_START;
      break;
    case SERVICE_CONTROL_CONTINUE:
    case SERVICE_CONTROL_PAUSE:
      access |= SERVICE_PAUSE_CONTINUE;
      break;
    case SERVICE_CONTROL_STOP:
      access |= SERVICE_STOP;
      break;
    case kServiceControlRotate:
      access |= SERVICE_USER_DEFINED_CONTROL;
      break;
  }

  unique_service_handle service(open_service(
      manager.get(), service_name, access, canonical_name,
      _countof(canonical_name)));
  if (! service) return return_status ? 0 : 3;

  SERVICE_STATUS status = {};
  int result = 0;
  unsigned long error = ERROR_SUCCESS;
  if (control == kServiceControlStart) {
    result = StartService(service.get(), 0, nullptr);
    error = result ? ERROR_SUCCESS : GetLastError();
    if (result) {
      unsigned long query_error = ERROR_SUCCESS;
      int response = await_service_control_response(
          control, service.get(), &status, SERVICE_STOPPED, &query_error);
      if (response) {
        print_control_wait_failure(canonical_name, control, status, response,
                                   query_error);
        return return_status ? 0 : 1;
      }
      _tprintf(_T("%s: %s: %s"), canonical_name,
               service_control_text(control), error_string(error));
      return 0;
    }
  }
  else if (control == SERVICE_CONTROL_INTERROGATE) {
    result = QueryServiceStatus(service.get(), &status);
    error = result ? ERROR_SUCCESS : GetLastError();
    if (result) {
      _tprintf(_T("%s\n"), service_status_text(status.dwCurrentState));
      return return_status ? status.dwCurrentState : 0;
    }
  }
  else {
    result = ControlService(service.get(), control, &status);
    unsigned long initial_status = status.dwCurrentState;
    error = result ? ERROR_SUCCESS : GetLastError();
    if (! result && error == ERROR_IO_PENDING) {
      result = 1;
      error = ERROR_SUCCESS;
    }
    if (result) {
      unsigned long query_error = ERROR_SUCCESS;
      int response = await_service_control_response(
          control, service.get(), &status, initial_status, &query_error);
      if (response) {
        print_control_wait_failure(canonical_name, control, status, response,
                                   query_error);
        return return_status ? 0 : 1;
      }
      _tprintf(_T("%s: %s: %s"), canonical_name,
               service_control_text(control), error_string(error));
      return return_status ? status.dwCurrentState : 0;
    }
  }

  _ftprintf(stderr, _T("%s: %s: %s"), canonical_name,
            service_control_text(control), error_string(error));
  if (error == ERROR_SERVICE_NOT_ACTIVE &&
      (control == SERVICE_CONTROL_SHUTDOWN ||
       control == SERVICE_CONTROL_STOP)) {
    return return_status ? SERVICE_STOPPED : 0;
  }
  return return_status ? 0 : 1;
}

int control_service(unsigned long control, int argc, TCHAR **argv) {
  return control_service(control, argc, argv, false);
}

namespace {

int remove_service(nssm_service_t *service) {
  if (! service) return 1;
  unique_service_handle manager(open_service_manager(
      SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
  if (! manager) {
    print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
    return 2;
  }

  service->handle = open_service(manager.get(), service->name, DELETE,
                                 nullptr, 0);
  if (! service->handle) return 3;

  std::wstring config_path;
  unsigned long registration_error = ERROR_SUCCESS;
  if (! registered_service_config_path(service->name, &config_path,
                                       &registration_error)) {
    _ftprintf(stderr,
              _T("%s: refusing to remove a service not managed by this NSSM: %s\n"),
              service->name, error_string(registration_error));
    return 4;
  }
  if (! DeleteService(service->handle)) {
    print_message(stderr, NSSM_MESSAGE_DELETESERVICE_FAILED);
    return 5;
  }
  print_message(stdout, NSSM_MESSAGE_SERVICE_REMOVED, service->name);
  return 0;
}

}  // namespace
