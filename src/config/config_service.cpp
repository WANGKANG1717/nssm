#include "nssm.h"
#include "child_process.h"
#include "config.h"
#include "output.h"
#include "recovery.h"
#include "security.h"
#include "win32_handle.h"

#include <new>
#include <string>
#include <vector>

namespace {

#define NSSM_REG_CONFIG_PATH _T("ConfigPath")

static_assert(SERVICE_NAME_LENGTH == kMaximumServiceNameCharacters + 1);
static_assert(EXE_LENGTH == kMaximumPathCharacters + 1);
static_assert(DIR_LENGTH == kMaximumWorkingDirectoryCharacters + 1);
static_assert(VALUE_LENGTH == kMaximumSerializedArgumentCharacters + 1);

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

bool full_config_path(const wchar_t *path, std::wstring *full,
                      config_validation_t *validation) {
  unsigned long needed = GetFullPathNameW(path, 0, nullptr, nullptr);
  if (! needed) {
    validation->add(L"Unable to resolve the configuration path");
    return false;
  }
  std::vector<wchar_t> buffer(needed);
  unsigned long written = GetFullPathNameW(path, needed, buffer.data(), nullptr);
  if (! written || written >= needed) {
    validation->add(L"Unable to resolve the configuration path");
    return false;
  }
  *full = buffer.data();
  return true;
}

bool write_config_path(const std::wstring &service_name,
                       const std::wstring &path, unsigned long *error) {
  std::wstring key_path = L"SYSTEM\\CurrentControlSet\\Services\\" +
                          service_name + L"\\Parameters";
  unique_registry_key key;
  unsigned long disposition = 0;
  long result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, nullptr,
                                0, KEY_SET_VALUE, nullptr, key.put(), &disposition);
  if (result != ERROR_SUCCESS) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }
  result = RegSetValueExW(
      key.get(), NSSM_REG_CONFIG_PATH, 0, REG_SZ,
      reinterpret_cast<const unsigned char *>(path.c_str()),
      static_cast<unsigned long>((path.size() + 1) * sizeof(wchar_t)));
  if (error) *error = static_cast<unsigned long>(result);
  return result == ERROR_SUCCESS;
}

bool read_config_path(const std::wstring &service_name, std::wstring *path,
                      unsigned long *error) {
  std::wstring key_path = L"SYSTEM\\CurrentControlSet\\Services\\" +
                          service_name + L"\\Parameters";
  unique_registry_key key;
  long result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0,
                              KEY_QUERY_VALUE, key.put());
  if (result != ERROR_SUCCESS) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }

  unsigned long type = 0;
  unsigned long bytes = 0;
  result = RegQueryValueExW(key.get(), NSSM_REG_CONFIG_PATH, nullptr, &type, nullptr,
                            &bytes);
  if (result != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t) ||
      bytes > PATH_LENGTH * sizeof(wchar_t)) {
    if (error) {
      *error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                       : static_cast<unsigned long>(result);
    }
    return false;
  }

  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  result = RegQueryValueExW(key.get(), NSSM_REG_CONFIG_PATH, nullptr, &type,
                            reinterpret_cast<unsigned char *>(buffer.data()),
                            &bytes);
  if (result != ERROR_SUCCESS) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }
  buffer.back() = L'\0';
  *path = buffer.data();
  if (error) *error = ERROR_SUCCESS;
  return true;
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
  if (!_wcsicmp(account.c_str(), L"LocalSystem") ||
      !_wcsicmp(account.c_str(), L"NT AUTHORITY\\LocalService") ||
      !_wcsicmp(account.c_str(), L"NT AUTHORITY\\NetworkService")) {
    return true;
  }
  if (account.size() > 11 &&
      !_wcsnicmp(account.c_str(), L"NT SERVICE\\", 11)) {
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

bool copy_checked(const std::wstring &source, wchar_t *destination,
                  size_t capacity, const wchar_t *field,
                  config_validation_t *validation) {
  if (source.size() >= capacity) {
    validation->add(std::wstring(field) + L" is too long");
    return false;
  }
  wmemcpy(destination, source.c_str(), source.size() + 1);
  return true;
}

bool copy_config_environment(const application_config_t &application,
                             nssm_service_t *service,
                             config_validation_t *validation) {
  const wchar_t empty_environment[] = {L'\0', L'\0'};
  std::vector<wchar_t> block;
  std::wstring error;
  if (! extend_child_environment(empty_environment, application.environment,
                                 &block, &error)) {
    validation->add(L"Unable to build application environment: " + error);
    return false;
  }
  size_t bytes = block.size() * sizeof(wchar_t);
  wchar_t *copy = static_cast<wchar_t *>(
      HeapAlloc(GetProcessHeap(), 0, bytes));
  if (! copy) {
    validation->add(L"Out of memory while building application environment");
    return false;
  }
  memcpy(copy, block.data(), bytes);
  if (application.inherit_environment) {
    service->env_extra = copy;
    service->env_extralen = static_cast<unsigned long>(block.size());
  }
  else {
    service->env = copy;
    service->envlen = static_cast<unsigned long>(block.size());
  }
  return true;
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
  if (_wcsicmp(current_path.c_str(), desired_path.c_str())) {
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
  if (! flushed || ! MoveFileExW(temporary.c_str(), destination.c_str(),
                                  MOVEFILE_REPLACE_EXISTING |
                                      MOVEFILE_WRITE_THROUGH)) {
    if (error) *error = flushed ? GetLastError() : flush_error;
    DeleteFileW(temporary.c_str());
    return false;
  }
  if (error) *error = ERROR_SUCCESS;
  return true;
}

}  // namespace

bool registered_service_config_path(const wchar_t *service_name,
                                    std::wstring *path,
                                    unsigned long *error) {
  if (! service_name || ! service_name[0] || ! path) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  return read_config_path(service_name, path, error);
}

int export_service_config_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 2 ||
      ! arguments.positional[0] || ! arguments.positional[1]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: nssm export [--json] <service> <destination.json>");
    return static_cast<int>(cli_exit_code::usage);
  }
  std::wstring service_name = arguments.positional[0];
  std::wstring source;
  unsigned long error = ERROR_SUCCESS;
  if (! read_config_path(service_name, &source, &error)) {
    print_cli_error(arguments.json,
                    error == ERROR_ACCESS_DENIED ? L"permission" : L"notFound",
                    L"Unable to read the registered configuration path", error);
    return static_cast<int>(error == ERROR_ACCESS_DENIED
                                ? cli_exit_code::permission
                                : cli_exit_code::conflict);
  }
  service_config_t config;
  config_validation_t validation;
  if (! load_service_config(source.c_str(), &config, &validation) ||
      _wcsicmp(config.service.name.c_str(), service_name.c_str())) {
    print_cli_error(arguments.json, L"conflict",
                    L"The registered configuration is invalid or belongs to another service");
    return static_cast<int>(cli_exit_code::conflict);
  }
  std::wstring destination;
  if (! full_config_path(arguments.positional[1], &destination, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors);
    return static_cast<int>(cli_exit_code::validation);
  }
  if (! atomic_copy_file(source, destination, &error)) {
    print_cli_error(arguments.json, L"system",
                    L"Unable to export the configuration atomically", error);
    return static_cast<int>(cli_exit_code::system);
  }
  print_cli_success(arguments.json, L"export", service_name,
                    L"Exported configuration to " + destination);
  return static_cast<int>(cli_exit_code::success);
}

int plan_service_config_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 1 ||
      ! arguments.positional[0]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: nssm plan [--json] <config.json>");
    return static_cast<int>(cli_exit_code::usage);
  }

  service_config_t desired;
  config_validation_t validation;
  if (! load_service_config(arguments.positional[0], &desired, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors);
    return static_cast<int>(cli_exit_code::validation);
  }
  std::wstring desired_path;
  std::wstring account;
  if (! full_config_path(arguments.positional[0], &desired_path, &validation) ||
      ! account_name(desired, &account, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors);
    return static_cast<int>(cli_exit_code::validation);
  }
  if (! audit_service_paths(desired_path, desired, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors, L"security");
    return static_cast<int>(cli_exit_code::security);
  }

  unique_service_handle manager(OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE,
                                                SC_MANAGER_CONNECT));
  if (! manager) {
    unsigned long error = GetLastError();
    print_cli_error(arguments.json,
                    error == ERROR_ACCESS_DENIED ? L"permission" : L"system",
                    L"Unable to open the Service Control Manager", error);
    return static_cast<int>(error == ERROR_ACCESS_DENIED
                                ? cli_exit_code::permission
                                : cli_exit_code::system);
  }
  unique_service_handle service(OpenServiceW(manager.get(), desired.service.name.c_str(),
                                              SERVICE_QUERY_CONFIG));
  if (! service && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
    print_cli_plan(arguments.json, desired.service.name, L"create",
                   {L"service", L"application", L"recovery", L"readiness",
                    L"health", L"resources", L"logging", L"security"});
    return static_cast<int>(cli_exit_code::success);
  }
  if (! service) {
    unsigned long error = GetLastError();
    print_cli_error(arguments.json,
                    error == ERROR_ACCESS_DENIED ? L"permission" : L"system",
                    L"Unable to query the service", error);
    return static_cast<int>(error == ERROR_ACCESS_DENIED
                                ? cli_exit_code::permission
                                : cli_exit_code::system);
  }

  existing_service_state_t state;
  unsigned long query_error = ERROR_SUCCESS;
  if (! read_existing_service_state(service.get(), desired.service.name, &state,
                                    &query_error)) {
    print_cli_error(arguments.json, L"conflict",
                    L"The existing service is not managed by declarative NSSM",
                    query_error);
    return static_cast<int>(cli_exit_code::conflict);
  }

  service_config_t current;
  validation.errors.clear();
  if (! load_service_config(state.config_path.c_str(), &current, &validation)) {
    print_cli_error(arguments.json, L"conflict",
                    L"The registered service configuration cannot be loaded");
    return static_cast<int>(cli_exit_code::conflict);
  }
  if (_wcsicmp(current.service.name.c_str(), desired.service.name.c_str())) {
    print_cli_error(arguments.json, L"conflict",
                    L"The registered configuration belongs to a different service");
    return static_cast<int>(cli_exit_code::conflict);
  }
  std::vector<std::wstring> changes =
      config_changes(current, desired, state.config_path, desired_path);
  const std::wstring expected_account = account.empty() ? L"LocalSystem" : account;
  const bool scm_matches =
      state.service_type == SERVICE_WIN32_OWN_PROCESS &&
      state.start_type == service_start_type(desired.service.startup) &&
      state.delayed == (desired.service.startup == service_startup_t::delayed) &&
      !_wcsicmp(state.binary_path.c_str(), nssm_imagepath()) &&
      state.dependencies == make_dependencies(desired.service.dependencies) &&
      !_wcsicmp(state.account.c_str(), expected_account.c_str()) &&
      state.display_name == desired.service.display_name &&
      state.description == desired.service.description &&
      state.service_sid_type == service_sid_type(desired.security.service_sid);
  if (! scm_matches) changes.emplace_back(L"scm");
  print_cli_plan(arguments.json, desired.service.name,
                 changes.empty() ? L"noChange" : L"update", changes);
  return static_cast<int>(cli_exit_code::success);
}

int apply_service_config_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 1 ||
      ! arguments.positional[0]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: nssm apply [--json] <config.json>");
    return static_cast<int>(cli_exit_code::usage);
  }

  service_config_t config;
  config_validation_t validation;
  if (! load_service_config(arguments.positional[0], &config, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors);
    return static_cast<int>(cli_exit_code::validation);
  }

  std::wstring path;
  std::wstring account;
  if (! full_config_path(arguments.positional[0], &path, &validation) ||
      ! account_name(config, &account, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors);
    return static_cast<int>(cli_exit_code::validation);
  }
  if (! audit_service_paths(path, config, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors, L"security");
    return static_cast<int>(cli_exit_code::security);
  }

  std::vector<wchar_t> dependencies =
      make_dependencies(config.service.dependencies);
  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE,
      SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (! manager) {
    unsigned long error = GetLastError();
    print_cli_error(arguments.json,
                    error == ERROR_ACCESS_DENIED ? L"permission" : L"system",
                    L"Unable to open the Service Control Manager", error);
    return static_cast<int>(error == ERROR_ACCESS_DENIED
                                ? cli_exit_code::permission
                                : cli_exit_code::system);
  }

  bool created = false;
  existing_service_state_t previous;
  unique_service_handle service(OpenServiceW(
      manager.get(), config.service.name.c_str(),
      SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | DELETE));
  if (! service && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
    service.reset(CreateServiceW(
        manager.get(), config.service.name.c_str(), config.service.display_name.c_str(),
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS, service_start_type(config.service.startup),
        SERVICE_ERROR_NORMAL, nssm_imagepath(), nullptr, nullptr,
        dependencies.data(), account.empty() ? nullptr : account.c_str(), nullptr));
    created = static_cast<bool>(service);
  }
  else if (service) {
    unsigned long query_error = ERROR_SUCCESS;
    if (! read_existing_service_state(service.get(), config.service.name, &previous,
                                      &query_error)) {
      print_cli_error(arguments.json, L"conflict",
                      L"Refusing to update a service not previously managed by this declarative NSSM",
                      query_error);
      return static_cast<int>(cli_exit_code::conflict);
    }
    if (! passwordless_service_account(previous.account)) {
      print_cli_error(arguments.json, L"conflict",
                      L"The existing service account cannot be restored without a password; update refused");
      return static_cast<int>(cli_exit_code::conflict);
    }
    if (! ChangeServiceConfigW(
            service.get(), SERVICE_WIN32_OWN_PROCESS,
            service_start_type(config.service.startup), SERVICE_ERROR_NORMAL,
            nssm_imagepath(), nullptr, nullptr, dependencies.data(),
            account.empty() ? L"LocalSystem" : account.c_str(), nullptr,
            config.service.display_name.c_str())) {
      unsigned long error = GetLastError();
      print_cli_error(arguments.json, L"system",
                      L"ChangeServiceConfig failed", error);
      return static_cast<int>(cli_exit_code::system);
    }
  }

  if (! service) {
    unsigned long error = GetLastError();
    print_cli_error(arguments.json,
                    error == ERROR_ACCESS_DENIED ? L"permission" : L"system",
                    L"CreateService/OpenService failed", error);
    return static_cast<int>(error == ERROR_ACCESS_DENIED
                                ? cli_exit_code::permission
                                : cli_exit_code::system);
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
    print_cli_error(arguments.json, L"system",
                    restored ? L"Setting service description failed"
                             : L"Setting service description failed and rollback failed",
                    error);
    return static_cast<int>(cli_exit_code::system);
  }

  SERVICE_DELAYED_AUTO_START_INFO delayed = {
      config.service.startup == service_startup_t::delayed};
  if (! ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                              &delayed)) {
    unsigned long error = GetLastError();
    bool restored = rollback();
    print_cli_error(arguments.json, L"system",
                    restored ? L"Setting delayed start failed"
                             : L"Setting delayed start failed and rollback failed",
                    error);
    return static_cast<int>(cli_exit_code::system);
  }

  unsigned long sid_error = ERROR_SUCCESS;
  if (! set_service_sid_type(service.get(), service_sid_type(config.security.service_sid),
                             &sid_error)) {
    bool restored = rollback();
    print_cli_error(arguments.json, L"system",
                    restored ? L"Setting service SID type failed"
                             : L"Setting service SID type failed and rollback failed",
                    sid_error);
    return static_cast<int>(cli_exit_code::system);
  }

  unsigned long registry_error = ERROR_SUCCESS;
  if (! write_config_path(config.service.name, path, &registry_error)) {
    bool restored = rollback();
    print_cli_error(arguments.json, L"system",
                    restored ? L"Storing configuration path failed"
                             : L"Storing configuration path failed and rollback failed",
                    registry_error);
    return static_cast<int>(cli_exit_code::system);
  }

  std::wstring detail =
      std::wstring(created ? L"Installed service " : L"Updated service ") +
      config.service.name + L" from " + path;
  print_cli_success(arguments.json, created ? L"create" : L"update",
                    config.service.name, detail);
  return static_cast<int>(cli_exit_code::success);
}

bool load_registered_service_config(nssm_service_t *service,
                                    config_validation_t *validation) {
  if (! service || ! validation) return false;

  unsigned long error = ERROR_SUCCESS;
  std::wstring path;
  if (! read_config_path(service->name, &path, &error)) {
    validation->add(L"Unable to read ConfigPath from the service registry key (Windows error " +
                    std::to_wstring(error) + L")");
    return false;
  }

  auto *config = new (std::nothrow) service_config_t();
  if (! config) {
    validation->add(L"Out of memory while loading service configuration");
    return false;
  }
  if (! load_service_config(path.c_str(), config, validation)) {
    delete config;
    return false;
  }
  if (_wcsicmp(config->service.name.c_str(), service->name)) {
    validation->add(L"The configuration service name does not match the SCM service name");
    delete config;
    return false;
  }
  if (! audit_service_paths(path, *config, validation)) {
    delete config;
    return false;
  }

  std::wstring arguments = join_windows_arguments(config->application.arguments);
  bool copied =
      copy_checked(path, service->config_path, _countof(service->config_path),
                   L"configuration path", validation) &&
      copy_checked(config->service.display_name, service->displayname,
                   _countof(service->displayname), L"service.displayName",
                   validation) &&
      copy_checked(config->application.path, service->exe,
                   _countof(service->exe), L"application.path", validation) &&
      copy_checked(config->application.working_directory, service->dir,
                   _countof(service->dir), L"application.workingDirectory",
                   validation) &&
      copy_checked(arguments, service->flags, _countof(service->flags),
                   L"application.arguments", validation) &&
      copy_checked(config->logging.stdout_path, service->stdout_path,
                   _countof(service->stdout_path), L"logging.stdout.path",
                   validation) &&
      copy_checked(config->logging.stderr_path, service->stderr_path,
                   _countof(service->stderr_path), L"logging.stderr.path",
                   validation) &&
      copy_config_environment(config->application, service, validation);
  if (! copied) {
    delete config;
    return false;
  }

  service->config = config;
  service->timestamp_log = config->logging.timestamp;
  ULARGE_INTEGER maximum_log_size = {};
  maximum_log_size.QuadPart = config->logging.maximum_bytes;
  service->rotate_bytes_low = maximum_log_size.LowPart;
  service->rotate_bytes_high = maximum_log_size.HighPart;
  service->rotate_files = config->logging.maximum_bytes != 0;
  service->rotate_stdout_online =
      ! config->logging.stdout_path.empty() && service->rotate_files
          ? NSSM_ROTATE_ONLINE
          : NSSM_ROTATE_OFFLINE;
  service->rotate_stderr_online =
      ! config->logging.stderr_path.empty() && service->rotate_files
          ? NSSM_ROTATE_ONLINE
          : NSSM_ROTATE_OFFLINE;
  service->use_stdout_pipe = ! config->logging.stdout_path.empty() &&
      (service->rotate_files || service->timestamp_log);
  service->use_stderr_pipe = ! config->logging.stderr_path.empty() &&
      (service->rotate_files || service->timestamp_log);
  service->recovery_state = new (std::nothrow) recovery_state_t();
  if (! service->recovery_state) {
    validation->add(L"Out of memory while creating recovery state");
    service->config = nullptr;
    if (service->env) {
      HeapFree(GetProcessHeap(), 0, service->env);
      service->env = nullptr;
    }
    if (service->env_extra) {
      HeapFree(GetProcessHeap(), 0, service->env_extra);
      service->env_extra = nullptr;
    }
    delete config;
    return false;
  }
  return true;
}
