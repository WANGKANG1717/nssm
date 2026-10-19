#include "service_enumeration.h"

#include "config_registration.h"
#include "win32_handle.h"

#include <windows.h>

#include <new>
#include <utility>

namespace {

void save_error(unsigned long *error, unsigned long value) {
  if (error) *error = value;
}

bool unmanaged_service_error(unsigned long error) {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

}  // namespace

bool enumerate_windows_services(bool include_all,
                                std::vector<service_enumeration_entry_t> *entries,
                                unsigned long *error) {
  if (! entries) {
    save_error(error, ERROR_INVALID_PARAMETER);
    return false;
  }
  entries->clear();

  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE,
      SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
  if (! manager) {
    save_error(error, GetLastError());
    return false;
  }

  unsigned long required = 0;
  unsigned long count = 0;
  unsigned long resume = 0;
  const BOOL initially_complete = EnumServicesStatusExW(
      manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
      nullptr, 0, &required, &count, &resume, nullptr);
  unsigned long query_error = initially_complete ? ERROR_SUCCESS : GetLastError();
  if (initially_complete && ! required) {
    save_error(error, ERROR_SUCCESS);
    return true;
  }
  if (query_error != ERROR_MORE_DATA || ! required) {
    save_error(error, query_error);
    return false;
  }

  try {
    std::vector<unsigned char> buffer(required);
    while (true) {
      const BOOL complete = EnumServicesStatusExW(
          manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
          SERVICE_STATE_ALL, buffer.data(),
          static_cast<unsigned long>(buffer.size()), &required, &count,
          &resume, nullptr);
      query_error = complete ? ERROR_SUCCESS : GetLastError();
      if (! complete && query_error != ERROR_MORE_DATA) {
        save_error(error, query_error);
        return false;
      }

      const auto *statuses =
          reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW *>(buffer.data());
      for (unsigned long index = 0; index < count; ++index) {
        if (! include_all) {
          std::wstring config_path;
          unsigned long registration_error = ERROR_SUCCESS;
          if (! registered_service_config_path(statuses[index].lpServiceName,
                                                &config_path,
                                                &registration_error)) {
            if (unmanaged_service_error(registration_error)) continue;
            save_error(error, registration_error);
            return false;
          }
        }
        service_enumeration_entry_t entry;
        entry.name = statuses[index].lpServiceName;
        entry.display_name = statuses[index].lpDisplayName;
        entry.state = statuses[index].ServiceStatusProcess.dwCurrentState;
        entry.process_id = statuses[index].ServiceStatusProcess.dwProcessId;
        entries->push_back(std::move(entry));
      }
      if (complete) {
        save_error(error, ERROR_SUCCESS);
        return true;
      }
      if (required > buffer.size()) buffer.resize(required);
    }
  }
  catch (const std::bad_alloc &) {
    entries->clear();
    save_error(error, ERROR_NOT_ENOUGH_MEMORY);
    return false;
  }
}
