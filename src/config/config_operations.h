#ifndef SVP_CONFIG_OPERATIONS_H
#define SVP_CONFIG_OPERATIONS_H

#include <string>
#include <vector>

enum class config_operation_status_t {
  success,
  validation,
  security,
  permission,
  not_found,
  conflict,
  system,
};

struct config_operation_result_t {
  config_operation_status_t status = config_operation_status_t::success;
  unsigned long windows_error = 0;
  std::wstring message;
  std::vector<std::wstring> validation_errors;
  std::wstring service;
  std::wstring action;
  std::wstring detail;
  std::vector<std::wstring> changes;

  [[nodiscard]] bool ok() const {
    return status == config_operation_status_t::success;
  }
};

config_operation_result_t export_service_config(
    const std::wstring &service_name, const std::wstring &destination_path);
config_operation_result_t plan_service_config(
    const std::wstring &config_path, const std::wstring &wrapper_image_path);
config_operation_result_t apply_service_config(
    const std::wstring &config_path, const std::wstring &wrapper_image_path);

#endif
