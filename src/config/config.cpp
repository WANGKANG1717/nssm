#include "config.h"
#include "config_parse_utils.h"
#include "child_process.h"
#include "string_utils.h"

#include <windows.h>
#include <shlwapi.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

bool valid_service_name(const std::wstring &name) {
  if (name.empty() || name.size() > kMaximumServiceNameCharacters) return false;
  for (wchar_t character : name) {
    if (character == L'/' || character == L'\\' || character < 32) return false;
  }
  return true;
}

const toml::table *configuration_table(const toml::table *parent,
                                       const char *name,
                                       const wchar_t *path,
                                       bool required,
                                       config_validation_t *validation) {
  const toml::node *node = parent ? parent->get(name) : nullptr;
  if (! node) {
    if (required) validation->add(std::wstring(path) + L" must be a table");
    return nullptr;
  }
  const toml::table *table = node->as_table();
  if (! table) validation->add(std::wstring(path) + L" must be a table");
  return table;
}

void parse_account(const toml::table *windows,
                   service_account_config_t *account,
                   config_validation_t *validation) {
  std::wstring type;
  if (! read_optional_string(windows, "Account", L"Windows.Account", &type,
                             validation) || type.empty()) return;
  if (type == L"system") account->type = service_account_type_t::system;
  else if (type == L"localService") account->type = service_account_type_t::local_service;
  else if (type == L"networkService") account->type = service_account_type_t::network_service;
  else if (type == L"virtual") account->type = service_account_type_t::virtual_account;
  else if (type == L"gmsa") account->type = service_account_type_t::gmsa;
  else if (type == L"user") account->type = service_account_type_t::user;
  else {
    validation->add(L"Windows.Account has an unsupported value");
    return;
  }

  read_optional_string(windows, "AccountName", L"Windows.AccountName", &account->name,
                       validation);
  bool needs_name = account->type == service_account_type_t::gmsa ||
                    account->type == service_account_type_t::user;
  if (needs_name && account->name.empty()) {
    validation->add(L"Windows.AccountName is required for this account type");
  }
  if (! needs_name && ! account->name.empty()) {
    if (account->type != service_account_type_t::virtual_account) {
      validation->add(L"Windows.AccountName is not allowed for this account type");
    }
  }
  if (account->type == service_account_type_t::gmsa &&
      ! account->name.empty() && ! account->name.ends_with(L'$')) {
    validation->add(L"Windows.AccountName for a gMSA must end with '$'");
  }
}

void parse_security(const toml::table *windows, security_config_t *security,
                    config_validation_t *validation) {
  if (! windows || ! windows->get("ServiceSid")) return;
  std::wstring service_sid;
  if (! read_required_string(windows, "ServiceSid", L"Windows.ServiceSid",
                             &service_sid, validation)) return;
  if (service_sid.empty()) {
    validation->add(L"Windows.ServiceSid must not be empty");
    return;
  }
  if (service_sid == L"none") {
    security->service_sid = service_sid_type_t::none;
  }
  else if (service_sid == L"unrestricted") {
    security->service_sid = service_sid_type_t::unrestricted;
  }
  else if (service_sid == L"restricted") {
    security->service_sid = service_sid_type_t::restricted;
  }
  else {
    validation->add(L"Windows.ServiceSid has an unsupported value");
  }
}

void parse_dependencies(const toml::table *unit,
                        std::vector<std::wstring> *dependencies,
                        config_validation_t *validation) {
  const toml::node *node = unit->get("Requires");
  if (! node) return;
  const toml::array *array = node->as_array();
  if (! array) {
    validation->add(L"Unit.Requires must be an array of strings");
    return;
  }

  size_t index = 0;
  for (const toml::node &element : *array) {
    const auto value = element.value<std::string>();
    if (! value) {
      validation->add(L"Unit.Requires[" + std::to_wstring(index) +
                      L"] must be a string");
    }
    else {
      bool valid = false;
      std::wstring dependency = utf8_to_wide(*value, &valid);
      if (! valid || dependency.empty() ||
          dependency.find(L'\0') != std::wstring::npos) {
        validation->add(L"Unit.Requires[" + std::to_wstring(index) +
                        L"] must be non-empty valid UTF-8");
      }
      else dependencies->push_back(std::move(dependency));
    }
    ++index;
  }
}

void parse_unit(const toml::table *root,
                   service_metadata_config_t *service,
                   config_validation_t *validation) {
  const toml::table *object = configuration_table(
      root, "Unit", L"Unit", true, validation);
  if (! object) return;
  reject_unknown_fields(object, L"Unit",
                        {"Name", "Description", "DisplayName", "Requires"},
                        validation);

  read_required_string(object, "Name", L"Unit.Name", &service->name,
                       validation);
  if (! valid_service_name(service->name)) {
    validation->add(L"Unit.Name is empty, too long, or contains an invalid character");
  }
  read_optional_string(object, "DisplayName", L"Unit.DisplayName",
                       &service->display_name, validation);
  read_optional_string(object, "Description", L"Unit.Description",
                       &service->description, validation);
  if (service->display_name.empty()) service->display_name = service->name;
  if (service->display_name.size() > kMaximumServiceNameCharacters) {
    validation->add(L"Unit.DisplayName must not exceed 256 characters");
  }
  parse_dependencies(object, &service->dependencies, validation);
}

void parse_string_array(const toml::table *object, const char *name,
                        const wchar_t *path,
                        std::vector<std::wstring> *arguments,
                        config_validation_t *validation) {
  const toml::node *node = object->get(name);
  if (! node) return;
  const toml::array *array = node->as_array();
  if (! array) {
    validation->add(std::wstring(path) + L" must be an array of strings");
    return;
  }

  size_t index = 0;
  for (const toml::node &element : *array) {
    const auto value = element.value<std::string>();
    if (! value) {
      validation->add(std::wstring(path) + L"[" + std::to_wstring(index) +
                      L"] must be a string");
    }
    else {
      bool valid = false;
      std::wstring argument = utf8_to_wide(*value, &valid);
      if (! valid || argument.find(L'\0') != std::wstring::npos) {
        validation->add(std::wstring(path) + L"[" + std::to_wstring(index) +
                        L"] must be valid UTF-8 without U+0000");
      }
      else arguments->push_back(std::move(argument));
    }
    ++index;
  }
}

void parse_environment(const toml::table *service,
                       application_config_t *config,
                       config_validation_t *validation) {
  read_optional_bool(service, "InheritEnvironment",
                     L"Service.InheritEnvironment",
                     &config->inherit_environment, validation);

  const toml::node *node = service->get("Environment");
  if (! node) return;
  const toml::array *environment = node->as_array();
  if (! environment) {
    validation->add(L"Service.Environment must be an array of NAME=value strings");
    return;
  }

  std::set<std::wstring, ordinal_ignore_case_less> names;
  size_t index = 0;
  for (const toml::node &entry : *environment) {
    const auto text = entry.value<std::string>();
    bool valid = false;
    std::wstring converted = text ? utf8_to_wide(*text, &valid) : L"";
    const size_t separator = converted.find(L'=');
    if (! valid || separator == std::wstring::npos || ! separator ||
        converted.find(L'\0') != std::wstring::npos) {
      validation->add(L"Service.Environment[" + std::to_wstring(index) +
                      L"] must be a valid NAME=value string");
      ++index;
      continue;
    }
    std::wstring name = converted.substr(0, separator);
    if (! names.insert(name).second) {
      validation->add(L"Service.Environment contains duplicate names ignoring case: " + name);
    }
    else config->environment.emplace_back(
        std::move(name), converted.substr(separator + 1));
    ++index;
  }
}

void parse_application(const toml::table *service,
                       application_config_t *application,
                       config_validation_t *validation) {
  read_required_string(service, "ExecStart", L"Service.ExecStart", &application->path,
                       validation);
  if (application->path.empty() || PathIsRelativeW(application->path.c_str())) {
    validation->add(L"Service.ExecStart must be an absolute executable path");
  }
  if (application->path.size() > kMaximumPathCharacters) {
    validation->add(L"Service.ExecStart is too long");
  }
  read_optional_string(service, "WorkingDirectory",
                       L"Service.WorkingDirectory",
                       &application->working_directory, validation);
  if (! application->working_directory.empty() &&
      PathIsRelativeW(application->working_directory.c_str())) {
    validation->add(L"Service.WorkingDirectory must be an absolute path");
  }
  if (application->working_directory.empty() && ! application->path.empty()) {
    size_t separator = application->path.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
      size_t length = separator == 2 && application->path[1] == L':'
                          ? separator + 1
                          : separator;
      application->working_directory = application->path.substr(0, length);
    }
  }

  parse_string_array(service, "Arguments", L"Service.Arguments",
                     &application->arguments, validation);
  std::wstring serialized = join_windows_arguments(application->arguments);
  if (serialized.size() > kMaximumSerializedArgumentCharacters) {
    validation->add(L"Service.Arguments serialize to more than 16382 characters");
  }
  std::wstring command_line;
  std::wstring command_error;
  if (! application->path.empty() &&
      ! build_child_command_line(application->path, serialized, &command_line,
                                 &command_error)) {
    validation->add(L"Service command line is invalid: " + command_error);
  }
  if (application->working_directory.size() >
      kMaximumWorkingDirectoryCharacters) {
    validation->add(L"Service.WorkingDirectory is too long");
  }
  parse_environment(service, application, validation);
}

void parse_commands(const toml::table *service, const char *name,
                    const wchar_t *path,
                    std::vector<command_config_t> *commands,
                    config_validation_t *validation) {
  const toml::node *node = service->get(name);
  if (! node) return;
  const toml::array *array = node->as_array();
  if (! array) {
    validation->add(std::wstring(path) + L" must be an array of inline tables");
    return;
  }
  if (array->size() > 64) {
    validation->add(std::wstring(path) + L" must contain at most 64 commands");
  }

  size_t index = 0;
  for (const toml::node &element : *array) {
    std::wstring item_path = std::wstring(path) + L"[" +
                             std::to_wstring(index) + L"]";
    const toml::table *table = element.as_table();
    if (! table) {
      validation->add(item_path + L" must be an inline table");
      ++index;
      continue;
    }
    reject_unknown_fields(table, item_path.c_str(), {"Path", "Arguments"},
                          validation);
    command_config_t command;
    std::wstring field_path = item_path + L".Path";
    read_required_string(table, "Path", field_path.c_str(), &command.path,
                         validation);
    if (command.path.empty() || PathIsRelativeW(command.path.c_str())) {
      validation->add(field_path + L" must be an absolute executable path");
    }
    if (command.path.size() > kMaximumPathCharacters) {
      validation->add(field_path + L" is too long");
    }
    field_path = item_path + L".Arguments";
    parse_string_array(table, "Arguments", field_path.c_str(),
                       &command.arguments, validation);
    std::wstring command_line;
    std::wstring command_error;
    if (! command.path.empty() &&
        ! build_child_command_line(command.path, command.arguments,
                                   &command_line, &command_error)) {
      validation->add(item_path + L" command line is invalid: " +
                      command_error);
    }
    if (index < 64) commands->push_back(std::move(command));
    ++index;
  }
}

void parse_lifecycle(const toml::table *service,
                     lifecycle_config_t *lifecycle,
                     config_validation_t *validation) {
  parse_commands(service, "ExecStartPre", L"Service.ExecStartPre",
                 &lifecycle->exec_start_pre, validation);
  parse_commands(service, "ExecStartPost", L"Service.ExecStartPost",
                 &lifecycle->exec_start_post, validation);
  parse_commands(service, "ExecStop", L"Service.ExecStop",
                 &lifecycle->exec_stop, validation);
  parse_commands(service, "ExecStopPost", L"Service.ExecStopPost",
                 &lifecycle->exec_stop_post, validation);
  parse_commands(service, "ExecReload", L"Service.ExecReload",
                 &lifecycle->exec_reload, validation);
  constexpr unsigned long kDayMs = 24UL * 60UL * 60UL * 1000UL;
  read_optional_duration_ms(service, "TimeoutStartSec",
                            L"Service.TimeoutStartSec", 100, kDayMs,
                            &lifecycle->timeout_start_ms, validation);
  read_optional_duration_ms(service, "TimeoutStopSec",
                            L"Service.TimeoutStopSec", 100, kDayMs,
                            &lifecycle->timeout_stop_ms, validation);
}

void parse_recovery(const toml::table *service, recovery_config_t *recovery,
                    config_validation_t *validation) {
  std::wstring mode;
  if (read_optional_string(service, "Restart", L"Service.Restart", &mode,
                           validation) && ! mode.empty()) {
    if (mode == L"never") recovery->mode = recovery_mode_t::never;
    else if (mode == L"on-failure") recovery->mode = recovery_mode_t::on_failure;
    else if (mode == L"always") recovery->mode = recovery_mode_t::always;
    else validation->add(L"Service.Restart must be never, on-failure, or always");
  }
  constexpr unsigned long kDayMs = 24UL * 60UL * 60UL * 1000UL;
  constexpr unsigned long kWeekMs = 7UL * kDayMs;
  read_optional_duration_ms(service, "RestartSec", L"Service.RestartSec",
                       0, kDayMs, &recovery->initial_delay_ms, validation);
  read_optional_duration_ms(service, "RestartMaxDelaySec", L"Service.RestartMaxDelaySec",
                       0, kDayMs, &recovery->maximum_delay_ms, validation);
  read_optional_number(service, "RestartSteps", L"Service.RestartSteps", 1, 16,
                       &recovery->multiplier, validation);
  read_optional_number(service, "RestartJitterPercent", L"Service.RestartJitterPercent", 0,
                       100, &recovery->jitter_percent, validation);
  read_optional_duration_ms(service, "StartLimitIntervalSec", L"Service.StartLimitIntervalSec", 1, kWeekMs,
                       &recovery->window_ms, validation);
  read_optional_number(service, "StartLimitBurst", L"Service.StartLimitBurst",
                       0, 100000, &recovery->maximum_restarts, validation);
  read_optional_duration_ms(service, "RestartResetSec", L"Service.RestartResetSec", 0,
                       kWeekMs, &recovery->reset_after_ms, validation);
  if (recovery->maximum_delay_ms < recovery->initial_delay_ms) {
    validation->add(
        L"Service.RestartMaxDelaySec must be at least Service.RestartSec");
  }
}

bool parse_probe_target(const toml::table *object,
                        const std::wstring &section,
                        health_check_config_t *health,
                        config_validation_t *validation) {
  std::wstring type;
  std::wstring type_path = section + L".Type";
  if (! read_required_string(object, "Type", type_path.c_str(), &type,
                             validation)) return false;
  if (type == L"process") health->type = health_check_type_t::process;
  else if (type == L"tcp") health->type = health_check_type_t::tcp;
  else if (type == L"http") health->type = health_check_type_t::http;
  else if (type == L"command") health->type = health_check_type_t::command;
  else if (type == L"file") health->type = health_check_type_t::file;
  else {
    validation->add(type_path + L" has an unsupported value");
    return false;
  }

  const char *selected = nullptr;
  switch (health->type) {
    case health_check_type_t::tcp: selected = "TCP"; break;
    case health_check_type_t::http: selected = "HTTP"; break;
    case health_check_type_t::command: selected = "Command"; break;
    case health_check_type_t::file: selected = "File"; break;
    default: break;
  }
  for (const char *name : {"TCP", "HTTP", "Command", "File"}) {
    if (object->get(name) && (! selected || strcmp(name, selected))) {
      validation->add(section +
                      L" contains settings for a probe other than " +
                      section + L".Type");
    }
  }
  if (! selected) return true;
  const toml::node *probe_node = object->get(selected);
  const toml::table *probe = probe_node ? probe_node->as_table() : nullptr;
  if (! probe) {
    validation->add(section +
                    L" is missing the settings object selected by " +
                    section + L".type");
    return false;
  }

  std::wstring probe_path = section + L"." + utf8_to_wide(selected);
  if (health->type == health_check_type_t::tcp) {
    reject_unknown_fields(probe, probe_path.c_str(), {"Host", "Port"},
                          validation);
    std::wstring host_path = probe_path + L".Host";
    read_required_string(probe, "Host", host_path.c_str(), &health->host,
                         validation);
    if (health->host.empty()) {
      validation->add(host_path + L" must not be empty");
    }
    unsigned long port = 0;
    std::wstring port_path = probe_path + L".Port";
    if (read_required_number(probe, "Port", port_path.c_str(), 1, 65535,
                             &port, validation)) {
      health->port = static_cast<unsigned short>(port);
    }
  }
  else if (health->type == health_check_type_t::http) {
    reject_unknown_fields(probe, probe_path.c_str(),
                          {"URL", "MinimumStatus", "MaximumStatus"}, validation);
    std::wstring url_path = probe_path + L".URL";
    std::wstring minimum_path = probe_path + L".MinimumStatus";
    std::wstring maximum_path = probe_path + L".MaximumStatus";
    read_required_string(probe, "URL", url_path.c_str(), &health->url,
                         validation);
    read_optional_number(probe, "MinimumStatus", minimum_path.c_str(),
                         100, 599, &health->minimum_status, validation);
    read_optional_number(probe, "MaximumStatus", maximum_path.c_str(),
                         100, 599, &health->maximum_status, validation);
    if (! health->url.starts_with(L"http://") &&
        ! health->url.starts_with(L"https://")) {
      validation->add(url_path + L" must use http:// or https://");
    }
    if (health->maximum_status < health->minimum_status) {
      validation->add(maximum_path + L" must be at least minimumStatus");
    }
  }
  else if (health->type == health_check_type_t::command) {
    reject_unknown_fields(probe, probe_path.c_str(), {"ExecStart", "Arguments"},
                          validation);
    std::wstring path_path = probe_path + L".ExecStart";
    std::wstring arguments_path = probe_path + L".Arguments";
    read_required_string(probe, "ExecStart", path_path.c_str(),
                         &health->command_path, validation);
    if (health->command_path.empty() || PathIsRelativeW(health->command_path.c_str())) {
      validation->add(path_path + L" must be an absolute path");
    }
    parse_string_array(probe, "Arguments", arguments_path.c_str(),
                       &health->command_arguments, validation);
    std::wstring command_line;
    std::wstring command_error;
    if (! health->command_path.empty() &&
        ! build_child_command_line(health->command_path,
                                  health->command_arguments, &command_line,
                                  &command_error)) {
      validation->add(section + L" command line is invalid: " + command_error);
    }
  }
  else if (health->type == health_check_type_t::file) {
    reject_unknown_fields(probe, probe_path.c_str(), {"Path", "MaximumAgeSec"},
                          validation);
    std::wstring path_path = probe_path + L".Path";
    std::wstring age_path = probe_path + L".MaximumAgeSec";
    read_required_string(probe, "Path", path_path.c_str(),
                         &health->file_path, validation);
    if (health->file_path.empty() || PathIsRelativeW(health->file_path.c_str())) {
      validation->add(path_path + L" must be an absolute path");
    }
    constexpr unsigned long kDayMs = 24UL * 60UL * 60UL * 1000UL;
    read_optional_duration_ms(probe, "MaximumAgeSec", age_path.c_str(),
                         1, kDayMs, &health->maximum_age_ms, validation);
  }
  return true;
}

void parse_health(const toml::table *root, health_check_config_t *health,
                  config_validation_t *validation) {
  const toml::table *object = configuration_table(
      root, "Health", L"Health", false, validation);
  if (! object) return;
  reject_unknown_fields(
      object, L"Health",
      {"Type", "InitialDelaySec", "IntervalSec", "TimeoutSec",
       "FailureThreshold", "SuccessThreshold", "TCP", "HTTP", "Command",
       "File"},
      validation);

  parse_probe_target(object, L"Health", health, validation);
  constexpr unsigned long kDayMs = 24UL * 60UL * 60UL * 1000UL;
  read_optional_duration_ms(object, "InitialDelaySec", L"Health.InitialDelaySec", 0,
                       kDayMs, &health->initial_delay_ms, validation);
  read_optional_duration_ms(object, "IntervalSec", L"Health.IntervalSec", 100,
                       kDayMs, &health->interval_ms, validation);
  read_optional_duration_ms(object, "TimeoutSec", L"Health.TimeoutSec", 100,
                       10000, &health->timeout_ms, validation);
  read_optional_number(object, "FailureThreshold", L"Health.FailureThreshold",
                       1, 1000, &health->failure_threshold, validation);
  read_optional_number(object, "SuccessThreshold", L"Health.SuccessThreshold",
                       1, 1000, &health->success_threshold, validation);
}

void parse_readiness(const toml::table *root, readiness_config_t *readiness,
                     config_validation_t *validation) {
  const toml::table *object = configuration_table(
      root, "Readiness", L"Readiness", false, validation);
  if (! object) return;
  reject_unknown_fields(
      object, L"Readiness",
      {"Type", "InitialDelaySec", "IntervalSec", "TimeoutSec",
       "StartupTimeoutSec", "SuccessThreshold", "TCP", "HTTP", "Command",
       "File"},
      validation);

  parse_probe_target(object, L"Readiness", &readiness->probe, validation);
  constexpr unsigned long kDayMs = 24UL * 60UL * 60UL * 1000UL;
  read_optional_duration_ms(object, "InitialDelaySec", L"Readiness.InitialDelaySec",
                       0, kDayMs, &readiness->probe.initial_delay_ms,
                       validation);
  read_optional_duration_ms(object, "IntervalSec", L"Readiness.IntervalSec", 100,
                       kDayMs, &readiness->probe.interval_ms, validation);
  read_optional_duration_ms(object, "TimeoutSec", L"Readiness.TimeoutSec", 100,
                       10000, &readiness->probe.timeout_ms, validation);
  read_optional_duration_ms(object, "StartupTimeoutSec",
                       L"Readiness.StartupTimeoutSec", 100, kDayMs,
                       &readiness->startup_timeout_ms, validation);
  read_optional_number(object, "SuccessThreshold",
                       L"Readiness.SuccessThreshold", 1, 1000,
                       &readiness->probe.success_threshold, validation);
  if (readiness->probe.initial_delay_ms >= readiness->startup_timeout_ms) {
    validation->add(
        L"readiness.initialDelayMs must be less than startupTimeoutMs");
  }
}

void parse_service_options(const toml::table *root,
                           service_config_t *config,
                           config_validation_t *validation) {
  const toml::table *service = configuration_table(
      root, "Service", L"Service", true, validation);
  if (! service) return;
  reject_unknown_fields(
      service, L"Service",
      {"ExecStart", "Arguments", "WorkingDirectory", "Environment",
       "ExecStartPre", "ExecStartPost", "ExecStop", "ExecStopPost", "ExecReload",
       "TimeoutStartSec", "TimeoutStopSec",
       "InheritEnvironment", "Restart", "RestartSec",
       "RestartMaxDelaySec", "RestartSteps", "RestartJitterPercent",
       "StartLimitIntervalSec", "StartLimitBurst", "RestartResetSec",
       "MemoryMax", "TasksMax", "CPUQuota"}, validation);
  parse_application(service, &config->application, validation);
  parse_lifecycle(service, &config->lifecycle, validation);
  parse_recovery(service, &config->recovery, validation);
  constexpr unsigned long long kMaximumMemory =
      16ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  read_optional_number64(service, "MemoryMax", L"Service.MemoryMax",
                         kMaximumMemory, &config->resources.memory_limit_bytes,
                         validation);
  read_optional_number(service, "TasksMax", L"Service.TasksMax", 0, 65535,
                       &config->resources.maximum_processes, validation);
  read_optional_number(service, "CPUQuota", L"Service.CPUQuota", 1, 100,
                       &config->resources.maximum_cpu_percent, validation);
}

void parse_windows(const toml::table *root, service_config_t *config,
                   config_validation_t *validation) {
  const toml::table *windows = configuration_table(
      root, "Windows", L"Windows", false, validation);
  if (! windows) return;
  reject_unknown_fields(windows, L"Windows",
                        {"Account", "AccountName", "ServiceSid",
                         "CPUMonitorInterval",
                         "CPUViolationThreshold"}, validation);
  parse_account(windows, &config->service.account, validation);
  if (config->service.account.type == service_account_type_t::virtual_account &&
      ! config->service.account.name.empty() &&
      ! strings_equal_ignore_case(config->service.account.name.c_str(),
                                  config->service.name.c_str())) {
    validation->add(L"Windows.AccountName for a virtual account must match Unit.Name");
  }
  parse_security(windows, &config->security, validation);
  read_optional_duration_ms(windows, "CPUMonitorInterval",
                            L"Windows.CPUMonitorInterval", 100, 60000,
                            &config->resources.monitor_interval_ms, validation);
  read_optional_number(windows, "CPUViolationThreshold",
                       L"Windows.CPUViolationThreshold", 1, 1000,
                       &config->resources.cpu_violation_threshold, validation);
}

void parse_log_stream(const toml::table *logging, const char *name,
                      const wchar_t *path, std::wstring *destination,
                      config_validation_t *validation) {
  const toml::table *stream = configuration_table(
      logging, name, path, false, validation);
  if (! stream) return;
  reject_unknown_fields(stream, path, {"Path"}, validation);
  std::wstring path_field = std::wstring(path) + L".Path";
  read_required_string(stream, "Path", path_field.c_str(), destination,
                       validation);
  if (destination->empty() || PathIsRelativeW(destination->c_str())) {
    validation->add(path_field + L" must be an absolute path");
  }
  if (destination->size() > kMaximumPathCharacters) {
    validation->add(path_field + L" is too long");
  }
}

void parse_logging(const toml::table *root, logging_config_t *logging,
                   config_validation_t *validation) {
  const toml::table *object = configuration_table(
      root, "Logging", L"Logging", false, validation);
  if (! object) return;
  reject_unknown_fields(object, L"Logging",
                        {"Stdout", "Stderr", "Timestamp", "Rotation"},
                        validation);
  parse_log_stream(object, "Stdout", L"Logging.Stdout",
                   &logging->stdout_path, validation);
  parse_log_stream(object, "Stderr", L"Logging.Stderr",
                   &logging->stderr_path, validation);
  read_optional_bool(object, "Timestamp", L"Logging.Timestamp",
                     &logging->timestamp, validation);

  const toml::table *rotation = configuration_table(
      object, "Rotation", L"Logging.Rotation", false, validation);
  if (rotation) {
    reject_unknown_fields(rotation, L"Logging.Rotation", {"MaximumBytes"},
                          validation);
    constexpr unsigned long long kMaximumLogBytes =
        16ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    read_optional_number64(rotation, "MaximumBytes",
                           L"Logging.Rotation.MaximumBytes",
                           kMaximumLogBytes, &logging->maximum_bytes,
                           validation);
  }
  if (! logging->enabled()) {
    validation->add(L"Logging must configure stdout and/or stderr");
  }
}

}  // namespace

bool parse_service_config(const char *toml_text, size_t length,
                          service_config_t *config,
                          config_validation_t *validation) {
  if (! config || ! validation) return false;
  *config = {};
  validation->errors.clear();
  if (! toml_text || ! length) {
    validation->add(L"Configuration file is empty");
    return false;
  }

  toml::table root;
  try {
    root = toml::parse(std::string_view(toml_text, length));
  }
  catch (const toml::parse_error &parse_error) {
    bool valid = false;
    const std::string_view description = parse_error.description();
    std::wstring message = utf8_to_wide(description, &valid);
    const auto &position = parse_error.source().begin;
    validation->add(L"Invalid TOML at line " +
                    std::to_wstring(position.line) + L", column " +
                    std::to_wstring(position.column) + L": " +
                    (valid ? message : L"parse error"));
    return false;
  }

  reject_unknown_fields(&root, L"root",
                        {"Schema", "Unit", "Service", "Windows",
                         "Readiness", "Health", "Logging"},
                        validation);

  const toml::node *schema = root.get("Schema");
  const auto schema_value = schema ? schema->value<int64_t>() : std::nullopt;
  if (! schema_value || *schema_value != 1) {
    validation->add(L"Schema must be the integer 1");
  }
  else config->schema = 1;

  parse_unit(&root, &config->service, validation);
  parse_service_options(&root, config, validation);
  parse_windows(&root, config, validation);
  parse_readiness(&root, &config->readiness, validation);
  parse_health(&root, &config->health, validation);
  parse_logging(&root, &config->logging, validation);
  return validation->ok();
}
