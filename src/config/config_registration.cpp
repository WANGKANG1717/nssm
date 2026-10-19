#include "config_registration.h"

#include "config.h"
#include "win32_handle.h"

#include <windows.h>

#include <vector>

namespace {

constexpr wchar_t kConfigPathValue[] = L"ConfigPath";

std::wstring registration_key(const std::wstring &service_name) {
  return L"SYSTEM\\CurrentControlSet\\Services\\" + service_name +
         L"\\Parameters\\SvcPilot";
}

}  // namespace

bool full_config_path(const wchar_t *path, std::wstring *full,
                      config_validation_t *validation) {
  const unsigned long needed = GetFullPathNameW(path, 0, nullptr, nullptr);
  if (! needed) {
    validation->add(L"Unable to resolve the configuration path");
    return false;
  }
  std::vector<wchar_t> buffer(needed);
  const unsigned long written =
      GetFullPathNameW(path, needed, buffer.data(), nullptr);
  if (! written || written >= needed) {
    validation->add(L"Unable to resolve the configuration path");
    return false;
  }
  *full = buffer.data();
  return true;
}

bool write_config_path(const std::wstring &service_name,
                       const std::wstring &path, unsigned long *error) {
  unique_registry_key key;
  unsigned long disposition = 0;
  long result = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE, registration_key(service_name).c_str(), 0, nullptr, 0,
      KEY_SET_VALUE, nullptr, key.put(), &disposition);
  if (result != ERROR_SUCCESS) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }
  result = RegSetValueExW(
      key.get(), kConfigPathValue, 0, REG_SZ,
      reinterpret_cast<const unsigned char *>(path.c_str()),
      static_cast<unsigned long>((path.size() + 1) * sizeof(wchar_t)));
  if (error) *error = static_cast<unsigned long>(result);
  return result == ERROR_SUCCESS;
}

bool read_config_path(const std::wstring &service_name, std::wstring *path,
                      unsigned long *error) {
  unique_registry_key key;
  long result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              registration_key(service_name).c_str(), 0,
                              KEY_QUERY_VALUE, key.put());
  if (result != ERROR_SUCCESS) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }

  unsigned long type = 0;
  unsigned long bytes = 0;
  result = RegQueryValueExW(key.get(), kConfigPathValue, nullptr, &type,
                            nullptr, &bytes);
  constexpr size_t maximum_bytes =
      (kMaximumPathCharacters + 1) * sizeof(wchar_t);
  if (result != ERROR_SUCCESS || type != REG_SZ ||
      bytes < 2 * sizeof(wchar_t) || bytes > maximum_bytes ||
      bytes % sizeof(wchar_t) != 0) {
    if (error) {
      *error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                       : static_cast<unsigned long>(result);
    }
    return false;
  }

  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  result = RegQueryValueExW(
      key.get(), kConfigPathValue, nullptr, &type,
      reinterpret_cast<unsigned char *>(buffer.data()), &bytes);
  const size_t characters = bytes / sizeof(wchar_t);
  if (result != ERROR_SUCCESS || type != REG_SZ ||
      bytes < 2 * sizeof(wchar_t) || bytes > maximum_bytes ||
      bytes % sizeof(wchar_t) != 0 || characters >= buffer.size() ||
      buffer[characters - 1] != L'\0' ||
      wcsnlen_s(buffer.data(), characters) != characters - 1) {
    if (error) {
      *error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                       : static_cast<unsigned long>(result);
    }
    return false;
  }
  *path = buffer.data();
  if (error) *error = ERROR_SUCCESS;
  return true;
}

bool registered_service_config_path(const wchar_t *service_name,
                                    std::wstring *path,
                                    unsigned long *error) {
  if (! service_name || ! service_name[0] || ! path) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  return read_config_path(service_name, path, error);
}
