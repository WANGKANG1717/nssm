#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <utility>
#include <vector>

struct nssm_service_t;

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
};

struct service_account_config_t {
  service_account_type_t type = service_account_type_t::system;
  std::wstring name;
};

struct service_metadata_config_t {
  std::wstring name;
  std::wstring display_name;
  std::wstring description;
  service_startup_t startup = service_startup_t::automatic;
  service_account_config_t account;
  std::vector<std::wstring> dependencies;
};

struct application_config_t {
  std::wstring path;
  std::vector<std::wstring> arguments;
  std::wstring working_directory;
  bool inherit_environment = true;
  std::vector<std::pair<std::wstring, std::wstring>> environment;
};

struct service_config_t {
  unsigned int schema = 0;
  service_metadata_config_t service;
  application_config_t application;
  recovery_config_t recovery;
};

struct config_validation_t {
  std::vector<std::wstring> errors;

  bool ok() const { return errors.empty(); }
  void add(std::wstring error) { errors.push_back(std::move(error)); }
};

bool parse_service_config(const char *json, size_t length,
                          service_config_t *config,
                          config_validation_t *validation);
bool load_service_config(const wchar_t *path, service_config_t *config,
                         config_validation_t *validation);
int validate_service_config_command(int argc, wchar_t **argv);
int apply_service_config_command(int argc, wchar_t **argv);
bool load_registered_service_config(nssm_service_t *service,
                                    config_validation_t *validation);

#endif
