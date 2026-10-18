#include "config.h"
#include "child_process.h"

#include <windows.h>
#include <shlwapi.h>

#include <cwctype>
#include <set>
#include <string>
#include <vector>

#include <yyjson.h>

namespace {

constexpr unsigned long long kMaximumConfigBytes = 4ULL * 1024ULL * 1024ULL;

struct case_insensitive_less {
  bool operator()(const std::wstring &left, const std::wstring &right) const {
    size_t common = left.size() < right.size() ? left.size() : right.size();
    for (size_t i = 0; i < common; ++i) {
      wchar_t a = static_cast<wchar_t>(std::towupper(left[i]));
      wchar_t b = static_cast<wchar_t>(std::towupper(right[i]));
      if (a != b) return a < b;
    }
    return left.size() < right.size();
  }
};

std::wstring utf8_to_wide(const char *value, size_t length, bool *valid) {
  if (valid) *valid = false;
  if (! value) return L"";
  if (! length) {
    if (valid) *valid = true;
    return L"";
  }

  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                                   static_cast<int>(length), nullptr, 0);
  if (needed <= 0) return L"";
  std::wstring result(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                          static_cast<int>(length), result.data(), needed) !=
      needed) {
    return L"";
  }
  if (valid) *valid = true;
  return result;
}

std::wstring utf8_to_wide(const char *value, bool *valid = nullptr) {
  return utf8_to_wide(value, value ? strlen(value) : 0, valid);
}

std::wstring field_path(const wchar_t *object, const char *field) {
  bool valid = false;
  std::wstring converted = utf8_to_wide(field, &valid);
  return std::wstring(object) + L"." + (valid ? converted : L"<invalid UTF-8>");
}

void reject_unknown_fields(yyjson_val *object, const wchar_t *object_name,
                           const std::set<std::string> &allowed,
                           config_validation_t *validation) {
  std::set<std::string> seen;
  yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
  yyjson_val *key = nullptr;
  while ((key = yyjson_obj_iter_next(&iterator))) {
    const char *name = yyjson_get_str(key);
    if (name && ! seen.insert(name).second) {
      validation->add(L"Duplicate configuration field: " +
                      field_path(object_name, name));
    }
    else if (name && ! allowed.contains(name)) {
      validation->add(L"Unknown configuration field: " +
                      field_path(object_name, name));
    }
  }
}

bool read_required_string(yyjson_val *object, const char *name,
                          const wchar_t *path, std::wstring *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! yyjson_is_str(value)) {
    validation->add(std::wstring(path) + L" must be a string");
    return false;
  }
  bool valid = false;
  *destination = utf8_to_wide(yyjson_get_str(value), yyjson_get_len(value), &valid);
  if (! valid) {
    validation->add(std::wstring(path) + L" is not valid UTF-8");
    return false;
  }
  if (destination->find(L'\0') != std::wstring::npos) {
    validation->add(std::wstring(path) + L" must not contain U+0000");
    return false;
  }
  return true;
}

bool read_optional_string(yyjson_val *object, const char *name,
                          const wchar_t *path, std::wstring *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! value) return true;
  return read_required_string(object, name, path, destination, validation);
}

void read_optional_number(yyjson_val *object, const char *name,
                          const wchar_t *path, unsigned long minimum,
                          unsigned long maximum, unsigned long *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! value) return;
  if (! yyjson_is_uint(value) || yyjson_get_uint(value) < minimum ||
      yyjson_get_uint(value) > maximum) {
    validation->add(std::wstring(path) + L" is outside the allowed range");
    return;
  }
  *destination = static_cast<unsigned long>(yyjson_get_uint(value));
}

bool read_required_number(yyjson_val *object, const char *name,
                          const wchar_t *path, unsigned long minimum,
                          unsigned long maximum, unsigned long *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! yyjson_is_uint(value) || yyjson_get_uint(value) < minimum ||
      yyjson_get_uint(value) > maximum) {
    validation->add(std::wstring(path) + L" is required and outside the allowed range");
    return false;
  }
  *destination = static_cast<unsigned long>(yyjson_get_uint(value));
  return true;
}

void read_optional_number64(yyjson_val *object, const char *name,
                            const wchar_t *path,
                            unsigned long long maximum,
                            unsigned long long *destination,
                            config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! value) return;
  if (! yyjson_is_uint(value) || yyjson_get_uint(value) > maximum) {
    validation->add(std::wstring(path) + L" is outside the allowed range");
    return;
  }
  *destination = yyjson_get_uint(value);
}

bool valid_service_name(const std::wstring &name) {
  if (name.empty() || name.size() > kMaximumServiceNameCharacters) return false;
  for (wchar_t character : name) {
    if (character == L'/' || character == L'\\' || character < 32) return false;
  }
  return true;
}

void parse_account(yyjson_val *service, service_account_config_t *account,
                   config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(service, "account");
  if (! object) return;
  if (! yyjson_is_obj(object)) {
    validation->add(L"service.account must be an object");
    return;
  }
  reject_unknown_fields(object, L"service.account", {"type", "name"},
                        validation);

  std::wstring type;
  if (! read_required_string(object, "type", L"service.account.type", &type,
                             validation)) {
    return;
  }
  if (type == L"system") account->type = service_account_type_t::system;
  else if (type == L"localService") account->type = service_account_type_t::local_service;
  else if (type == L"networkService") account->type = service_account_type_t::network_service;
  else if (type == L"virtual") account->type = service_account_type_t::virtual_account;
  else if (type == L"gmsa") account->type = service_account_type_t::gmsa;
  else if (type == L"user") account->type = service_account_type_t::user;
  else {
    validation->add(L"service.account.type has an unsupported value");
    return;
  }

  read_optional_string(object, "name", L"service.account.name", &account->name,
                       validation);
  bool needs_name = account->type == service_account_type_t::gmsa ||
                    account->type == service_account_type_t::user;
  if (needs_name && account->name.empty()) {
    validation->add(L"service.account.name is required for this account type");
  }
  if (! needs_name && ! account->name.empty()) {
    if (account->type != service_account_type_t::virtual_account) {
      validation->add(L"service.account.name is not allowed for this account type");
    }
  }
  if (account->type == service_account_type_t::gmsa &&
      ! account->name.empty() && ! account->name.ends_with(L'$')) {
    validation->add(L"service.account.name for a gMSA must end with '$'");
  }
}

void parse_dependencies(yyjson_val *service,
                        std::vector<std::wstring> *dependencies,
                        config_validation_t *validation) {
  yyjson_val *array = yyjson_obj_get(service, "dependencies");
  if (! array) return;
  if (! yyjson_is_arr(array)) {
    validation->add(L"service.dependencies must be an array of strings");
    return;
  }

  size_t index = 0;
  yyjson_val *value = nullptr;
  yyjson_arr_iter iterator = yyjson_arr_iter_with(array);
  while ((value = yyjson_arr_iter_next(&iterator))) {
    if (! yyjson_is_str(value)) {
      validation->add(L"service.dependencies[" + std::to_wstring(index) +
                      L"] must be a string");
    }
    else {
      bool valid = false;
      std::wstring dependency = utf8_to_wide(
          yyjson_get_str(value), yyjson_get_len(value), &valid);
      if (! valid || dependency.empty() ||
          dependency.find(L'\0') != std::wstring::npos) {
        validation->add(L"service.dependencies[" + std::to_wstring(index) +
                        L"] must be non-empty valid UTF-8");
      }
      else dependencies->push_back(std::move(dependency));
    }
    ++index;
  }
}

void parse_service(yyjson_val *root, service_metadata_config_t *service,
                   config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(root, "service");
  if (! yyjson_is_obj(object)) {
    validation->add(L"service must be an object");
    return;
  }
  reject_unknown_fields(object, L"service",
                        {"name", "displayName", "description", "startup",
                         "account", "dependencies"},
                        validation);

  read_required_string(object, "name", L"service.name", &service->name,
                       validation);
  if (! valid_service_name(service->name)) {
    validation->add(L"service.name is empty, too long, or contains an invalid character");
  }
  read_optional_string(object, "displayName", L"service.displayName",
                       &service->display_name, validation);
  read_optional_string(object, "description", L"service.description",
                       &service->description, validation);
  if (service->display_name.empty()) service->display_name = service->name;
  if (service->display_name.size() > kMaximumServiceNameCharacters) {
    validation->add(L"service.displayName must not exceed 256 characters");
  }

  std::wstring startup;
  if (read_optional_string(object, "startup", L"service.startup", &startup,
                           validation) && ! startup.empty()) {
    if (startup == L"automatic") service->startup = service_startup_t::automatic;
    else if (startup == L"delayed") service->startup = service_startup_t::delayed;
    else if (startup == L"manual") service->startup = service_startup_t::manual;
    else if (startup == L"disabled") service->startup = service_startup_t::disabled;
    else validation->add(L"service.startup has an unsupported value");
  }

  parse_account(object, &service->account, validation);
  if (service->account.type == service_account_type_t::virtual_account &&
      ! service->account.name.empty() &&
      _wcsicmp(service->account.name.c_str(), service->name.c_str())) {
    validation->add(
        L"service.account.name for a virtual account must match service.name");
  }
  parse_dependencies(object, &service->dependencies, validation);
}

void parse_string_array(yyjson_val *object, const char *name,
                        const wchar_t *path,
                        std::vector<std::wstring> *arguments,
                        config_validation_t *validation) {
  yyjson_val *array = yyjson_obj_get(object, name);
  if (! array) return;
  if (! yyjson_is_arr(array)) {
    validation->add(std::wstring(path) + L" must be an array of strings");
    return;
  }

  size_t index = 0;
  yyjson_val *value = nullptr;
  yyjson_arr_iter iterator = yyjson_arr_iter_with(array);
  while ((value = yyjson_arr_iter_next(&iterator))) {
    if (! yyjson_is_str(value)) {
      validation->add(L"application.arguments[" + std::to_wstring(index) +
                      L"] must be a string");
    }
    else {
      bool valid = false;
      std::wstring argument = utf8_to_wide(yyjson_get_str(value),
                                           yyjson_get_len(value), &valid);
      if (! valid || argument.find(L'\0') != std::wstring::npos) {
        validation->add(std::wstring(path) + L"[" + std::to_wstring(index) +
                        L"] must be valid UTF-8 without U+0000");
      }
      else arguments->push_back(std::move(argument));
    }
    ++index;
  }
}

void parse_arguments(yyjson_val *application,
                     std::vector<std::wstring> *arguments,
                     config_validation_t *validation) {
  parse_string_array(application, "arguments", L"application.arguments",
                     arguments, validation);
}

void parse_environment(yyjson_val *application,
                       application_config_t *config,
                       config_validation_t *validation) {
  yyjson_val *inherit = yyjson_obj_get(application, "inheritEnvironment");
  if (inherit) {
    if (! yyjson_is_bool(inherit)) {
      validation->add(L"application.inheritEnvironment must be a boolean");
    }
    else config->inherit_environment = yyjson_get_bool(inherit);
  }

  yyjson_val *environment = yyjson_obj_get(application, "environment");
  if (! environment) return;
  if (! yyjson_is_obj(environment)) {
    validation->add(L"application.environment must be an object of string values");
    return;
  }

  std::set<std::wstring, case_insensitive_less> names;
  yyjson_obj_iter iterator = yyjson_obj_iter_with(environment);
  yyjson_val *key = nullptr;
  while ((key = yyjson_obj_iter_next(&iterator))) {
    yyjson_val *value = yyjson_obj_iter_get_val(key);
    bool name_valid = false;
    std::wstring name = utf8_to_wide(yyjson_get_str(key), yyjson_get_len(key),
                                     &name_valid);
    if (! name_valid || name.empty() || name.find(L'=') != std::wstring::npos ||
        name.find(L'\0') != std::wstring::npos) {
      validation->add(L"application.environment contains an invalid variable name");
      continue;
    }
    if (! names.insert(name).second) {
      validation->add(L"application.environment contains duplicate variable names ignoring case: " + name);
      continue;
    }
    if (! yyjson_is_str(value)) {
      validation->add(L"application.environment." + name + L" must be a string");
      continue;
    }
    bool value_valid = false;
    std::wstring converted = utf8_to_wide(yyjson_get_str(value),
                                          yyjson_get_len(value), &value_valid);
    if (! value_valid || converted.find(L'\0') != std::wstring::npos) {
      validation->add(L"application.environment." + name +
                      L" must be valid UTF-8 without U+0000");
      continue;
    }
    config->environment.emplace_back(std::move(name), std::move(converted));
  }
}

void parse_application(yyjson_val *root, application_config_t *application,
                       config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(root, "application");
  if (! yyjson_is_obj(object)) {
    validation->add(L"application must be an object");
    return;
  }
  reject_unknown_fields(object, L"application",
                        {"path", "arguments", "workingDirectory",
                         "inheritEnvironment", "environment"},
                        validation);

  read_required_string(object, "path", L"application.path", &application->path,
                       validation);
  if (application->path.empty() || PathIsRelativeW(application->path.c_str())) {
    validation->add(L"application.path must be an absolute path");
  }
  if (application->path.size() > kMaximumPathCharacters) {
    validation->add(L"application.path is too long");
  }
  read_optional_string(object, "workingDirectory",
                       L"application.workingDirectory",
                       &application->working_directory, validation);
  if (! application->working_directory.empty() &&
      PathIsRelativeW(application->working_directory.c_str())) {
    validation->add(L"application.workingDirectory must be an absolute path");
  }
  if (application->working_directory.empty() && ! application->path.empty()) {
    size_t separator = application->path.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
      application->working_directory = application->path.substr(0, separator);
    }
  }

  parse_arguments(object, &application->arguments, validation);
  std::wstring serialized = join_windows_arguments(application->arguments);
  if (serialized.size() > kMaximumSerializedArgumentCharacters) {
    validation->add(L"application.arguments serialize to more than 16382 characters");
  }
  std::wstring command_line;
  std::wstring command_error;
  if (! application->path.empty() &&
      ! build_child_command_line(application->path, serialized, &command_line,
                                 &command_error)) {
    validation->add(L"application command line is invalid: " + command_error);
  }
  if (application->working_directory.size() >
      kMaximumWorkingDirectoryCharacters) {
    validation->add(L"application.workingDirectory is too long");
  }
  parse_environment(object, application, validation);
}

void parse_recovery(yyjson_val *root, recovery_config_t *recovery,
                    config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(root, "recovery");
  if (! object) return;
  if (! yyjson_is_obj(object)) {
    validation->add(L"recovery must be an object");
    return;
  }
  reject_unknown_fields(
      object, L"recovery",
      {"mode", "initialDelayMs", "maximumDelayMs", "multiplier",
       "jitterPercent", "windowMs", "maximumRestarts", "resetAfterMs"},
      validation);

  std::wstring mode;
  if (read_optional_string(object, "mode", L"recovery.mode", &mode,
                           validation) && ! mode.empty()) {
    if (mode == L"never") recovery->mode = recovery_mode_t::never;
    else if (mode == L"onFailure") recovery->mode = recovery_mode_t::on_failure;
    else if (mode == L"always") recovery->mode = recovery_mode_t::always;
    else validation->add(L"recovery.mode has an unsupported value");
  }
  constexpr unsigned long kDayMs = 24UL * 60UL * 60UL * 1000UL;
  constexpr unsigned long kWeekMs = 7UL * kDayMs;
  read_optional_number(object, "initialDelayMs", L"recovery.initialDelayMs",
                       0, kDayMs, &recovery->initial_delay_ms, validation);
  read_optional_number(object, "maximumDelayMs", L"recovery.maximumDelayMs",
                       0, kDayMs, &recovery->maximum_delay_ms, validation);
  read_optional_number(object, "multiplier", L"recovery.multiplier", 1, 16,
                       &recovery->multiplier, validation);
  read_optional_number(object, "jitterPercent", L"recovery.jitterPercent", 0,
                       100, &recovery->jitter_percent, validation);
  read_optional_number(object, "windowMs", L"recovery.windowMs", 1, kWeekMs,
                       &recovery->window_ms, validation);
  read_optional_number(object, "maximumRestarts", L"recovery.maximumRestarts",
                       0, 100000, &recovery->maximum_restarts, validation);
  read_optional_number(object, "resetAfterMs", L"recovery.resetAfterMs", 0,
                       kWeekMs, &recovery->reset_after_ms, validation);
  if (recovery->maximum_delay_ms < recovery->initial_delay_ms) {
    validation->add(
        L"recovery.maximumDelayMs must be at least recovery.initialDelayMs");
  }
}

void parse_health(yyjson_val *root, health_check_config_t *health,
                  config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(root, "health");
  if (! object) return;
  if (! yyjson_is_obj(object)) {
    validation->add(L"health must be an object");
    return;
  }
  reject_unknown_fields(
      object, L"health",
      {"type", "initialDelayMs", "intervalMs", "timeoutMs",
       "failureThreshold", "successThreshold", "tcp", "http", "command",
       "file"},
      validation);

  std::wstring type;
  if (! read_required_string(object, "type", L"health.type", &type,
                             validation)) return;
  if (type == L"process") health->type = health_check_type_t::process;
  else if (type == L"tcp") health->type = health_check_type_t::tcp;
  else if (type == L"http") health->type = health_check_type_t::http;
  else if (type == L"command") health->type = health_check_type_t::command;
  else if (type == L"file") health->type = health_check_type_t::file;
  else {
    validation->add(L"health.type has an unsupported value");
    return;
  }

  constexpr unsigned long kDayMs = 24UL * 60UL * 60UL * 1000UL;
  read_optional_number(object, "initialDelayMs", L"health.initialDelayMs", 0,
                       kDayMs, &health->initial_delay_ms, validation);
  read_optional_number(object, "intervalMs", L"health.intervalMs", 100,
                       kDayMs, &health->interval_ms, validation);
  read_optional_number(object, "timeoutMs", L"health.timeoutMs", 100,
                       10000, &health->timeout_ms, validation);
  read_optional_number(object, "failureThreshold", L"health.failureThreshold",
                       1, 1000, &health->failure_threshold, validation);
  read_optional_number(object, "successThreshold", L"health.successThreshold",
                       1, 1000, &health->success_threshold, validation);

  const char *selected = nullptr;
  switch (health->type) {
    case health_check_type_t::tcp: selected = "tcp"; break;
    case health_check_type_t::http: selected = "http"; break;
    case health_check_type_t::command: selected = "command"; break;
    case health_check_type_t::file: selected = "file"; break;
    default: break;
  }
  for (const char *name : {"tcp", "http", "command", "file"}) {
    if (yyjson_obj_get(object, name) && (! selected || strcmp(name, selected))) {
      validation->add(L"health contains settings for a probe other than health.type");
    }
  }
  if (! selected) return;
  yyjson_val *probe = yyjson_obj_get(object, selected);
  if (! yyjson_is_obj(probe)) {
    validation->add(L"health is missing the settings object selected by health.type");
    return;
  }

  if (health->type == health_check_type_t::tcp) {
    reject_unknown_fields(probe, L"health.tcp", {"host", "port"}, validation);
    read_required_string(probe, "host", L"health.tcp.host", &health->host,
                         validation);
    if (health->host.empty()) {
      validation->add(L"health.tcp.host must not be empty");
    }
    unsigned long port = 0;
    if (read_required_number(probe, "port", L"health.tcp.port", 1, 65535,
                             &port, validation)) {
      health->port = static_cast<unsigned short>(port);
    }
  }
  else if (health->type == health_check_type_t::http) {
    reject_unknown_fields(probe, L"health.http",
                          {"url", "minimumStatus", "maximumStatus"}, validation);
    read_required_string(probe, "url", L"health.http.url", &health->url,
                         validation);
    read_optional_number(probe, "minimumStatus", L"health.http.minimumStatus",
                         100, 599, &health->minimum_status, validation);
    read_optional_number(probe, "maximumStatus", L"health.http.maximumStatus",
                         100, 599, &health->maximum_status, validation);
    if (! health->url.starts_with(L"http://") &&
        ! health->url.starts_with(L"https://")) {
      validation->add(L"health.http.url must use http:// or https://");
    }
    if (health->maximum_status < health->minimum_status) {
      validation->add(L"health.http.maximumStatus must be at least minimumStatus");
    }
  }
  else if (health->type == health_check_type_t::command) {
    reject_unknown_fields(probe, L"health.command", {"path", "arguments"},
                          validation);
    read_required_string(probe, "path", L"health.command.path",
                         &health->command_path, validation);
    if (health->command_path.empty() || PathIsRelativeW(health->command_path.c_str())) {
      validation->add(L"health.command.path must be an absolute path");
    }
    parse_string_array(probe, "arguments", L"health.command.arguments",
                       &health->command_arguments, validation);
    std::wstring command_line;
    std::wstring command_error;
    if (! health->command_path.empty() &&
        ! build_child_command_line(health->command_path,
                                  health->command_arguments, &command_line,
                                  &command_error)) {
      validation->add(L"health command line is invalid: " + command_error);
    }
  }
  else if (health->type == health_check_type_t::file) {
    reject_unknown_fields(probe, L"health.file", {"path", "maximumAgeMs"},
                          validation);
    read_required_string(probe, "path", L"health.file.path",
                         &health->file_path, validation);
    if (health->file_path.empty() || PathIsRelativeW(health->file_path.c_str())) {
      validation->add(L"health.file.path must be an absolute path");
    }
    read_optional_number(probe, "maximumAgeMs", L"health.file.maximumAgeMs",
                         1, kDayMs, &health->maximum_age_ms, validation);
  }
}

void parse_resources(yyjson_val *root, resource_limit_config_t *resources,
                     config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(root, "resources");
  if (! object) return;
  if (! yyjson_is_obj(object)) {
    validation->add(L"resources must be an object");
    return;
  }
  reject_unknown_fields(object, L"resources",
                        {"monitorIntervalMs", "memoryLimitBytes",
                         "maximumProcesses", "cpu"},
                        validation);
  read_optional_number(object, "monitorIntervalMs",
                       L"resources.monitorIntervalMs", 100, 60000,
                       &resources->monitor_interval_ms, validation);
  constexpr unsigned long long kMaximumMemory = 16ULL * 1024ULL * 1024ULL *
                                                 1024ULL * 1024ULL;
  read_optional_number64(object, "memoryLimitBytes",
                         L"resources.memoryLimitBytes", kMaximumMemory,
                         &resources->memory_limit_bytes, validation);
  read_optional_number(object, "maximumProcesses",
                       L"resources.maximumProcesses", 0, 65535,
                       &resources->maximum_processes, validation);

  yyjson_val *cpu = yyjson_obj_get(object, "cpu");
  if (! cpu) return;
  if (! yyjson_is_obj(cpu)) {
    validation->add(L"resources.cpu must be an object");
    return;
  }
  reject_unknown_fields(cpu, L"resources.cpu",
                        {"maximumPercent", "violationThreshold"}, validation);
  read_required_number(cpu, "maximumPercent",
                       L"resources.cpu.maximumPercent", 1, 100,
                       &resources->maximum_cpu_percent, validation);
  read_optional_number(cpu, "violationThreshold",
                       L"resources.cpu.violationThreshold", 1, 1000,
                       &resources->cpu_violation_threshold, validation);
}

std::wstring read_error_message(unsigned long error) {
  return L"Unable to read configuration file (Windows error " +
         std::to_wstring(error) + L")";
}

}  // namespace

bool parse_service_config(const char *json, size_t length,
                          service_config_t *config,
                          config_validation_t *validation) {
  if (! config || ! validation) return false;
  *config = {};
  validation->errors.clear();
  if (! json || ! length) {
    validation->add(L"Configuration file is empty");
    return false;
  }

  yyjson_read_err parse_error = {};
  yyjson_doc *document = yyjson_read_opts(const_cast<char *>(json), length, 0,
                                          nullptr, &parse_error);
  if (! document) {
    bool valid = false;
    std::wstring message = utf8_to_wide(parse_error.msg, &valid);
    validation->add(L"Invalid JSON at byte " + std::to_wstring(parse_error.pos) +
                    L": " + (valid ? message : L"parse error"));
    return false;
  }

  yyjson_val *root = yyjson_doc_get_root(document);
  if (! yyjson_is_obj(root)) {
    validation->add(L"Configuration root must be an object");
    yyjson_doc_free(document);
    return false;
  }
  reject_unknown_fields(root, L"root",
                        {"schema", "service", "application", "recovery",
                         "health", "resources"},
                        validation);

  yyjson_val *schema = yyjson_obj_get(root, "schema");
  if (! yyjson_is_uint(schema) || yyjson_get_uint(schema) != 1) {
    validation->add(L"schema must be the integer 1");
  }
  else config->schema = 1;

  parse_service(root, &config->service, validation);
  parse_application(root, &config->application, validation);
  parse_recovery(root, &config->recovery, validation);
  parse_health(root, &config->health, validation);
  parse_resources(root, &config->resources, validation);
  yyjson_doc_free(document);
  return validation->ok();
}

bool load_service_config(const wchar_t *path, service_config_t *config,
                         config_validation_t *validation) {
  if (! config || ! validation) return false;
  validation->errors.clear();
  if (! path || ! *path) {
    validation->add(L"Configuration path must not be empty");
    return false;
  }

  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    validation->add(read_error_message(GetLastError()));
    return false;
  }

  LARGE_INTEGER size = {};
  if (! GetFileSizeEx(file, &size)) {
    unsigned long error = GetLastError();
    CloseHandle(file);
    validation->add(read_error_message(error));
    return false;
  }
  if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) >
                                kMaximumConfigBytes) {
    CloseHandle(file);
    validation->add(L"Configuration file must be between 1 byte and 4 MiB");
    return false;
  }

  std::vector<char> content(static_cast<size_t>(size.QuadPart));
  size_t offset = 0;
  while (offset < content.size()) {
    unsigned long chunk = static_cast<unsigned long>(
        (content.size() - offset) > 1024 * 1024 ? 1024 * 1024
                                                : content.size() - offset);
    unsigned long read = 0;
    if (! ReadFile(file, content.data() + offset, chunk, &read, nullptr) ||
        ! read) {
      unsigned long error = GetLastError();
      CloseHandle(file);
      validation->add(read_error_message(error));
      return false;
    }
    offset += read;
  }
  CloseHandle(file);

  size_t start = content.size() >= 3 &&
                         static_cast<unsigned char>(content[0]) == 0xEF &&
                         static_cast<unsigned char>(content[1]) == 0xBB &&
                         static_cast<unsigned char>(content[2]) == 0xBF
                     ? 3
                     : 0;
  return parse_service_config(content.data() + start, content.size() - start,
                              config, validation);
}

int validate_service_config_command(int argc, wchar_t **argv) {
  if (argc != 1 || ! argv || ! argv[0]) {
    fwprintf(stderr, L"Usage: nssm validate <config.json>\n");
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
  fwprintf(stdout, L"Configuration is valid: %ls\n",
           config.service.name.c_str());
  return 0;
}
