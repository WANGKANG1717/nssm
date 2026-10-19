#include "config_security.h"

#include "config.h"
#include "security.h"
#include "string_utils.h"

bool audit_secure_path(const std::wstring &path, const wchar_t *field,
                       bool must_exist, config_validation_t *validation) {
  if (! validation || path.empty()) return false;
  std::wstring reason;
  if (check_secure_path(path, must_exist, &reason)) return true;
  validation->add(std::wstring(field) + L" " + reason);
  return false;
}

bool audit_service_paths(const std::wstring &config_path,
                         const service_config_t &config,
                         config_validation_t *validation) {
  bool secure = true;
  secure = audit_secure_path(config_path, L"configuration path", true,
                             validation) && secure;
  secure = audit_secure_path(config.application.path, L"Service.ExecStart",
                             true, validation) && secure;
  secure = audit_secure_path(config.application.working_directory,
                             L"Service.WorkingDirectory", true,
                             validation) && secure;
  if (! config.logging.stdout_path.empty()) {
    secure = audit_secure_path(config.logging.stdout_path,
                               L"Logging.Stdout.Path", false, validation) &&
             secure;
  }
  if (! config.logging.stderr_path.empty() &&
      ! strings_equal_ignore_case(config.logging.stderr_path.c_str(),
                                  config.logging.stdout_path.c_str())) {
    secure = audit_secure_path(config.logging.stderr_path,
                               L"Logging.Stderr.Path", false, validation) &&
             secure;
  }
  if (config.readiness.probe.type == health_check_type_t::command) {
    secure = audit_secure_path(config.readiness.probe.command_path,
                               L"Readiness.Command.ExecStart", true, validation) &&
             secure;
  }
  if (config.health.type == health_check_type_t::command) {
    secure = audit_secure_path(config.health.command_path,
                               L"Health.Command.ExecStart", true, validation) &&
             secure;
  }
  return secure;
}
