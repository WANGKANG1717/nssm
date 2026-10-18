#include "nssm.h"
#include "env.h"
#include "event.h"
#include "messages.h"

namespace {

size_t environment_length(const TCHAR *environment) {
  if (! environment) return 0;
  const TCHAR *cursor = environment;
  while (*cursor || *(cursor + 1)) ++cursor;
  return static_cast<size_t>(cursor - environment) + 2;
}

TCHAR *copy_environment_block(const TCHAR *environment) {
  const size_t length = environment_length(environment);
  if (! length) return nullptr;
  auto *copy = static_cast<TCHAR *>(
      HeapAlloc(GetProcessHeap(), 0, length * sizeof(TCHAR)));
  if (! copy) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY,
              _T("environment"), _T("copy_environment_block()"), 0);
    return nullptr;
  }
  memcpy(copy, environment, length * sizeof(TCHAR));
  return copy;
}

}  // namespace

TCHAR *copy_environment() {
  TCHAR *environment = GetEnvironmentStrings();
  if (! environment) return nullptr;
  TCHAR *copy = copy_environment_block(environment);
  FreeEnvironmentStrings(environment);
  return copy;
}
