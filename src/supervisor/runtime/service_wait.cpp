#include "event.h"
#include "messages.h"
#include "service_wait.h"
#include "service_constants.h"
#include "service_status.h"

#include <string>

int await_single_handle(SERVICE_STATUS_HANDLE status_handle,
                        SERVICE_STATUS *status, HANDLE handle,
                        const wchar_t *name, const wchar_t *function_name,
                        unsigned long timeout,
                        SRWLOCK *status_lock) {
  const std::wstring function = std::wstring(function_name) + L"()";
  wchar_t timeout_milliseconds[16] = {};
  _snwprintf_s(timeout_milliseconds, _countof(timeout_milliseconds), _TRUNCATE,
               L"%lu", timeout);

  unsigned long waited = 0;
  while (waited < timeout) {
    unsigned long interval = timeout - waited;
    if (interval > kServiceStatusDeadlineMs) {
      interval = kServiceStatusDeadlineMs;
    }

    if (status) {
      if (status_lock) AcquireSRWLockExclusive(status_lock);
      status->dwWaitHint += interval;
      ++status->dwCheckPoint;
      const bool reported = report_service_status(
          status_handle, status, name, L"reporting wait progress");
      if (status_lock) ReleaseSRWLockExclusive(status_lock);
      if (! reported) return -1;
    }

    if (waited) {
      wchar_t waited_milliseconds[16] = {};
      wchar_t interval_milliseconds[16] = {};
      _snwprintf_s(waited_milliseconds, _countof(waited_milliseconds),
                   _TRUNCATE, L"%lu", waited);
      _snwprintf_s(interval_milliseconds, _countof(interval_milliseconds),
                   _TRUNCATE, L"%lu", interval);
      log_event(EVENTLOG_INFORMATION_TYPE, SVP_EVENT_AWAITING_SINGLE_HANDLE,
                function.c_str(), name, waited_milliseconds,
                interval_milliseconds, timeout_milliseconds, 0);
    }

    switch (WaitForSingleObject(handle, interval)) {
      case WAIT_OBJECT_0:
        return 0;
      case WAIT_TIMEOUT:
        waited += interval;
        break;
      default:
        return -1;
    }
  }
  return 1;
}
