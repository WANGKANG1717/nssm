#include "config.h"
#include "security.h"
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

static bool load_service_config_impl(const wchar_t *path, bool require_secure,
                                     service_config_t *config,
                                     config_validation_t *validation,
                                     bool *security_failure) {
  if (! config || ! validation) return false;
  if (security_failure) *security_failure = false;
  validation->errors.clear();
  if (! path || ! *path) {
    validation->add(L"Configuration path must not be empty");
    return false;
  }

  if (require_secure) {
    std::wstring reason;
    if (! check_secure_path(path, true, &reason)) {
      if (security_failure) *security_failure = true;
      validation->add(L"Configuration path " + reason);
      return false;
    }
  }

  unique_handle file(CreateFileW(
      path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (! file) {
    validation->add(read_error_message(GetLastError()));
    return false;
  }
  if (require_secure) {
    std::wstring reason;
    if (! check_secure_file_handle(file.get(), &reason)) {
      if (security_failure) *security_failure = true;
      validation->add(L"Configuration file is insecure: " + reason);
      return false;
    }
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
    if (! ReadFile(file.get(), content.data() + offset, chunk, &read,
                   nullptr)) {
      validation->add(read_error_message(GetLastError()));
      return false;
    }
    if (! read) {
      validation->add(read_error_message(ERROR_HANDLE_EOF));
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

bool load_service_config(const wchar_t *path, service_config_t *config,
                         config_validation_t *validation) {
  return load_service_config_impl(path, false, config, validation, nullptr);
}

bool load_secure_service_config(const wchar_t *path, service_config_t *config,
                                config_validation_t *validation,
                                bool *security_failure) {
  return load_service_config_impl(path, true, config, validation,
                                  security_failure);
}
