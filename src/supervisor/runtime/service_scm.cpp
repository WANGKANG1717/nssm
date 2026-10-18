#include "event.h"
#include "messages.h"
#include "security.h"
#include "service.h"
#include "string_utils.h"

#include <vector>

SC_HANDLE open_service_manager(unsigned long access) {
  SC_HANDLE manager =
      OpenSCManager(nullptr, SERVICES_ACTIVE_DATABASE, access);
  if (! manager && is_process_administrator()) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENSCMANAGER_FAILED, 0);
  }
  return manager;
}

SC_HANDLE open_service(SC_HANDLE manager, TCHAR *service_name,
                       unsigned long access, TCHAR *canonical_name,
                       unsigned long canonical_name_length) {
  SC_HANDLE service = OpenService(manager, service_name, access);
  if (service) {
    if (canonical_name && canonical_name != service_name) {
      TCHAR display_name[SERVICE_NAME_LENGTH] = {};
      unsigned long display_name_length = _countof(display_name);
      if (! GetServiceDisplayName(manager, service_name, display_name,
                                  &display_name_length)) {
        display_name[0] = _T('\0');
      }
      unsigned long key_name_length = canonical_name_length;
      if (! display_name[0] ||
          ! GetServiceKeyName(manager, display_name, canonical_name,
                              &key_name_length)) {
        _sntprintf_s(canonical_name, canonical_name_length, _TRUNCATE,
                     _T("%s"), service_name);
      }
    }
    return service;
  }

  unsigned long error = GetLastError();
  if (error != ERROR_SERVICE_DOES_NOT_EXIST || ! canonical_name) {
    print_message(stderr, NSSM_MESSAGE_OPENSERVICE_FAILED, error_string(error));
    return nullptr;
  }

  unsigned long required = 0;
  unsigned long count = 0;
  unsigned long resume = 0;
  EnumServicesStatusEx(manager, SC_ENUM_PROCESS_INFO,
                       SERVICE_DRIVER | SERVICE_FILE_SYSTEM_DRIVER |
                           SERVICE_KERNEL_DRIVER | SERVICE_WIN32,
                       SERVICE_STATE_ALL, nullptr, 0, &required, &count,
                       &resume, nullptr);
  error = GetLastError();
  if (error != ERROR_MORE_DATA || ! required) {
    print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
                  error_string(error));
    return nullptr;
  }

  std::vector<unsigned char> buffer(required);
  do {
    int complete = EnumServicesStatusEx(
        manager, SC_ENUM_PROCESS_INFO,
        SERVICE_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_KERNEL_DRIVER |
            SERVICE_WIN32,
        SERVICE_STATE_ALL, buffer.data(),
        static_cast<unsigned long>(buffer.size()), &required, &count, &resume,
        nullptr);
    error = complete ? ERROR_SUCCESS : GetLastError();
    if (! complete && error != ERROR_MORE_DATA) {
      print_message(stderr, NSSM_MESSAGE_ENUMSERVICESSTATUS_FAILED,
                    error_string(error));
      return nullptr;
    }

    auto *statuses =
        reinterpret_cast<ENUM_SERVICE_STATUS_PROCESS *>(buffer.data());
    for (unsigned long index = 0; index < count; ++index) {
      if (! strings_equal_ignore_case(statuses[index].lpDisplayName,
                                      service_name)) {
        continue;
      }
      if (_sntprintf_s(canonical_name, canonical_name_length, _TRUNCATE,
                       _T("%s"), statuses[index].lpServiceName) < 0) {
        print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY,
                      _T("canonical_name"), _T("open_service()"));
        return nullptr;
      }
      return open_service(manager, canonical_name, access, nullptr, 0);
    }
    if (complete) break;
  } while (true);

  return open_service(manager, service_name, access, nullptr, 0);
}
