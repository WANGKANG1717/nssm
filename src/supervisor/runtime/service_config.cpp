#include "service_config.h"

#include "child_process.h"
#include "config.h"
#include "config_registration.h"
#include "config_security.h"
#include "recovery.h"
#include "service.h"
#include "string_utils.h"

#include <new>
#include <string>
#include <vector>

namespace {

static_assert(SERVICE_NAME_LENGTH == kMaximumServiceNameCharacters + 1);
static_assert(EXE_LENGTH == kMaximumPathCharacters + 1);
static_assert(DIR_LENGTH == kMaximumWorkingDirectoryCharacters + 1);
static_assert(VALUE_LENGTH == kMaximumSerializedArgumentCharacters + 1);

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
  wchar_t *copy =
      static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, bytes));
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

bool load_registered_service_config(nssm_service_t *service,
                                    config_validation_t *validation) {
  if (! service || ! validation) return false;

  unsigned long error = ERROR_SUCCESS;
  std::wstring path;
  if (! read_config_path(service->name, &path, &error)) {
    validation->add(
        L"Unable to read ConfigPath from the service registry key (Windows error " +
        std::to_wstring(error) + L")");
    return false;
  }

  auto *config = new (std::nothrow) service_config_t();
  if (! config) {
    validation->add(L"Out of memory while loading service configuration");
    return false;
  }
  if (! load_secure_service_config(path.c_str(), config, validation)) {
    delete config;
    return false;
  }
  if (! strings_equal_ignore_case(config->service.name.c_str(),
                                  service->name)) {
    validation->add(
        L"The configuration service name does not match the SCM service name");
    delete config;
    return false;
  }
  if (! audit_service_paths(path, *config, validation)) {
    delete config;
    return false;
  }

  std::wstring arguments =
      join_windows_arguments(config->application.arguments);
  bool copied =
      copy_checked(config->service.display_name, service->displayname,
                   _countof(service->displayname), L"Unit.DisplayName",
                   validation) &&
      copy_checked(config->application.path, service->exe,
                   _countof(service->exe), L"Service.ExecStart", validation) &&
      copy_checked(config->application.working_directory, service->dir,
                   _countof(service->dir), L"Service.WorkingDirectory",
                   validation) &&
      copy_checked(arguments, service->flags, _countof(service->flags),
                   L"Service.Arguments", validation) &&
      copy_checked(config->logging.stdout_path, service->stdout_path,
                   _countof(service->stdout_path), L"Logging.Stdout.Path",
                   validation) &&
      copy_checked(config->logging.stderr_path, service->stderr_path,
                   _countof(service->stderr_path), L"Logging.Stderr.Path",
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
