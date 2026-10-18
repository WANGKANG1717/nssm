#include "config_commands.h"

#include "config.h"
#include "output.h"

int validate_service_config_command(int argc, wchar_t **argv) {
  cli_arguments_t arguments = parse_cli_arguments(argc, argv);
  if (arguments.verbose || arguments.positional.size() != 1 ||
      ! arguments.positional[0]) {
    print_cli_error(arguments.json, L"usage",
                    L"Usage: nssm validate [--json] <config.json>");
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
