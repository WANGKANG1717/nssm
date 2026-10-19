#include "config.h"
#include "config_operations.h"
#include "config_registration.h"
#include "config_security.h"
#include "string_utils.h"
#include "win32_handle.h"

#include <string>
#include <vector>

namespace {

struct existing_service_state_t {
  unsigned long service_type = SERVICE_NO_CHANGE;
  unsigned long start_type = SERVICE_NO_CHANGE;
  unsigned long error_control = SERVICE_NO_CHANGE;
  std::wstring binary_path;
  std::wstring load_order_group;
  std::vector<wchar_t> dependencies;
  std::wstring account;
  std::wstring display_name;
  std::wstring description;
  bool delayed = false;
  unsigned long service_sid_type = SERVICE_SID_TYPE_NONE;
  std::wstring config_path;
};

std::vector<wchar_t> make_dependencies(
    const std::vector<std::wstring> &dependencies) {
  std::vector<wchar_t> result;
  for (const auto &dependency : dependencies) {
    result.insert(result.end(), dependency.begin(), dependency.end());
    result.push_back(L'\0');
  }
  result.push_back(L'\0');
  if (dependencies.empty()) result.push_back(L'\0');
  return result;
}

unsigned long service_start_type(service_startup_t startup) {
  switch (startup) {
    case service_startup_t::manual: return SERVICE_DEMAND_START;
    case service_startup_t::disabled: return SERVICE_DISABLED;
    case service_startup_t::automatic:
    case service_startup_t::delayed:
    default: return SERVICE_AUTO_START;
  }
}

unsigned long service_sid_type(service_sid_type_t type) {
  switch (type) {
    case service_sid_type_t::none: return SERVICE_SID_TYPE_NONE;
    case service_sid_type_t::restricted: return SERVICE_SID_TYPE_RESTRICTED;
    case service_sid_type_t::unrestricted:
    default: return SERVICE_SID_TYPE_UNRESTRICTED;
  }
}

bool set_service_sid_type(SC_HANDLE service, unsigned long type,
                          unsigned long *error) {
  SERVICE_SID_INFO info = {type};
  if (! ChangeServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO, &info)) {
    if (error) *error = GetLastError();
    return false;
  }
  if (error) *error = ERROR_SUCCESS;
  return true;
}

bool account_name(const service_config_t &config, std::wstring *name,
                  config_validation_t *validation) {
  switch (config.service.account.type) {
    case service_account_type_t::system:
      name->clear();
      return true;
    case service_account_type_t::local_service:
      *name = L"NT AUTHORITY\\LocalService";
      return true;
    case service_account_type_t::network_service:
      *name = L"NT AUTHORITY\\NetworkService";
      return true;
    case service_account_type_t::virtual_account:
      *name = L"NT SERVICE\\" + config.service.name;
      return true;
    case service_account_type_t::gmsa:
      if (! config.service.account.name.ends_with(L'$')) {
        validation->add(L"A gMSA account name must end with '$'");
        return false;
      }
      *name = config.service.account.name;
      return true;
    case service_account_type_t::user:
      validation->add(
          L"User accounts require secure credential input, which is not yet available");
      return false;
  }
  return false;
}

std::vector<wchar_t> copy_multisz(const wchar_t *source) {
  std::vector<wchar_t> result;
  if (source) {
    const wchar_t *cursor = source;
    do {
      size_t length = wcslen(cursor);
      result.insert(result.end(), cursor, cursor + length + 1);
      cursor += length + 1;
    } while (*cursor);
  }
  result.push_back(L'\0');
  if (result.size() == 1) result.push_back(L'\0');
  return result;
}

bool query_service_config2_string(SC_HANDLE service, unsigned long level,
                                  std::wstring *value,
                                  unsigned long *error) {
  unsigned long needed = 0;
  QueryServiceConfig2W(service, level, nullptr, 0, &needed);
  unsigned long query_error = GetLastError();
  if (! needed || query_error != ERROR_INSUFFICIENT_BUFFER) {
    if (error) *error = query_error;
    return false;
  }
  std::vector<unsigned char> buffer(needed);
  if (! QueryServiceConfig2W(service, level, buffer.data(), needed, &needed)) {
    if (error) *error = GetLastError();
    return false;
  }
  auto *description = reinterpret_cast<SERVICE_DESCRIPTIONW *>(buffer.data());
  *value = description->lpDescription ? description->lpDescription : L"";
  return true;
}

bool read_existing_service_state(SC_HANDLE service,
                                 const std::wstring &service_name,
                                 existing_service_state_t *state,
                                 unsigned long *error) {
  unsigned long needed = 0;
  QueryServiceConfigW(service, nullptr, 0, &needed);
  unsigned long query_error = GetLastError();
  if (! needed || query_error != ERROR_INSUFFICIENT_BUFFER) {
    if (error) *error = query_error;
    return false;
  }
  std::vector<unsigned char> buffer(needed);
  auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
  if (! QueryServiceConfigW(service, config, needed, &needed)) {
    if (error) *error = GetLastError();
    return false;
  }

  state->service_type = config->dwServiceType;
  state->start_type = config->dwStartType;
  state->error_control = config->dwErrorControl;
  state->binary_path = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
  state->load_order_group =
      config->lpLoadOrderGroup ? config->lpLoadOrderGroup : L"";
  state->dependencies = copy_multisz(config->lpDependencies);
  state->account = config->lpServiceStartName ? config->lpServiceStartName : L"";
  state->display_name = config->lpDisplayName ? config->lpDisplayName : L"";

  if (! query_service_config2_string(service, SERVICE_CONFIG_DESCRIPTION,
                                     &state->description, error)) {
    return false;
  }
  SERVICE_DELAYED_AUTO_START_INFO delayed = {};
  needed = 0;
  if (! QueryServiceConfig2W(
          service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
          reinterpret_cast<unsigned char *>(&delayed), sizeof(delayed),
          &needed)) {
    if (error) *error = GetLastError();
    return false;
  }
  state->delayed = delayed.fDelayedAutostart != FALSE;
  SERVICE_SID_INFO sid = {};
  needed = 0;
  if (! QueryServiceConfig2W(
          service, SERVICE_CONFIG_SERVICE_SID_INFO,
          reinterpret_cast<unsigned char *>(&sid), sizeof(sid), &needed)) {
    if (error) *error = GetLastError();
    return false;
  }
  state->service_sid_type = sid.dwServiceSidType;
  return read_config_path(service_name, &state->config_path, error);
}

bool passwordless_service_account(const std::wstring &account) {
  if (strings_equal_ignore_case(account.c_str(), L"LocalSystem") ||
      strings_equal_ignore_case(account.c_str(),
                                L"NT AUTHORITY\\LocalService") ||
      strings_equal_ignore_case(account.c_str(),
                                L"NT AUTHORITY\\NetworkService")) {
    return true;
  }
  if (string_starts_with_ignore_case(account, L"NT SERVICE\\")) {
    return true;
  }
  return account.ends_with(L'$');
}

bool restore_existing_service(SC_HANDLE service,
                              const std::wstring &service_name,
                              const existing_service_state_t &state) {
  bool restored = ChangeServiceConfigW(
      service, state.service_type, state.start_type, state.error_control,
      state.binary_path.c_str(),
      state.load_order_group.empty() ? nullptr : state.load_order_group.c_str(),
      nullptr, state.dependencies.data(), state.account.c_str(), nullptr,
      state.display_name.c_str()) != FALSE;
  SERVICE_DESCRIPTIONW description = {
      const_cast<wchar_t *>(state.description.c_str())};
  restored = ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION,
                                   &description) != FALSE && restored;
  SERVICE_DELAYED_AUTO_START_INFO delayed = {state.delayed};
  restored = ChangeServiceConfig2W(service,
                                   SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                   &delayed) != FALSE && restored;
  unsigned long sid_error = ERROR_SUCCESS;
  restored = set_service_sid_type(service, state.service_sid_type, &sid_error) &&
             restored;
  unsigned long registry_error = ERROR_SUCCESS;
  restored = write_config_path(service_name, state.config_path,
                               &registry_error) && restored;
  return restored;
}

std::vector<std::wstring> config_changes(const service_config_t &current,
                                         const service_config_t &desired,
                                         const std::wstring &current_path,
                                         const std::wstring &desired_path) {
  std::vector<std::wstring> changes;
  if (!(current.service == desired.service)) changes.emplace_back(L"service");
  if (!(current.application == desired.application)) changes.emplace_back(L"application");
  if (!(current.recovery == desired.recovery)) changes.emplace_back(L"recovery");
  if (!(current.readiness == desired.readiness)) changes.emplace_back(L"readiness");
  if (!(current.health == desired.health)) changes.emplace_back(L"health");
  if (!(current.resources == desired.resources)) changes.emplace_back(L"resources");
  if (!(current.logging == desired.logging)) changes.emplace_back(L"logging");
  if (!(current.security == desired.security)) changes.emplace_back(L"security");
  if (! strings_equal_ignore_case(current_path.c_str(),
                                  desired_path.c_str())) {
    changes.emplace_back(L"configPath");
  }
  return changes;
}

bool atomic_copy_file(const std::wstring &source,
                      const std::wstring &destination,
                      unsigned long *error) {
  std::wstring temporary = destination + L".tmp." +
                           std::to_wstring(GetCurrentProcessId());
  DeleteFileW(temporary.c_str());
  if (! CopyFileW(source.c_str(), temporary.c_str(), TRUE)) {
    if (error) *error = GetLastError();
    return false;
  }
  unique_handle file(CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
  if (! file) {
    if (error) *error = GetLastError();
    DeleteFileW(temporary.c_str());
    return false;
  }
  bool flushed = FlushFileBuffers(file.get()) != FALSE;
  unsigned long flush_error = flushed ? ERROR_SUCCESS : GetLastError();
  file.reset();
  if (! flushed ||
      ! MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    if (error) *error = flushed ? GetLastError() : flush_error;
    DeleteFileW(temporary.c_str());
    return false;
  }
  if (error) *error = ERROR_SUCCESS;
  return true;
}

config_operation_result_t operation_failure(
    config_operation_status_t status, std::wstring message,
    unsigned long windows_error = ERROR_SUCCESS) {
  config_operation_result_t result;
  result.status = status;
  result.windows_error = windows_error;
  result.message = std::move(message);
  return result;
}

config_operation_result_t validation_failure(
    const config_validation_t &validation,
    config_operation_status_t status =
        config_operation_status_t::validation) {
  config_operation_result_t result;
  result.status = status;
  result.validation_errors = validation.errors;
  return result;
}

config_operation_status_t registration_error_status(unsigned long error) {
  switch (error) {
    case ERROR_ACCESS_DENIED:
      return config_operation_status_t::permission;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return config_operation_status_t::not_found;
    case ERROR_INVALID_DATA:
      return config_operation_status_t::conflict;
    default:
      return config_operation_status_t::system;
  }
}

config_operation_status_t managed_state_error_status(unsigned long error) {
  const config_operation_status_t status = registration_error_status(error);
  return status == config_operation_status_t::not_found
             ? config_operation_status_t::conflict
             : status;
}

}  // namespace

config_operation_result_t export_service_config(
    const std::wstring &service_name, const std::wstring &destination_path) {
  std::wstring source;
  unsigned long error = ERROR_SUCCESS;
  if (! read_config_path(service_name, &source, &error)) {
    return operation_failure(
        registration_error_status(error),
        L"Unable to read the registered configuration path", error);
  }
  service_config_t config;
  config_validation_t validation;
  if (! load_secure_service_config(source.c_str(), &config, &validation) ||
      ! strings_equal_ignore_case(config.service.name.c_str(),
                                  service_name.c_str())) {
    return operation_failure(
        config_operation_status_t::conflict,
        L"The registered configuration is invalid or belongs to another service");
  }
  std::wstring destination;
  if (! full_config_path(destination_path.c_str(), &destination, &validation)) {
    return validation_failure(validation);
  }
  if (! atomic_copy_file(source, destination, &error)) {
    return operation_failure(config_operation_status_t::system,
                             L"Unable to export the configuration atomically",
                             error);
  }
  config_operation_result_t result;
  result.service = service_name;
  result.action = L"export";
  result.detail = L"Exported configuration to " + destination;
  return result;
}

config_operation_result_t plan_service_config(
    const std::wstring &config_path, const std::wstring &wrapper_image_path) {
  service_config_t desired;
  config_validation_t validation;
  if (! load_secure_service_config(config_path.c_str(), &desired,
                                   &validation)) {
    return validation_failure(validation);
  }
  std::wstring desired_path;
  std::wstring account;
  if (! full_config_path(config_path.c_str(), &desired_path, &validation) ||
      ! account_name(desired, &account, &validation)) {
    return validation_failure(validation);
  }
  if (! audit_service_paths(desired_path, desired, &validation)) {
    return validation_failure(validation, config_operation_status_t::security);
  }

  unique_service_handle manager(OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE,
                                                SC_MANAGER_CONNECT));
  if (! manager) {
    unsigned long error = GetLastError();
    return operation_failure(
        error == ERROR_ACCESS_DENIED ? config_operation_status_t::permission
                                     : config_operation_status_t::system,
        L"Unable to open the Service Control Manager", error);
  }
  unique_service_handle service(OpenServiceW(manager.get(), desired.service.name.c_str(),
                                              SERVICE_QUERY_CONFIG));
  const unsigned long open_error = service ? ERROR_SUCCESS : GetLastError();
  if (! service && open_error == ERROR_SERVICE_DOES_NOT_EXIST) {
    config_operation_result_t result;
    result.service = desired.service.name;
    result.action = L"create";
    result.changes = {L"service", L"application", L"recovery", L"readiness",
                      L"health", L"resources", L"logging", L"security"};
    return result;
  }
  if (! service) {
    return operation_failure(
        open_error == ERROR_ACCESS_DENIED ? config_operation_status_t::permission
                                          : config_operation_status_t::system,
        L"Unable to query the service", open_error);
  }

  existing_service_state_t state;
  unsigned long query_error = ERROR_SUCCESS;
  if (! read_existing_service_state(service.get(), desired.service.name, &state,
                                    &query_error)) {
    const config_operation_status_t status =
        managed_state_error_status(query_error);
    return operation_failure(
        status,
        status == config_operation_status_t::conflict
            ? L"The existing service is not managed by SvcPilot"
            : L"Unable to read the existing service configuration",
        query_error);
  }

  service_config_t current;
  validation.errors.clear();
  if (! load_secure_service_config(state.config_path.c_str(), &current,
                                   &validation)) {
    return operation_failure(
        config_operation_status_t::conflict,
        L"The registered service configuration cannot be loaded");
  }
  if (! strings_equal_ignore_case(current.service.name.c_str(),
                                  desired.service.name.c_str())) {
    return operation_failure(
        config_operation_status_t::conflict,
        L"The registered configuration belongs to a different service");
  }
  std::vector<std::wstring> changes =
      config_changes(current, desired, state.config_path, desired_path);
  const std::wstring expected_account = account.empty() ? L"LocalSystem" : account;
  const bool scm_matches =
      state.service_type == SERVICE_WIN32_OWN_PROCESS &&
      strings_equal_ignore_case(state.binary_path.c_str(),
                                wrapper_image_path.c_str()) &&
      state.dependencies == make_dependencies(desired.service.dependencies) &&
      strings_equal_ignore_case(state.account.c_str(),
                                expected_account.c_str()) &&
      state.display_name == desired.service.display_name &&
      state.description == desired.service.description &&
      state.service_sid_type == service_sid_type(desired.security.service_sid);
  if (! scm_matches) changes.emplace_back(L"scm");
  config_operation_result_t result;
  result.service = desired.service.name;
  result.action = changes.empty() ? L"noChange" : L"update";
  result.changes = std::move(changes);
  return result;
}

config_operation_result_t apply_service_config(
    const std::wstring &config_path, const std::wstring &wrapper_image_path) {
  service_config_t config;
  config_validation_t validation;
  if (! load_secure_service_config(config_path.c_str(), &config,
                                   &validation)) {
    return validation_failure(validation);
  }

  std::wstring path;
  std::wstring account;
  if (! full_config_path(config_path.c_str(), &path, &validation) ||
      ! account_name(config, &account, &validation)) {
    return validation_failure(validation);
  }
  if (! audit_service_paths(path, config, &validation)) {
    return validation_failure(validation, config_operation_status_t::security);
  }

  std::vector<wchar_t> dependencies =
      make_dependencies(config.service.dependencies);
  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE,
      SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (! manager) {
    unsigned long error = GetLastError();
    return operation_failure(
        error == ERROR_ACCESS_DENIED ? config_operation_status_t::permission
                                     : config_operation_status_t::system,
        L"Unable to open the Service Control Manager", error);
  }

  bool created = false;
  existing_service_state_t previous;
  unique_service_handle service(OpenServiceW(
      manager.get(), config.service.name.c_str(),
      SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | DELETE));
  unsigned long open_error = service ? ERROR_SUCCESS : GetLastError();
  if (! service && open_error == ERROR_SERVICE_DOES_NOT_EXIST) {
    service.reset(CreateServiceW(
        manager.get(), config.service.name.c_str(), config.service.display_name.c_str(),
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS, service_start_type(config.service.startup),
        SERVICE_ERROR_NORMAL, wrapper_image_path.c_str(), nullptr, nullptr,
        dependencies.data(), account.empty() ? nullptr : account.c_str(), nullptr));
    open_error = service ? ERROR_SUCCESS : GetLastError();
    created = static_cast<bool>(service);
  }
  else if (service) {
    unsigned long query_error = ERROR_SUCCESS;
    if (! read_existing_service_state(service.get(), config.service.name, &previous,
                                      &query_error)) {
      const config_operation_status_t status =
          managed_state_error_status(query_error);
      return operation_failure(
          status,
          status == config_operation_status_t::conflict
              ? L"Refusing to update a service not previously managed by SvcPilot"
              : L"Unable to read the existing service configuration",
          query_error);
    }
    if (! passwordless_service_account(previous.account)) {
      return operation_failure(
          config_operation_status_t::conflict,
          L"The existing service account cannot be restored without a password; update refused");
    }
    if (! ChangeServiceConfigW(
            service.get(), SERVICE_WIN32_OWN_PROCESS,
            SERVICE_NO_CHANGE, SERVICE_ERROR_NORMAL,
            wrapper_image_path.c_str(), nullptr, nullptr, dependencies.data(),
            account.empty() ? L"LocalSystem" : account.c_str(), nullptr,
            config.service.display_name.c_str())) {
      unsigned long error = GetLastError();
      return operation_failure(config_operation_status_t::system,
                               L"ChangeServiceConfig failed", error);
    }
  }

  if (! service) {
    return operation_failure(
        open_error == ERROR_ACCESS_DENIED ? config_operation_status_t::permission
                                          : config_operation_status_t::system,
        L"CreateService/OpenService failed", open_error);
  }

  auto rollback = [&]() {
    if (created) return DeleteService(service.get()) != FALSE;
    return restore_existing_service(service.get(), config.service.name, previous);
  };

  SERVICE_DESCRIPTIONW description = {
      const_cast<wchar_t *>(config.service.description.c_str())};
  if (! ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description)) {
    unsigned long error = GetLastError();
    bool restored = rollback();
    return operation_failure(
        config_operation_status_t::system,
        restored ? L"Setting service description failed"
                 : L"Setting service description failed and rollback failed",
        error);
  }

  SERVICE_DELAYED_AUTO_START_INFO delayed = {FALSE};
  if (created && ! ChangeServiceConfig2W(
                     service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                     &delayed)) {
    unsigned long error = GetLastError();
    bool restored = rollback();
    return operation_failure(
        config_operation_status_t::system,
        restored ? L"Setting delayed start failed"
                 : L"Setting delayed start failed and rollback failed",
        error);
  }

  unsigned long sid_error = ERROR_SUCCESS;
  if (! set_service_sid_type(service.get(), service_sid_type(config.security.service_sid),
                             &sid_error)) {
    bool restored = rollback();
    return operation_failure(
        config_operation_status_t::system,
        restored ? L"Setting service SID type failed"
                 : L"Setting service SID type failed and rollback failed",
        sid_error);
  }

  unsigned long registry_error = ERROR_SUCCESS;
  if (! write_config_path(config.service.name, path, &registry_error)) {
    bool restored = rollback();
    return operation_failure(
        config_operation_status_t::system,
        restored ? L"Storing configuration path failed"
                 : L"Storing configuration path failed and rollback failed",
        registry_error);
  }

  config_operation_result_t result;
  result.service = config.service.name;
  result.action = created ? L"create" : L"update";
  result.detail =
      std::wstring(created ? L"Installed service " : L"Updated service ") +
      config.service.name + L" from " + path;
  return result;
}
