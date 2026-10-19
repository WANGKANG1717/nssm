#include "config_commands.h"

#include "config.h"
#include "config_operations.h"
#include "output.h"

namespace {

const wchar_t *error_code(config_operation_status_t status) {
  switch (status) {
    case config_operation_status_t::validation: return L"validation";
    case config_operation_status_t::security: return L"security";
    case config_operation_status_t::permission: return L"permission";
    case config_operation_status_t::not_found: return L"notFound";
    case config_operation_status_t::conflict: return L"conflict";
    case config_operation_status_t::system: return L"system";
    case config_operation_status_t::success: return L"";
  }
  return L"system";
}

cli_exit_code exit_code(config_operation_status_t status) {
  switch (status) {
    case config_operation_status_t::success: return cli_exit_code::success;
    case config_operation_status_t::validation: return cli_exit_code::validation;
    case config_operation_status_t::security: return cli_exit_code::security;
    case config_operation_status_t::permission: return cli_exit_code::permission;
    case config_operation_status_t::not_found:
    case config_operation_status_t::conflict: return cli_exit_code::conflict;
    case config_operation_status_t::system: return cli_exit_code::system;
  }
  return cli_exit_code::system;
}

int print_operation_result(bool json, const config_operation_result_t &result,
                           bool plan) {
  if (! result.ok()) {
    if (! result.validation_errors.empty()) {
      print_cli_validation_errors(json, result.validation_errors,
                                  error_code(result.status));
    }
    else {
      print_cli_error(json, error_code(result.status), result.message,
                      result.windows_error);
    }
    return static_cast<int>(exit_code(result.status));
  }

  if (plan) {
    print_cli_plan(json, result.service, result.action.c_str(), result.changes);
  }
  else {
    print_cli_success(json, result.action.c_str(), result.service,
                      result.detail);
  }
  return static_cast<int>(cli_exit_code::success);
}

}  // namespace

int validate_service_config_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 1 ||
      ! arguments.positional[0]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: svp validate [--json] <config.toml>");
    return static_cast<int>(cli_exit_code::usage);
  }

  service_config_t config;
  config_validation_t validation;
  if (! load_service_config(arguments.positional[0], &config, &validation)) {
    print_cli_validation_errors(arguments.json, validation.errors);
    return static_cast<int>(cli_exit_code::validation);
  }
  print_cli_success(arguments.json, L"validate", config.service.name,
                    L"Configuration is valid: " + config.service.name);
  return static_cast<int>(cli_exit_code::success);
}

int export_service_config_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 2 ||
      ! arguments.positional[0] || ! arguments.positional[1]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: svp export [--json] <service> <destination.toml>");
    return static_cast<int>(cli_exit_code::usage);
  }
  return print_operation_result(
      arguments.json,
      export_service_config(arguments.positional[0], arguments.positional[1]),
      false);
}

int plan_service_config_command(int argc, wchar_t **argv,
                                const wchar_t *wrapper_image_path) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 1 ||
      ! arguments.positional[0] || ! wrapper_image_path) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: svp plan [--json] <config.toml>");
    return static_cast<int>(cli_exit_code::usage);
  }
  return print_operation_result(
      arguments.json,
      plan_service_config(arguments.positional[0], wrapper_image_path), true);
}

int apply_service_config_command(int argc, wchar_t **argv,
                                 const wchar_t *wrapper_image_path) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 1 ||
      ! arguments.positional[0] || ! wrapper_image_path) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: svp apply [--json] <config.toml>");
    return static_cast<int>(cli_exit_code::usage);
  }
  return print_operation_result(
      arguments.json,
      apply_service_config(arguments.positional[0], wrapper_image_path), false);
}
