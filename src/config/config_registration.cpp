#include "config_registration.h"

#include "config.h"
#include "win32_handle.h"

#include <windows.h>

#include <vector>

namespace {

constexpr wchar_t kConfigPathValue[] = L"ConfigPath";
constexpr wchar_t kFailureExitCodeValue[] = L"FailureExitCode";
constexpr wchar_t kFailureTimeValue[] = L"FailureTime";
constexpr wchar_t kFailureKindValue[] = L"FailureKind";
constexpr wchar_t kFailurePhaseValue[] = L"FailurePhase";
constexpr wchar_t kFailureWindowsErrorValue[] = L"FailureWindowsError";
constexpr wchar_t kFailureMessageValue[] = L"FailureMessage";
constexpr wchar_t kFailureRestartAttemptsValue[] = L"FailureRestartAttempts";

std::wstring registration_key(const std::wstring &service_name) {
  return L"SYSTEM\\CurrentControlSet\\Services\\" + service_name +
         L"\\Parameters\\SvcPilot";
}

bool open_registration_key(const std::wstring &service_name, REGSAM access,
                           unique_registry_key *key, unsigned long *error) {
  const long result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                    registration_key(service_name).c_str(), 0,
                                    access, key->put());
  if (error) *error = static_cast<unsigned long>(result);
  return result == ERROR_SUCCESS;
}

long write_string(HKEY key, const wchar_t *name, const std::wstring &value) {
  return RegSetValueExW(
      key, name, 0, REG_SZ,
      reinterpret_cast<const unsigned char *>(value.c_str()),
      static_cast<unsigned long>((value.size() + 1) * sizeof(wchar_t)));
}

long read_optional_string(HKEY key, const wchar_t *name, std::wstring *value) {
  unsigned long type = 0;
  unsigned long bytes = 0;
  long result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
  if (result == ERROR_FILE_NOT_FOUND) return result;
  if (result != ERROR_SUCCESS || type != REG_SZ || ! bytes ||
      bytes % sizeof(wchar_t) != 0) {
    return ERROR_INVALID_DATA;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  result = RegQueryValueExW(
      key, name, nullptr, &type,
      reinterpret_cast<unsigned char *>(buffer.data()), &bytes);
  if (result != ERROR_SUCCESS || type != REG_SZ || ! bytes ||
      bytes % sizeof(wchar_t) != 0) {
    return result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result;
  }
  buffer.back() = L'\0';
  *value = buffer.data();
  return ERROR_SUCCESS;
}

long read_optional_dword(HKEY key, const wchar_t *name,
                         unsigned long *value) {
  unsigned long type = 0;
  unsigned long bytes = sizeof(*value);
  const long result = RegQueryValueExW(
      key, name, nullptr, &type, reinterpret_cast<unsigned char *>(value),
      &bytes);
  if (result == ERROR_SUCCESS &&
      (type != REG_DWORD || bytes != sizeof(*value))) {
    return ERROR_INVALID_DATA;
  }
  return result;
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

bool write_persistent_failure(const std::wstring &service_name,
                              const failure_record_t &failure,
                              unsigned long *error) {
  unique_registry_key key;
  if (! open_registration_key(service_name, KEY_SET_VALUE, &key, error)) {
    return false;
  }
  /* Remove the commit marker before replacing any part of the record. */
  long result = RegDeleteValueW(key.get(), kFailureExitCodeValue);
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    if (error) *error = static_cast<unsigned long>(result);
    return false;
  }
  unsigned long long timestamp_value = failure.timestamp_100ns;
  if (! timestamp_value) {
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER timestamp = {};
    timestamp.LowPart = now.dwLowDateTime;
    timestamp.HighPart = now.dwHighDateTime;
    timestamp_value = timestamp.QuadPart;
  }
  const std::wstring kind = failure_kind_text(failure.kind);
  result = write_string(key.get(), kFailureKindValue, kind);
  if (result == ERROR_SUCCESS) {
    result = write_string(key.get(), kFailurePhaseValue, failure.phase);
  }
  if (result == ERROR_SUCCESS) {
    result = write_string(key.get(), kFailureMessageValue, failure.message);
  }
  if (result == ERROR_SUCCESS) {
    result = RegSetValueExW(
        key.get(), kFailureWindowsErrorValue, 0, REG_DWORD,
        reinterpret_cast<const unsigned char *>(&failure.windows_error),
        sizeof(failure.windows_error));
  }
  if (result == ERROR_SUCCESS) {
    result = RegSetValueExW(
        key.get(), kFailureRestartAttemptsValue, 0, REG_DWORD,
        reinterpret_cast<const unsigned char *>(&failure.restart_attempts),
        sizeof(failure.restart_attempts));
  }
  if (result == ERROR_SUCCESS) result = RegSetValueExW(
      key.get(), kFailureTimeValue, 0, REG_QWORD,
      reinterpret_cast<const unsigned char *>(&timestamp_value),
      sizeof(timestamp_value));
  if (result == ERROR_SUCCESS) {
    /* Commit marker written last so readers never observe a partial record. */
    result = RegSetValueExW(
        key.get(), kFailureExitCodeValue, 0, REG_DWORD,
        reinterpret_cast<const unsigned char *>(&failure.exit_code),
        sizeof(failure.exit_code));
  }
  if (error) *error = static_cast<unsigned long>(result);
  return result == ERROR_SUCCESS;
}

bool read_persistent_failure(const std::wstring &service_name,
                             persistent_failure_t *failure,
                             unsigned long *error) {
  if (! failure) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  *failure = {};
  unique_registry_key key;
  if (! open_registration_key(service_name, KEY_QUERY_VALUE, &key, error)) {
    if (error && *error == ERROR_FILE_NOT_FOUND) {
      *error = ERROR_SUCCESS;
      return true;
    }
    return false;
  }
  unsigned long type = 0;
  unsigned long bytes = sizeof(failure->record.exit_code);
  long result = RegQueryValueExW(
      key.get(), kFailureExitCodeValue, nullptr, &type,
      reinterpret_cast<unsigned char *>(&failure->record.exit_code), &bytes);
  if (result == ERROR_FILE_NOT_FOUND) {
    if (error) *error = ERROR_SUCCESS;
    return true;
  }
  if (result != ERROR_SUCCESS || type != REG_DWORD ||
      bytes != sizeof(failure->record.exit_code)) {
    if (error) *error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result;
    return false;
  }
  type = 0;
  bytes = sizeof(failure->record.timestamp_100ns);
  result = RegQueryValueExW(
      key.get(), kFailureTimeValue, nullptr, &type,
      reinterpret_cast<unsigned char *>(&failure->record.timestamp_100ns), &bytes);
  if (result != ERROR_SUCCESS || type != REG_QWORD ||
      bytes != sizeof(failure->record.timestamp_100ns)) {
    if (error) *error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result;
    return false;
  }
  std::wstring kind;
  result = read_optional_string(key.get(), kFailureKindValue, &kind);
  if (result == ERROR_FILE_NOT_FOUND) {
    failure->record.kind = failure_kind_t::process_exit;
  }
  else if (result != ERROR_SUCCESS) {
    if (error) *error = result;
    return false;
  }
  else failure->record.kind = parse_failure_kind(kind);

  result = read_optional_string(key.get(), kFailurePhaseValue,
                                &failure->record.phase);
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    if (error) *error = result;
    return false;
  }
  result = read_optional_string(key.get(), kFailureMessageValue,
                                &failure->record.message);
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    if (error) *error = result;
    return false;
  }
  result = read_optional_dword(key.get(), kFailureWindowsErrorValue,
                               &failure->record.windows_error);
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    if (error) *error = result;
    return false;
  }
  result = read_optional_dword(key.get(), kFailureRestartAttemptsValue,
                               &failure->record.restart_attempts);
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    if (error) *error = result;
    return false;
  }
  failure->failed = true;
  if (error) *error = ERROR_SUCCESS;
  return true;
}

bool clear_persistent_failure(const std::wstring &service_name,
                              unsigned long *error) {
  unique_registry_key key;
  if (! open_registration_key(service_name, KEY_SET_VALUE, &key, error)) {
    return false;
  }
  const wchar_t *values[] = {
      kFailureExitCodeValue, kFailureTimeValue, kFailureKindValue,
      kFailurePhaseValue, kFailureWindowsErrorValue, kFailureMessageValue,
      kFailureRestartAttemptsValue};
  long result = ERROR_SUCCESS;
  for (const wchar_t *value : values) {
    long current = RegDeleteValueW(key.get(), value);
    if (current == ERROR_FILE_NOT_FOUND) current = ERROR_SUCCESS;
    if (result == ERROR_SUCCESS && current != ERROR_SUCCESS) result = current;
  }
  if (error) *error = static_cast<unsigned long>(result);
  return result == ERROR_SUCCESS;
}
