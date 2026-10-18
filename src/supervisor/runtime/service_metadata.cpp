#include "nssm.h"
#include "account.h"
#include "event.h"
#include "messages.h"
#include "service.h"

#include <vector>

namespace {

bool query_service_config2(SC_HANDLE service, unsigned long level,
                           std::vector<unsigned char> *buffer,
                           const TCHAR *service_name,
                           const TCHAR *level_name) {
  unsigned long bytes = 0;
  QueryServiceConfig2(service, level, nullptr, 0, &bytes);
  unsigned long error = GetLastError();
  if (error != ERROR_INSUFFICIENT_BUFFER || ! bytes) {
    print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG2_FAILED,
                  service_name, level_name, error_string(error));
    return false;
  }

  buffer->resize(bytes);
  if (! QueryServiceConfig2(service, level, buffer->data(), bytes, &bytes)) {
    error = GetLastError();
    print_message(stderr, NSSM_MESSAGE_QUERYSERVICECONFIG2_FAILED,
                  service_name, level_name, error_string(error));
    return false;
  }
  return true;
}

}  // namespace

int set_service_description(const TCHAR *service_name,
                            SC_HANDLE service_handle, TCHAR *buffer) {
  SERVICE_DESCRIPTION description = {};
  description.lpDescription = buffer && buffer[0] ? buffer : const_cast<TCHAR *>(_T(""));
  if (ChangeServiceConfig2(service_handle, SERVICE_CONFIG_DESCRIPTION,
                           &description)) {
    return 0;
  }
  log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SERVICE_CONFIG_DESCRIPTION_FAILED,
            service_name, error_string(GetLastError()), 0);
  return 1;
}

int get_service_description(const TCHAR *service_name,
                            SC_HANDLE service_handle, unsigned long length,
                            TCHAR *buffer) {
  if (! buffer) return 1;
  std::vector<unsigned char> data;
  if (! query_service_config2(service_handle, SERVICE_CONFIG_DESCRIPTION, &data,
                              service_name,
                              _T("SERVICE_CONFIG_DESCRIPTION"))) {
    return 3;
  }
  auto *description = reinterpret_cast<SERVICE_DESCRIPTION *>(data.data());
  if (description->lpDescription) {
    _sntprintf_s(buffer, length, _TRUNCATE, _T("%s"),
                 description->lpDescription);
  }
  else ZeroMemory(buffer, length * sizeof(TCHAR));
  return 0;
}

int get_service_startup(const TCHAR *service_name, SC_HANDLE service_handle,
                        const QUERY_SERVICE_CONFIG *config,
                        unsigned long *startup) {
  if (! config || ! startup) return 1;
  switch (config->dwStartType) {
    case SERVICE_DEMAND_START:
      *startup = NSSM_STARTUP_MANUAL;
      return 0;
    case SERVICE_DISABLED:
      *startup = NSSM_STARTUP_DISABLED;
      return 0;
    default:
      *startup = NSSM_STARTUP_AUTOMATIC;
      break;
  }

  std::vector<unsigned char> data;
  if (! query_service_config2(service_handle,
                              SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &data,
                              service_name,
                              _T("SERVICE_CONFIG_DELAYED_AUTO_START_INFO"))) {
    return 3;
  }
  auto *delayed =
      reinterpret_cast<SERVICE_DELAYED_AUTO_START_INFO *>(data.data());
  if (delayed->fDelayedAutostart) *startup = NSSM_STARTUP_DELAYED;
  return 0;
}

int get_service_username(const TCHAR *service_name,
                         const QUERY_SERVICE_CONFIG *config, TCHAR **username,
                         size_t *username_length) {
  (void) service_name;
  if (! username || ! username_length || ! config) return 1;
  *username = nullptr;
  *username_length = 0;
  if (! config->lpServiceStartName[0] ||
      is_localsystem(config->lpServiceStartName)) {
    return 0;
  }

  size_t length = _tcslen(config->lpServiceStartName);
  *username = static_cast<TCHAR *>(
      HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(TCHAR)));
  if (! *username) {
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, _T("username"),
                  _T("get_service_username()"));
    return 2;
  }
  memmove(*username, config->lpServiceStartName,
          (length + 1) * sizeof(TCHAR));
  *username_length = length;
  return 0;
}
