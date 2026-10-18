#include "config.h"
#include "output.h"
#include "win32_handle.h"

#include <windows.h>

#include <string>
#include <vector>

namespace {

constexpr unsigned long long kMaximumConfigBytes = 4ULL * 1024ULL * 1024ULL;

std::wstring read_error_message(unsigned long error) {
  return L"Unable to read configuration file (Windows error " +
         std::to_wstring(error) + L")";
}

}  // namespace

bool load_service_config(const wchar_t *path, service_config_t *config,
                         config_validation_t *validation) {
  if (! config || ! validation) return false;
  validation->errors.clear();
  if (! path || ! *path) {
    validation->add(L"Configuration path must not be empty");
    return false;
  }

  unique_handle file(CreateFileW(
      path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (! file) {
    validation->add(read_error_message(GetLastError()));
    return false;
  }

  LARGE_INTEGER size = {};
  if (! GetFileSizeEx(file.get(), &size)) {
    validation->add(read_error_message(GetLastError()));
    return false;
  }
  if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) >
                                kMaximumConfigBytes) {
    validation->add(L"Configuration file must be between 1 byte and 4 MiB");
    return false;
  }

  std::vector<char> content(static_cast<size_t>(size.QuadPart));
  size_t offset = 0;
  while (offset < content.size()) {
    const size_t remaining = content.size() - offset;
    const unsigned long chunk = static_cast<unsigned long>(
        remaining > 1024 * 1024 ? 1024 * 1024 : remaining);
    unsigned long read = 0;
    if (! ReadFile(file.get(), content.data() + offset, chunk, &read, nullptr) ||
        ! read) {
      validation->add(read_error_message(GetLastError()));
      return false;
    }
    offset += read;
  }

  const size_t start =
      content.size() >= 3 &&
              static_cast<unsigned char>(content[0]) == 0xEF &&
              static_cast<unsigned char>(content[1]) == 0xBB &&
              static_cast<unsigned char>(content[2]) == 0xBF
          ? 3
          : 0;
  return parse_service_config(content.data() + start, content.size() - start,
                              config, validation);
}

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
