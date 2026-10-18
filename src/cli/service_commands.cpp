#include "event.h"
#include "messages.h"
#include "service_control.h"
#include "service_constants.h"
#include "service_commands.h"
#include "service_enumeration.h"
#include "service_operations.h"
#include "string_utils.h"
#include "usage.h"
#include "windows_limits.h"

#include <string>
#include <vector>

namespace {

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
  std::vector<std::wstring> names;
  unsigned long error = ERROR_SUCCESS;
  if (! enumerate_windows_services(including_native, &names, &error)) {
    print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
                  error_string(error));
    return 1;
  }
  for (const auto &name : names) _tprintf(_T("%s\n"), name.c_str());
  return 0;
}

int service_process_tree(int argc, TCHAR **argv) {
  if (argc < 1) return usage(1);
  int errors = 0;
  for (int i = 0; i < argc; ++i) {
    std::vector<service_process_entry_t> entries;
    unsigned long error = ERROR_SUCCESS;
    if (! query_service_process_tree(argv[i], &entries, &error)) {
      _ftprintf(stderr, _T("%s: %s\n"), argv[i], error_string(error));
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

int pre_remove_service(int argc, TCHAR **argv) {
  if (argc != 2 || ! argv[0] || ! argv[0][0] ||
      ! strings_equal_ignore_case(argv[1], _T("confirm"))) {
    print_message(stderr, NSSM_MESSAGE_PRE_REMOVE_SERVICE);
    return 100;
  }
  if (_tcslen(argv[0]) >= SERVICE_NAME_LENGTH) return usage(1);
  unsigned long error = ERROR_SUCCESS;
  const service_removal_status_t status =
      remove_managed_service(argv[0], &error);
  if (status == service_removal_status_t::unmanaged) {
    _ftprintf(stderr,
              _T("%s: refusing to remove a service not managed by this NSSM: %s\n"),
              argv[0], error_string(error));
    return 4;
  }
  if (status == service_removal_status_t::system_error) {
    _ftprintf(stderr, _T("%s: DeleteService: %s\n"), argv[0],
              error_string(error));
    return 5;
  }
  print_message(stdout, NSSM_MESSAGE_SERVICE_REMOVED, argv[0]);
  return 0;
}

int control_service(unsigned long control, int argc, TCHAR **argv,
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
      _ftprintf(stderr, _T("%s: %s: %s"), result.service_name.c_str(),
                service_control_text(control),
                error_string(result.windows_error));
    }
    return return_status ? 0 : 1;
  }
  if (control == SERVICE_CONTROL_INTERROGATE) {
    _tprintf(_T("%s\n"), service_status_text(result.status.dwCurrentState));
  }
  else {
    _tprintf(_T("%s: %s: %s"), result.service_name.c_str(),
             service_control_text(control), error_string(ERROR_SUCCESS));
  }
  return return_status ? result.status.dwCurrentState : 0;
}

int control_service(unsigned long control, int argc, TCHAR **argv) {
  return control_service(control, argc, argv, false);
}
