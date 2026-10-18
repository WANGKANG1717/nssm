#ifndef NSSM_CLI_OUTPUT_H
#define NSSM_CLI_OUTPUT_H

#include <cstdio>
#include <string>
#include <vector>

enum class cli_exit_code : int {
  success = 0,
  validation = 1,
  usage = 2,
  permission = 3,
  conflict = 4,
  system = 5,
  security = 6,
};

struct cli_arguments_t {
  bool json = false;
  std::vector<wchar_t *> positional;
};

cli_arguments_t parse_cli_arguments(int argc, wchar_t **argv);
void write_json_string(FILE *stream, const std::wstring &value);
void print_cli_error(bool json, const wchar_t *code,
                     const std::wstring &message,
                     unsigned long windows_error = 0);
void print_cli_validation_errors(bool json,
                                 const std::vector<std::wstring> &errors,
                                 const wchar_t *code = L"validation");
void print_cli_success(bool json, const wchar_t *operation,
                       const std::wstring &service,
                       const std::wstring &detail = L"");
void print_cli_plan(bool json, const std::wstring &service,
                    const wchar_t *action,
                    const std::vector<std::wstring> &changes);

#endif
