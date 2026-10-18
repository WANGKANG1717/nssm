#include "env.h"
#include "event.h"
#include "messages.h"

#include <windows.h>

#include <cstring>

namespace {

size_t environment_length(const wchar_t *environment) {
  if (! environment) return 0;
  const wchar_t *cursor = environment;
  while (*cursor || *(cursor + 1)) ++cursor;
  return static_cast<size_t>(cursor - environment) + 2;
}

wchar_t *copy_environment_block(const wchar_t *environment) {
  const size_t length = environment_length(environment);
  if (! length) return nullptr;
  auto *copy = static_cast<wchar_t *>(
      HeapAlloc(GetProcessHeap(), 0, length * sizeof(wchar_t)));
  if (! copy) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
              L"environment", L"copy_environment_block()", 0);
    return nullptr;
  }
  memcpy(copy, environment, length * sizeof(wchar_t));
  return copy;
}

}  // namespace

wchar_t *copy_environment() {
  wchar_t *environment = GetEnvironmentStringsW();
  if (! environment) return nullptr;
  wchar_t *copy = copy_environment_block(environment);
  const unsigned long error = copy ? ERROR_SUCCESS : GetLastError();
  FreeEnvironmentStringsW(environment);
  SetLastError(error);
  return copy;
}
