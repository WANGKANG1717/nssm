#include "event.h"
#include "messages.h"
#include "registry.h"
#include "win32_handle.h"

#include <windows.h>

namespace {
constexpr wchar_t kEventLogRegistryPath[] =
    L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\SvcPilot";
}

int create_messages(const wchar_t *executable_path) {
  unique_registry_key key;
  const long error = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE, kEventLogRegistryPath, 0, nullptr,
      REG_OPTION_NON_VOLATILE,
      KEY_WRITE, nullptr, key.put(), nullptr);
  if (error != ERROR_SUCCESS) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED,
              kEventLogRegistryPath,
              error_string(error), 0);
    return 2;
  }

  const wchar_t *path = executable_path;
  if (! path || ! *path) return 3;
  const unsigned long bytes =
      static_cast<unsigned long>((wcslen(path) + 1) * sizeof(wchar_t));
  long result = RegSetValueExW(
      key.get(), L"EventMessageFile", 0, REG_SZ,
      reinterpret_cast<const unsigned char *>(path), bytes);
  if (result != ERROR_SUCCESS) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED,
              L"EventMessageFile", error_string(result), 0);
    return 3;
  }
  const unsigned long types =
      EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
  result = RegSetValueExW(key.get(), L"TypesSupported", 0, REG_DWORD,
                          reinterpret_cast<const unsigned char *>(&types),
                          sizeof(types));
  if (result != ERROR_SUCCESS) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED,
              L"TypesSupported", error_string(result), 0);
    return 4;
  }
  return 0;
}
