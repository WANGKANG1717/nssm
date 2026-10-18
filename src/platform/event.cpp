#include "event.h"
#include "win32_handle.h"

#include <cstdarg>
#include <vector>

namespace {

constexpr wchar_t kEventSource[] = L"nssm";
constexpr size_t kErrorBufferSize = 65535;

}  // namespace

wchar_t *error_string(unsigned long error) {
  thread_local std::vector<wchar_t> error_buffer(kErrorBufferSize);
  wchar_t *error_message = error_buffer.data();

  if (! FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, GetUserDefaultLangID(), error_message,
                       kErrorBufferSize, nullptr) &&
      ! FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, error_message, kErrorBufferSize,
                       nullptr) &&
      _snwprintf_s(error_message, kErrorBufferSize, _TRUNCATE,
                   L"system error %lu", error) < 0) {
    return nullptr;
  }
  return error_message;
}

wchar_t *message_string(unsigned long error) {
  wchar_t *message = nullptr;
  HMODULE module = GetModuleHandleW(nullptr);
  const unsigned long flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                              FORMAT_MESSAGE_FROM_HMODULE |
                              FORMAT_MESSAGE_IGNORE_INSERTS;
  if (! FormatMessageW(flags, module, error, GetUserDefaultLangID(),
                       reinterpret_cast<wchar_t *>(&message),
                       kErrorBufferSize, nullptr) &&
      ! FormatMessageW(flags, module, error, 0,
                       reinterpret_cast<wchar_t *>(&message),
                       kErrorBufferSize, nullptr)) {
    message = static_cast<wchar_t *>(
        LocalAlloc(LPTR, kErrorBufferSize * sizeof(wchar_t)));
    if (! message) return nullptr;
    if (_snwprintf_s(message, kErrorBufferSize, _TRUNCATE,
                     L"system error %lu", error) < 0) {
      LocalFree(message);
      return nullptr;
    }
  }
  return message;
}

void log_event_strings(unsigned short type, unsigned long id,
                       const wchar_t *const *strings, size_t count) {
  unique_event_source handle(RegisterEventSourceW(nullptr, kEventSource));
  if (! handle) return;
  ReportEventW(handle.get(), type, 0, id, nullptr,
               static_cast<unsigned short>(count), 0,
               const_cast<const wchar_t **>(strings), nullptr);
}

void print_message(FILE *file, unsigned long id, ...) {
  wchar_t *format = message_string(id);
  if (! format) return;

  va_list arguments;
  va_start(arguments, id);
  vfwprintf(file, format, arguments);
  va_end(arguments);
  free_message_string(format);
}
