#ifndef NSSM_CLI_CONFIG_COMMANDS_H
#define NSSM_CLI_CONFIG_COMMANDS_H

int validate_service_config_command(int argc, wchar_t **argv);
int apply_service_config_command(int argc, wchar_t **argv);
int plan_service_config_command(int argc, wchar_t **argv);
int export_service_config_command(int argc, wchar_t **argv);

#endif
