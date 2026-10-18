#include "nssm.h"
#include "event.h"
#include "messages.h"
#include "service.h"
#include "win32_handle.h"

namespace {

int service_control_response(unsigned long control, unsigned long status) {
  switch (control) {
    case NSSM_SERVICE_CONTROL_START:
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
    case NSSM_SERVICE_CONTROL_ROTATE:
    default:
      return 0;
  }
}

int await_service_control_response(unsigned long control, SC_HANDLE service,
                                   SERVICE_STATUS *status,
                                   unsigned long initial_status,
                                   unsigned long cutoff = 0) {
  int tries = 0;
  unsigned long checkpoint = 0;
  unsigned long wait_hint = 0;
  unsigned long waited = 0;
  while (QueryServiceStatus(service, status)) {
    int response = service_control_response(control, status->dwCurrentState);
    if (! response) return 0;
    if (response < 0 && status->dwCurrentState != initial_status) return response;

    if (status->dwCheckPoint != checkpoint || status->dwWaitHint != wait_hint) {
      tries = 0;
    }
    checkpoint = status->dwCheckPoint;
    wait_hint = status->dwWaitHint;
    if (++tries > 10) tries = 10;
    unsigned long wait = 50 * tries;
    if (cutoff && waited > cutoff) return response;
    waited += wait;
    Sleep(wait);
  }
  return -1;
}

}  // namespace

int pre_remove_service(int argc, TCHAR **argv) {
  nssm_service_t *service = alloc_nssm_service();
  if (! service) return 1;
  set_nssm_service_defaults(service);
  if (argc) {
    _sntprintf_s(service->name, _countof(service->name), _TRUNCATE,
                 _T("%s"), argv[0]);
  }
  if (argc >= 2 && str_equiv(argv[1], _T("confirm"))) {
    int result = remove_service(service);
    cleanup_nssm_service(service);
    return result;
  }
  cleanup_nssm_service(service);
  print_message(stderr, NSSM_MESSAGE_PRE_REMOVE_SERVICE);
  return 100;
}

int control_service(unsigned long control, int argc, TCHAR **argv,
                    bool return_status) {
  if (argc < 1) return usage(1);
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
    case NSSM_SERVICE_CONTROL_START:
      access |= SERVICE_START;
      break;
    case SERVICE_CONTROL_CONTINUE:
    case SERVICE_CONTROL_PAUSE:
      access |= SERVICE_PAUSE_CONTINUE;
      break;
    case SERVICE_CONTROL_STOP:
      access |= SERVICE_STOP;
      break;
    case NSSM_SERVICE_CONTROL_ROTATE:
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
  if (control == NSSM_SERVICE_CONTROL_START) {
    result = StartService(service.get(), static_cast<unsigned long>(argc),
                          const_cast<const TCHAR **>(argv));
    error = result ? ERROR_SUCCESS : GetLastError();
    if (result) {
      int response = await_service_control_response(
          control, service.get(), &status, SERVICE_STOPPED,
          NSSM_RESET_THROTTLE_RESTART);
      if (response) {
        print_message(stderr, NSSM_MESSAGE_BAD_CONTROL_RESPONSE,
                      canonical_name,
                      service_status_text(status.dwCurrentState),
                      service_control_text(control));
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
      int response = await_service_control_response(
          control, service.get(), &status, initial_status);
      if (response) {
        print_message(stderr, NSSM_MESSAGE_BAD_CONTROL_RESPONSE,
                      canonical_name,
                      service_status_text(status.dwCurrentState),
                      service_control_text(control));
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

int remove_service(nssm_service_t *service) {
  if (! service) return 1;
  unique_service_handle manager(open_service_manager(
      SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
  if (! manager) {
    print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
    return 2;
  }

  service->handle = open_service(manager.get(), service->name, DELETE,
                                 service->name, _countof(service->name));
  if (! service->handle) return 3;

  unsigned long length = _countof(service->displayname);
  GetServiceDisplayName(manager.get(), service->name, service->displayname,
                        &length);
  length = _countof(service->name);
  GetServiceKeyName(manager.get(), service->displayname, service->name,
                    &length);
  if (! DeleteService(service->handle)) {
    print_message(stderr, NSSM_MESSAGE_DELETESERVICE_FAILED);
    return 4;
  }
  print_message(stdout, NSSM_MESSAGE_SERVICE_REMOVED, service->name);
  return 0;
}
