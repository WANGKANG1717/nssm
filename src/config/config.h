#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <utility>
#include <vector>

inline constexpr size_t kMaximumServiceNameCharacters = 256;
inline constexpr size_t kMaximumPathCharacters = 32766;
inline constexpr size_t kMaximumWorkingDirectoryCharacters = 32754;
inline constexpr size_t kMaximumSerializedArgumentCharacters = 16382;

enum class service_startup_t {
  automatic,
  delayed,
  manual,
  disabled,
};

enum class service_account_type_t {
  system,
  local_service,
  network_service,
  virtual_account,
  gmsa,
  user,
};

enum class service_sid_type_t {
  none,
  unrestricted,
  restricted,
};

enum class recovery_mode_t {
  never,
  on_failure,
  always,
};

struct recovery_config_t {
  recovery_mode_t mode = recovery_mode_t::on_failure;
  unsigned long initial_delay_ms = 1000;
  unsigned long maximum_delay_ms = 60000;
  unsigned long multiplier = 2;
  unsigned long jitter_percent = 10;
  unsigned long window_ms = 60000;
  unsigned long maximum_restarts = 5;
  unsigned long reset_after_ms = 60000;

  bool operator==(const recovery_config_t &) const = default;
};

enum class health_check_type_t {
  disabled,
  process,
  tcp,
  http,
  command,
  file,
};

struct health_check_config_t {
  health_check_type_t type = health_check_type_t::disabled;
  unsigned long initial_delay_ms = 10000;
  unsigned long interval_ms = 10000;
  unsigned long timeout_ms = 5000;
  unsigned long failure_threshold = 3;
  unsigned long success_threshold = 1;

  std::wstring host;
  unsigned short port = 0;

  std::wstring url;
  unsigned long minimum_status = 200;
  unsigned long maximum_status = 399;

  std::wstring command_path;
  std::vector<std::wstring> command_arguments;

  std::wstring file_path;
  unsigned long maximum_age_ms = 60000;

  bool operator==(const health_check_config_t &) const = default;
};

struct readiness_config_t {
  health_check_config_t probe;
  unsigned long startup_timeout_ms = 60000;

  readiness_config_t() {
    probe.initial_delay_ms = 0;
    probe.interval_ms = 1000;
    probe.timeout_ms = 1000;
    probe.failure_threshold = 1;
    probe.success_threshold = 1;
  }

  bool enabled() const {
    return probe.type != health_check_type_t::disabled;
  }

  bool operator==(const readiness_config_t &) const = default;
};

struct resource_limit_config_t {
  unsigned long monitor_interval_ms = 1000;
  unsigned long long memory_limit_bytes = 0;
  unsigned long maximum_processes = 0;
  unsigned long maximum_cpu_percent = 0;
  unsigned long cpu_violation_threshold = 3;

  bool enabled() const {
    return memory_limit_bytes || maximum_processes || maximum_cpu_percent;
  }

  bool operator==(const resource_limit_config_t &) const = default;
};

struct logging_config_t {
  std::wstring stdout_path;
  std::wstring stderr_path;
  bool timestamp = false;
  unsigned long long maximum_bytes = 0;

  bool enabled() const {
    return ! stdout_path.empty() || ! stderr_path.empty();
  }

  bool operator==(const logging_config_t &) const = default;
};

struct service_account_config_t {
  service_account_type_t type = service_account_type_t::virtual_account;
  std::wstring name;

  bool operator==(const service_account_config_t &) const = default;
};

struct security_config_t {
  service_sid_type_t service_sid = service_sid_type_t::unrestricted;

  bool operator==(const security_config_t &) const = default;
};

struct service_metadata_config_t {
  std::wstring name;
  std::wstring display_name;
  std::wstring description;
  service_startup_t startup = service_startup_t::manual;
  service_account_config_t account;
  std::vector<std::wstring> dependencies;

  bool operator==(const service_metadata_config_t &) const = default;
};

struct application_config_t {
  std::wstring path;
  std::vector<std::wstring> arguments;
  std::wstring working_directory;
  bool inherit_environment = true;
  std::vector<std::pair<std::wstring, std::wstring>> environment;

  bool operator==(const application_config_t &) const = default;
};

struct service_config_t {
  unsigned int schema = 0;
  service_metadata_config_t service;
  application_config_t application;
  recovery_config_t recovery;
  readiness_config_t readiness;
  health_check_config_t health;
  resource_limit_config_t resources;
  logging_config_t logging;
  security_config_t security;

  bool operator==(const service_config_t &) const = default;
};

struct config_validation_t {
  std::vector<std::wstring> errors;

  bool ok() const { return errors.empty(); }
  void add(std::wstring error) { errors.push_back(std::move(error)); }
};

bool parse_service_config(const char *toml_text, size_t length,
                          service_config_t *config,
                          config_validation_t *validation);
bool load_service_config(const wchar_t *path, service_config_t *config,
                         config_validation_t *validation);
bool load_secure_service_config(const wchar_t *path, service_config_t *config,
                                config_validation_t *validation);
#endif
