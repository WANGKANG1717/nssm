#include "nssm.h"
#include "child_process.h"
#include "config.h"

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
  HKEY key = nullptr;
  unsigned long disposition = 0;
  long result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, nullptr,
                                0, KEY_SET_VALUE, nullptr, &key, &disposition);
  if (result != ERROR_SUCCESS) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }
  result = RegSetValueExW(
      key, NSSM_REG_CONFIG_PATH, 0, REG_SZ,
      reinterpret_cast<const unsigned char *>(path.c_str()),
      static_cast<unsigned long>((path.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  if (error) *error = static_cast<unsigned long>(result);
  return result == ERROR_SUCCESS;
}

bool read_config_path(const std::wstring &service_name, std::wstring *path,
                      unsigned long *error) {
  std::wstring key_path = L"SYSTEM\\CurrentControlSet\\Services\\" +
                          service_name + L"\\Parameters";
  HKEY key = nullptr;
  long result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0,
                              KEY_QUERY_VALUE, &key);
  if (result != ERROR_SUCCESS) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }

  unsigned long type = 0;
  unsigned long bytes = 0;
  result = RegQueryValueExW(key, NSSM_REG_CONFIG_PATH, nullptr, &type, nullptr,
                            &bytes);
  if (result != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t) ||
      bytes > PATH_LENGTH * sizeof(wchar_t)) {
    RegCloseKey(key);
    if (error) {
      *error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                       : static_cast<unsigned long>(result);
    }
    return false;
  }

  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  result = RegQueryValueExW(key, NSSM_REG_CONFIG_PATH, nullptr, &type,
                            reinterpret_cast<unsigned char *>(buffer.data()),
                            &bytes);
  RegCloseKey(key);
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

}  // namespace

int apply_service_config_command(int argc, wchar_t **argv) {
  if (argc != 1 || ! argv || ! argv[0]) {
    fwprintf(stderr, L"Usage: nssm apply <config.json>\n");
    return 2;
  }

  service_config_t config;
  config_validation_t validation;
  if (! load_service_config(argv[0], &config, &validation)) {
    for (const auto &error : validation.errors) {
      fwprintf(stderr, L"error: %ls\n", error.c_str());
    }
    return 1;
  }

  std::wstring path;
  std::wstring account;
  if (! full_config_path(argv[0], &path, &validation) ||
      ! account_name(config, &account, &validation)) {
    for (const auto &error : validation.errors) {
      fwprintf(stderr, L"error: %ls\n", error.c_str());
    }
    return 1;
  }

  std::vector<wchar_t> dependencies =
      make_dependencies(config.service.dependencies);
  SC_HANDLE manager = OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE,
                                     SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
  if (! manager) {
    fwprintf(stderr, L"error: OpenSCManager failed: %lu\n", GetLastError());
    return 3;
  }

  bool created = false;
  existing_service_state_t previous;
  SC_HANDLE service = OpenServiceW(manager, config.service.name.c_str(),
                                   SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG |
                                       DELETE);
  if (! service && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
    service = CreateServiceW(
        manager, config.service.name.c_str(), config.service.display_name.c_str(),
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS, service_start_type(config.service.startup),
        SERVICE_ERROR_NORMAL, nssm_imagepath(), nullptr, nullptr,
        dependencies.data(), account.empty() ? nullptr : account.c_str(), nullptr);
    created = service != nullptr;
  }
  else if (service) {
    unsigned long query_error = ERROR_SUCCESS;
    if (! read_existing_service_state(service, config.service.name, &previous,
                                      &query_error)) {
      fwprintf(stderr,
               L"error: refusing to update a service not previously managed "
               L"by this declarative NSSM (Windows error %lu)\n",
               query_error);
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return 4;
    }
    if (! passwordless_service_account(previous.account)) {
      fwprintf(stderr,
               L"error: the existing service account cannot be restored "
               L"without a password; update refused\n");
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return 4;
    }
    if (! ChangeServiceConfigW(
            service, SERVICE_WIN32_OWN_PROCESS,
            service_start_type(config.service.startup), SERVICE_ERROR_NORMAL,
            nssm_imagepath(), nullptr, nullptr, dependencies.data(),
            account.empty() ? L"LocalSystem" : account.c_str(), nullptr,
            config.service.display_name.c_str())) {
      fwprintf(stderr, L"error: ChangeServiceConfig failed: %lu\n",
               GetLastError());
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return 4;
    }
  }

  if (! service) {
    fwprintf(stderr, L"error: CreateService/OpenService failed: %lu\n",
             GetLastError());
    CloseServiceHandle(manager);
    return 4;
  }

  SERVICE_DESCRIPTIONW description = {
      const_cast<wchar_t *>(config.service.description.c_str())};
  if (! ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description)) {
    fwprintf(stderr, L"error: setting service description failed: %lu\n",
             GetLastError());
    if (created) DeleteService(service);
    else if (! restore_existing_service(service, config.service.name, previous)) {
      fwprintf(stderr, L"error: rollback of the existing service failed\n");
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 5;
  }

  SERVICE_DELAYED_AUTO_START_INFO delayed = {
      config.service.startup == service_startup_t::delayed};
  if (! ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                              &delayed)) {
    fwprintf(stderr, L"error: setting delayed start failed: %lu\n",
             GetLastError());
    if (created) DeleteService(service);
    else if (! restore_existing_service(service, config.service.name, previous)) {
      fwprintf(stderr, L"error: rollback of the existing service failed\n");
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 5;
  }

  unsigned long registry_error = ERROR_SUCCESS;
  if (! write_config_path(config.service.name, path, &registry_error)) {
    fwprintf(stderr, L"error: storing configuration path failed: %lu\n",
             registry_error);
    if (created) DeleteService(service);
    else if (! restore_existing_service(service, config.service.name, previous)) {
      fwprintf(stderr, L"error: rollback of the existing service failed\n");
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 6;
  }

  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  fwprintf(stdout, created ? L"Installed service %ls from %ls\n"
                           : L"Updated service %ls from %ls\n",
           config.service.name.c_str(), path.c_str());
  return 0;
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
      copy_config_environment(config->application, service, validation);
  if (! copied) {
    delete config;
    return false;
  }

  service->config = config;
  return true;
}
