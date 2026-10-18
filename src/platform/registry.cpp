#include "nssm.h"
#include "event.h"
#include "messages.h"
#include "registry.h"
#include "win32_handle.h"

namespace {
constexpr size_t kRegistryPathLength = 255;
}

int create_messages() {
  unique_registry_key key;
  TCHAR registry[kRegistryPathLength];
  if (_sntprintf_s(
          registry, _countof(registry), _TRUNCATE,
          _T("SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s"),
          NSSM) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
              _T("eventlog registry"), _T("create_messages()"), 0);
    return 1;
  }

  const long error = RegCreateKeyEx(
      HKEY_LOCAL_MACHINE, registry, 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_WRITE, nullptr, key.put(), nullptr);
  if (error != ERROR_SUCCESS) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, registry,
              error_string(error), 0);
    return 2;
  }

  const TCHAR *path = nssm_unquoted_imagepath();
  const unsigned long bytes =
      static_cast<unsigned long>((_tcslen(path) + 1) * sizeof(TCHAR));
  RegSetValueEx(key.get(), _T("EventMessageFile"), 0, REG_SZ,
                reinterpret_cast<const unsigned char *>(path), bytes);
  const unsigned long types =
      EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
  RegSetValueEx(key.get(), _T("TypesSupported"), 0, REG_DWORD,
                reinterpret_cast<const unsigned char *>(&types),
                sizeof(types));
  return 0;
}
